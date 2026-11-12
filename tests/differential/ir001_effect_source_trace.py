#!/usr/bin/env python3
"""IR-001 live LLVM event_broadcast source-PC diagnostic and parity."""
import argparse
import json
import os
from pathlib import Path
import re
import sys
import zipfile

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/"tests/differential"))
from runtime_broadcast_journal import fixture,execute  # noqa: E402

LINE=re.compile(
    r"\[ir001-effect-source\] kind=broadcast pc=(\d+) block=(\S+) "
    r"revision=(\d+) digest=([0-9a-f]+):([0-9a-f]+) "
    r"revision_matches=(\d) journal=(\d) published_delta=(\d+) "
    r"delivered_delta=(\d+) immediate_delta=(\d+) slice_delta=(\d+) "
    r"pending=(\d+) rooted=(\d+) exact_delivery=(\d) "
    r"event_published=(\d+) event_delivered=(\d+) event_slice=(\d+)"
)

CHECKPOINT=re.compile(
    r"\[ir001-effect-checkpoint\] pc=(\d+) block=(\S+) phase=(\d+) "
    r"captured=(\d) seq=(\d+) effect_pc=(\d+) slice=(\d+) "
    r"source_owner=(\d) executable=(\d) vm_write_pending=(\d)"
)

def expected_pcs(path, opcode="event_broadcast"):
    with zipfile.ZipFile(path) as archive:
        targets=json.loads(archive.read("project.json"))["targets"]
        blocks={key: val for target in targets
                for key, val in target["blocks"].items()}
    expected={}
    for root,obj in blocks.items():
        if not obj.get("topLevel"):
            continue
        work=[root]; visited=set()
        while work:
            ident=work.pop()
            if ident in visited or ident not in blocks:
                continue
            visited.add(ident)
            node=blocks[ident]
            if isinstance(node.get("next"),str):
                work.append(node["next"])
            for inp in node.get("inputs",{}).values():
                if isinstance(inp,list):
                    work.extend(x for x in inp[1:]
                                if isinstance(x,str) and x in blocks)
        for i,ident in enumerate(sorted(visited),1):
            if blocks[ident]["opcode"]==opcode:
                assert ident not in expected
                expected[ident]=i
    return expected

def repeat_fixture(path, *, extra_say=False, extra_vm_write=False,
                   extra_list_write=False, optimized_foreach=False):
    from gen_algo_bench import (
        Gen, S_set, S_change, S_repeat, num, var,
        make_project, write_sb3
    )
    lists=["history"] if extra_list_write else []
    scalars=["x","done","acc","idx"] if optimized_foreach else ["x","done"]
    g=Gen(scalars,lists)
    g.script([S_set("x",num(0)),
              S_repeat(num(3),[S_change("x",num(1))]),
              S_set("done",num(1))])
    repeat=next(k for k,v in g.blocks.items()
                if v["opcode"]=="control_repeat")
    body=g.blocks[repeat]["inputs"]["SUBSTACK"][1]
    assert isinstance(body,str)
    broadcast=g.new("event_broadcast",parent=body)
    g.blocks[body]["next"]=broadcast
    g.blocks[broadcast]["inputs"]["BROADCAST_INPUT"]=[
        1,[11,"go","id_go"]]
    if extra_vm_write:
        # Ordinary LLVM data_changevariableby after a broadcast is NOT
        # a CommitOnceEffects event, so the VM-write fence must reject
        # the source checkpoint even with an unchanged host journal.
        mutation=g.new("data_changevariableby", parent=broadcast,
                       fields={"VARIABLE":g.field("VARIABLE","x")})
        g.blocks[broadcast]["next"]=mutation
        g.blocks[mutation]["inputs"]["VALUE"]=[1,[4,"2"]]
    if extra_list_write:
        list_mutation=g.new("data_addtolist",parent=broadcast,
                            fields={"LIST":g.field("LIST","history")})
        g.blocks[broadcast]["next"]=list_mutation
        g.blocks[list_mutation]["inputs"]["ITEM"]=[1,[10,"payload"]]
    if optimized_foreach:
        # Fast-path for-each sum writes directly to promoted variable
        # storage. It must be fenced even though the body is eliminated
        # from ordinary emitStackBlock once SSA reduction is selected.
        foreach=g.new("control_for_each",parent=broadcast,
                      fields={"VARIABLE":g.field("VARIABLE","idx")})
        g.blocks[broadcast]["next"]=foreach
        g.blocks[foreach]["inputs"]["VALUE"]=[1,[4,"3"]]
        mutation=g.emit_stmt(S_change("acc",var("idx")),foreach)
        g.blocks[foreach]["inputs"]["SUBSTACK"]=[2,mutation]
    if extra_say:
        # Compiled looks_say is intentionally NOT instrumented by IR-001;
        # it still publishes a real journal event after the broadcast.
        say=g.new("looks_say", parent=broadcast)
        g.blocks[broadcast]["next"]=say
        g.blocks[say]["inputs"]["MESSAGE"]=[1,[10,"untraced"]]
    receiver=g.script([S_change("x",num(10))],x=300)
    g.blocks[receiver]["opcode"]="event_whenbroadcastreceived"
    g.blocks[receiver]["fields"]["BROADCAST_OPTION"]=["go","id_go"]
    project=make_project(scalars,lists,g)
    project["targets"][1]["broadcasts"]["id_go"]="go"
    path.parent.mkdir(parents=True,exist_ok=True)
    write_sb3(str(path),project)
    return path


def run(binary,project,journal,trace):
    prior=os.environ.get("XYO_IR001_TRACE_EFFECT_PC")
    try:
        if trace: os.environ["XYO_IR001_TRACE_EFFECT_PC"]="1"
        else: os.environ.pop("XYO_IR001_TRACE_EFFECT_PC",None)
        return execute(binary,project,"eager",journal)
    finally:
        if prior is None: os.environ.pop("XYO_IR001_TRACE_EFFECT_PC",None)
        else: os.environ["XYO_IR001_TRACE_EFFECT_PC"]=prior

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--native",type=Path,required=True)
    ap.add_argument("--out",type=Path,required=True)
    a=ap.parse_args()
    project=fixture(a.out/"ir001-broadcast-site.sb3")
    pcs=expected_pcs(project)
    assert pcs
    baseline,baseline_stats,base_log=run(a.native,project,True,False)
    assert "[ir001-effect-source]" not in base_log
    traced,trace_stats,trace_log=run(a.native,project,True,True)
    assert traced==baseline and trace_stats==baseline_stats
    records=LINE.findall(trace_log)
    assert records,trace_log[-3000:]
    observed=set()
    for row in records:
        pc,block,rev,lo,hi,valid,journal,published,delivered,immediate,slices,pending,roots,exact,event_published,event_delivered,event_slice=row
        assert block in pcs and int(pc)==pcs[block],(block,pc,pcs)
        assert int(rev)>0 and (int(lo,16) or int(hi,16))
        assert valid=="1" and journal=="1"
        assert int(published)+int(immediate)>=1
        assert int(delivered)+int(immediate)>=1
        assert int(slices)>=1
        assert pending=="0" and roots=="0" 
        assert (exact,event_published,event_delivered)==("1","1","1")
        assert int(event_slice)>0
        observed.add(block)
    assert observed==set(pcs),(observed,pcs)
    unjournaled,_,log=run(a.native,project,False,True)
    assert unjournaled==baseline
    records_off=LINE.findall(log)
    assert records_off and all(row[6]=="0" and row[7]=="0" for row in records_off)
    # Repeated visits to one immutable source PC are distinct dynamic
    # effects; do not mistake a repeated block for a duplicate site.
    repeated=repeat_fixture(a.out/"ir001-broadcast-loop.sb3")
    repeated_pcs=expected_pcs(repeated)
    assert len(repeated_pcs)==1,repeated_pcs
    loop_base,loop_stats,_=run(a.native,repeated,True,False)
    loop_state,traced_stats,loop_log=run(a.native,repeated,True,True)
    assert loop_state==loop_base and traced_stats==loop_stats
    assert loop_base.get("x")=="33" and loop_base.get("done")=="1",loop_base
    assert loop_stats[0]==3,loop_stats
    loop_records=LINE.findall(loop_log)
    assert len(loop_records)==3,loop_log[-2500:]
    for row in loop_records:
        pc,block=row[0],row[1]
        assert int(pc)==repeated_pcs[block],(row,repeated_pcs)
        assert row[5]=="1" and row[6]=="1"
        assert row[11:13]==("0","0"),row
        assert row[13:16]==("1","1","1"),row

    # The native CallFrame's EXACT SOURCE LOOP checkpoint now receives
    # the owned effect timeline only after real journal dispatch. The
    # same source PC is revisited with monotonically increasing slices.
    saved={key:os.environ.get(key) for key in (
        "XYO_JIT_CALLFRAME","XYO_RT_ROOT_ONLY_DEOPT")}
    try:
        os.environ["XYO_JIT_CALLFRAME"]="1"
        os.environ["XYO_RT_ROOT_ONLY_DEOPT"]="1"
        loop_site=expected_pcs(repeated,"control_repeat")
        assert len(loop_site)==1,loop_site
        source_loop_pc=next(iter(loop_site.values()))
        state,stats,log=run(a.native,repeated,True,True)
        assert state==loop_base and stats==loop_stats
        checkpoints=CHECKPOINT.findall(log)
        assert len(checkpoints)==3,log[-2200:]
        for sequence,row in enumerate(checkpoints,1):
            pc,block,phase,captured,seq,effect_pc,slice_id,owner,exe,vm_write=row
            assert int(pc)==source_loop_pc and block in loop_site,row
            assert (phase,captured,seq,owner,exe)==(
                "2","1",str(sequence),"1","0"),row
            assert int(effect_pc)==next(iter(repeated_pcs.values())),row
            assert int(slice_id)==sequence and vm_write=="0",row

        # The same commands without a host journal still run normally,
        # but a delivery-based checkpoint must fail closed.
        unjournaled_state,_,journal_off=run(a.native,repeated,False,True)
        assert unjournaled_state==loop_base
        rejected=CHECKPOINT.findall(journal_off)
        assert len(rejected)==3,journal_off[-2500:]
        assert all(row[3]=="0" and row[8]=="0" for row in rejected)

        # A separate compiled, but UNINSTRUMENTED looks_say after each
        # broadcast must invalidate the source effect frontier. The
        # sequence remains diagnostic-only; the Scratch outcome is intact.
        contaminated=repeat_fixture(
            a.out/"ir001-broadcast-untraced-say.sb3",extra_say=True)
        untraced_base,untraced_base_stats,_=run(
            a.native,contaminated,True,False)
        untraced_state,untraced_stats,untraced_log=run(
            a.native,contaminated,True,True)
        assert untraced_state==untraced_base
        assert untraced_stats==untraced_base_stats
        assert untraced_state.get("x")=="33",untraced_state
        assert untraced_stats[0]==3,untraced_stats
        drift=CHECKPOINT.findall(untraced_log)
        assert len(drift)==3,untraced_log[-2700:]
        # The source/CallFrame is still valid: denial is specifically
        # caused by the uninstrumented host journal epoch changing.
        assert all(row[3]=="0" and row[7]=="1" and row[8]=="0"
                   for row in drift),drift
        # The first rejection is specifically an untraced looks_say
        # journal event. Later iterations may also observe unrelated
        # native variable writes after that first failed checkpoint.
        assert drift[0][9]=="0",drift
        assert "[ir001-effect-source] kind=broadcast" in untraced_log

        vm_writer=repeat_fixture(
            a.out/"ir001-broadcast-untraced-variable.sb3",
            extra_vm_write=True)
        vm_base,vm_base_stats,_=run(a.native,vm_writer,True,False)
        vm_state,vm_stats,vm_log=run(a.native,vm_writer,True,True)
        assert vm_state == vm_base and vm_stats == vm_base_stats
        assert vm_state.get("x") == "39",vm_state
        assert vm_stats[0] == 3,vm_stats
        blocked=CHECKPOINT.findall(vm_log)
        assert len(blocked)==3,vm_log[-2700:]
        assert all(row[3]=="0" and row[7]=="1" and row[8]=="0"
                   and row[9]=="1" for row in blocked),blocked
        # Unlike the looks_say case, the host journal did NOT change
        # after the broadcast; the LLVM VM-write fence alone denied it.
        observed=LINE.findall(vm_log)
        assert len(observed)==3 and all(row[13]=="1" for row in observed)

        list_writer=repeat_fixture(
            a.out/"ir001-broadcast-untraced-list.sb3",
            extra_list_write=True)
        list_baseline,list_base_stats,_=run(
            a.native,list_writer,True,False)
        list_state,list_stats,list_log=run(
            a.native,list_writer,True,True)
        assert list_state==list_baseline
        assert list_stats==list_base_stats
        assert list_state.get("x")=="33",list_state
        list_blocked=CHECKPOINT.findall(list_log)
        assert len(list_blocked)==3,list_log[-2400:]
        assert all(row[3]=="0" and row[7]=="1" and row[8]=="0"
                   and row[9]=="1" for row in list_blocked),list_blocked

        accelerated=repeat_fixture(
            a.out/"ir001-broadcast-nested-foreach-ssa.sb3",
            optimized_foreach=True)
        optimized_env=os.environ.get("XYO_JIT_FOREACH_SSA")
        previous_verbose=os.environ.get("XYO_JIT_VERBOSE")
        try:
            os.environ["XYO_JIT_FOREACH_SSA"]="1"
            os.environ["XYO_JIT_VERBOSE"]="1"
            fast_state,fast_stats,fast_log=run(
                a.native,accelerated,True,True)
            slow_state,slow_stats,_=run(
                a.native,accelerated,True,False)
            assert fast_state==slow_state and fast_stats==slow_stats
            assert fast_state.get("x")=="33",fast_state
            assert fast_state.get("acc")=="18",fast_state
            outer_pc=next(iter(expected_pcs(
                accelerated,"control_repeat").values()))
            outer_checkpoints=[
                row for row in CHECKPOINT.findall(fast_log)
                if int(row[0])==outer_pc
            ]
            assert len(outer_checkpoints)==3,fast_log[-3000:]
            assert all(row[3]=="0" and row[8]=="0" and row[9]=="1"
                       for row in outer_checkpoints),outer_checkpoints
            assert "xyo-jit: foreach SSA sum" in fast_log,(
                "expected accelerated for-each SSA lowering",fast_log[-2300:])
        finally:
            if optimized_env is None:
                os.environ.pop("XYO_JIT_FOREACH_SSA",None)
            else:
                os.environ["XYO_JIT_FOREACH_SSA"]=optimized_env
            if previous_verbose is None:
                os.environ.pop("XYO_JIT_VERBOSE",None)
            else:
                os.environ["XYO_JIT_VERBOSE"]=previous_verbose
    finally:
        for key,value in saved.items():
            if value is None: os.environ.pop(key,None)
            else: os.environ[key]=value

    print("PASS IR-001 live LLVM source PCs, journal on/off parity,",
          len(records),"observations",len(pcs),"distinct sites")

if __name__=="__main__":
    main()
