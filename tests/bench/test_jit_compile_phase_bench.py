"""Regression tests for LLVM/JIT startup log parsing."""
import unittest
from jit_compile_phase_bench import CONFIGS, count_direct_calls, parse_logs


class PhaseParserTests(unittest.TestCase):
    def test_stages_and_eager_compilation(self):
        data = parse_logs(
            "[startup] read-sb3         0.123ms\n"
            "[startup] first-step       2500.500ms\n"
            "[jit-timing] abc emit=10.0ms opt=600.2ms submit=0.1ms "
            "lookup=890.3ms total=1500.6ms funcs=20 ir=4567 tier=eager\n"
        )
        self.assertEqual(data["stages_ms"]["first-step"], 2500.5)
        self.assertEqual(data["compilations"][0]["lookup_ms"], 890.3)
        self.assertEqual(data["compilations"][0]["functions"], 20)
        self.assertEqual(data["compilations"][0]["tier"], "eager")
        self.assertFalse(data["background_compilations"])

    def test_async_and_missing_compile(self):
        data = parse_logs("[startup] greenflag-spawn  100.0ms\n"
                          "[jit-async] test-script finished 800.5ms\n")
        self.assertEqual(data["background_compilations"][0]["finish_ms"], 800.5)
        self.assertEqual(data["compilations"], [])

    def test_orc_object_ready_and_initialization(self):
        data = parse_logs(
            "[jit-object-timing] backend-init target=0.1ms "
            "lljit=0.6ms symbols=0.1ms total=0.8ms\n"
            "[jit-object-timing] abc objects=1 bytes=1234 "
            "pre-object=300.4ms post-object=0.7ms\n"
        )
        self.assertEqual(data["jit_initialization"][0]["lljit_ms"], 0.6)
        self.assertEqual(data["object_phases"][0]["pre_object_ms"], 300.4)
        self.assertEqual(data["object_phases"][0]["post_object_ms"], 0.7)

    def test_source_call_density(self):
        blocks = {
            "root": {"opcode": "event_whenflagclicked", "next": "call"},
            "call": {"opcode": "procedures_call", "next": "tail"},
            "tail": {"opcode": "control_wait", "next": None},
        }
        self.assertEqual(count_direct_calls(blocks, "root"), (3, 1))
        blocks["tail"]["next"] = "call"
        self.assertEqual(count_direct_calls(blocks, "root"), (3, 1))
        self.assertEqual(count_direct_calls(blocks, "missing"), (0, 0))

    def test_modes_are_distinct(self):
        self.assertEqual(CONFIGS["hotspot_async"][0], "hotspot")
        self.assertEqual(CONFIGS["hotspot_lazy_on"][3], "on")
        self.assertEqual(CONFIGS["hotspot_lazy_off"][3], "off")


if __name__ == "__main__":
    unittest.main()
