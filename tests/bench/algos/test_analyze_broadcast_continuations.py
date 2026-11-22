#!/usr/bin/env python3
"""Self-contained regression for the broadcast continuation source tracer."""
import unittest
from analyze_broadcast_continuations import (
    analyze_target, broadcast_name, caller_paths, lexical_chain,
    procedure_definitions,
)


def block(opcode, *, parent=None, next=None, top_level=False, inputs=None,
          fields=None, mutation=None):
    return {
        "opcode": opcode,
        "parent": parent,
        "next": next,
        "topLevel": top_level,
        "inputs": inputs or {},
        "fields": fields or {},
        **({"mutation": mutation} if mutation else {}),
    }


class BroadcastContinuationTraceTests(unittest.TestCase):
    def fixture(self):
        blocks = {
            "hat": block("event_whenflagclicked", top_level=True,
                         next="call"),
            "call": block("procedures_call", parent="hat", next="after",
                          mutation={"proccode": "suite"}),
            "after": block("data_changevariableby", parent="call"),
            "definition": block("procedures_definition", top_level=True,
                                next="outer",
                                inputs={"custom_block": [1, "prototype"]}),
            "prototype": block("procedures_prototype",
                               parent="definition",
                               mutation={"proccode": "suite"}),
            "outer": block("control_repeat", parent="definition",
                           inputs={"TIMES": [1, [4, "5"]],
                                   "SUBSTACK": [2, "inner"]}),
            "inner": block("control_repeat_until", parent="outer",
                           inputs={"CONDITION": [2, "condition"],
                                   "SUBSTACK": [2, "send"]}),
            "condition": block("operator_not", parent="inner",
                               inputs={"OPERAND": [2, "timer"]}),
            "timer": block("sensing_dayssince2000", parent="condition"),
            "send": block("event_broadcastandwait", parent="inner",
                          inputs={"BROADCAST_INPUT": [1, [11, "PING", "ping"]]}),
        }
        return {"name": "Stage", "blocks": blocks}

    def test_procedure_path_and_clock_dependent_loop(self):
        report, = analyze_target(self.fixture())
        self.assertEqual(report["message"], "PING")
        self.assertEqual(
            report["procedure_path"], ["event_whenflagclicked", "suite"]
        )
        self.assertEqual(
            [item["opcode"] for item in report["loops"]],
            ["control_repeat_until", "control_repeat"],
        )
        self.assertEqual(report["caller_frames"], [{
            "callsite_id": "call", "callee": "suite",
            "return_block_id": "after",
        }])
        self.assertEqual(report["loops"][0]["body_entry_id"], "send")
        self.assertIsNone(report["loops"][0]["after_loop_id"])
        self.assertEqual(report["loops"][1]["body_entry_id"], "inner")
        self.assertIn("dynamic_or_unbounded_control_loop", report["blockers"])
        self.assertIn("wall_clock_dependent_condition", report["blockers"])
        self.assertIn("nested_loop_state", report["blockers"])

    def test_unknown_message_is_not_reported_as_static(self):
        target = self.fixture()
        target["blocks"]["send"]["inputs"]["BROADCAST_INPUT"] = [
            2, "dynamicReporter"]
        target["blocks"]["dynamicReporter"] = block(
            "data_variable", parent="send")
        report, = analyze_target(target)
        self.assertIsNone(report["message"])
        self.assertIn("dynamic_broadcast_message", report["blockers"])

    def test_procedure_cycle_does_not_recurse(self):
        target = self.fixture()
        target["blocks"]["again"] = block(
            "procedures_call", parent="outer",
            mutation={"proccode": "suite"})
        report, = analyze_target(target)
        self.assertEqual(
            report["procedure_path"], ["event_whenflagclicked", "suite"]
        )

    def test_unreachable_procedure_path_is_empty(self):
        target = self.fixture()
        del target["blocks"]["call"]
        report, = analyze_target(target)
        self.assertEqual(report["procedure_path"], [])


if __name__ == "__main__":
    unittest.main()
