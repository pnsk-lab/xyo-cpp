#!/usr/bin/env python3
"""Verify live, post-join variable reads in the Mega fast-var-pair lane."""
import argparse
import copy
from pathlib import Path
import tempfile

from test_mega_real_pingpong_full import rewrite, run

def check(fixture, label, directory, checksum):
    fiber,_,_=run(fixture,directory,False)
    mega,installed,_=run(fixture,directory,True)
    assert installed,(label,mega)
    for key in ("b_chk","b_round","b_chunks","sc_hits","output"):
        if key == "output":
            # Wall-clock reporters in bench_end are deliberately not
            # deterministic across independent runs.
            left=fiber[key][0].split(" | ")
            right=mega[key][0].split(" | ")
            assert len(left)==len(right)==6,(label,left,right)
            for i in (0,2,5):
                assert left[i]==right[i],(label,i,left,right)
        else:
            assert fiber[key]==mega[key],(label,key,fiber[key],mega[key])
    assert int(float(mega["b_chk"])) == checksum,(label,mega)
    print("PASS",label,flush=True)

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    args=parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="mega-live-pair-") as tmp:
        directory=Path(tmp)
        project=rewrite(args.source, budget=3, fixed_hits=3)
        project["targets"][0]["variables"]["b_budget"]=["b_budget","3"]
        check(project,"numeric-string budget",directory,3)

        project=rewrite(args.source, budget=3, fixed_hits=3)
        project["targets"][0]["variables"]["b_budget"]=["b_budget",2.5]
        check(project,"fractional budget",directory,3)

        project=rewrite(args.source, budget=3, fixed_hits=3)
        stage=project["targets"][0]
        blocks=stage["blocks"]
        hub=next(t for t in project["targets"] if t["name"]=="Hub")
        # Force the receiver to change the live RHS immediately after
        # its counter increment. The fast sender must not cache the
        # original budget or hoist its condition across a joined send.
        receiver=hub["blocks"]["~x"]
        assert receiver["opcode"]=="data_changevariableby"
        receiver["next"]="test-budget-change"
        hub["blocks"]["test-budget-change"]={
            "opcode":"data_setvariableto","next":None,"parent":"~x",
            "inputs":{"VALUE":[1,[4,"1"]]},
            "fields":{"VARIABLE":["b_budget","b_budget"]},
        }
        check(project,"receiver changes RHS after join",directory,1)

if __name__=="__main__":
    main()
