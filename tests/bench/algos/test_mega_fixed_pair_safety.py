#!/usr/bin/env python3
"""Negative source-proof tests for the opt-in fixed-work Mega until pair."""
import argparse
import copy
from pathlib import Path
import tempfile
from test_mega_real_pingpong_full import rewrite, run

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    args=parser.parse_args()
    original=rewrite(args.source, budget=3, fixed_hits=3)
    cases={}
    changed=copy.deepcopy(original)
    node=changed["targets"][0]["blocks"]["~TY"]
    node["inputs"]["OPERAND1"], node["inputs"]["OPERAND2"]=(
        node["inputs"]["OPERAND2"],node["inputs"]["OPERAND1"])
    cases["reversed-variables"]=changed

    changed=copy.deepcopy(original)
    changed["targets"][0]["blocks"]["~TY"]["opcode"]="operator_gt"
    cases["changed-comparator"]=changed

    changed=copy.deepcopy(original)
    changed["targets"][0]["blocks"]["~TY"]["inputs"]["OPERAND1"]=[
        1,[12,"sc_hits","sc_hits"]]
    cases["shadow-variable"]=changed

    changed=copy.deepcopy(original)
    changed["targets"][0]["blocks"]["~TT"]["fields"]["VARIABLE"]=[
        "b_n","b_n"]
    cases["mismatched-reset-target"]=changed

    with tempfile.TemporaryDirectory(prefix="mega-var-pair-proof-") as tmp:
        directory=Path(tmp)
        for name, project in cases.items():
            fiber,_,_=run(project,directory,False)
            mega,installed,_=run(project,directory,True)
            assert not installed,name
            for field in ("output","b_chk","b_round","b_chunks","sc_hits"):
                assert fiber[field]==mega[field],(
                    name,field,fiber[field],mega[field])
            print("PASS rejected altered source:",name,flush=True)

if __name__=="__main__":
    main()
