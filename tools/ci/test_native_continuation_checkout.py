"""Regression tests for the native-continuation Git checkout preflight.

Creates tiny isolated Git repositories. Nothing is staged or modified in the
developer's working tree.
"""
from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


GUARD_SOURCE = Path(__file__).with_name("native_continuation_checkout.py")


class NativeContinuationCheckoutTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="xyo-ci-guard-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.cmake = self.root / "tests" / "CMakeLists.txt"
        self.osr_cmake = (
            self.root / "tests" / "differential"
            / "osr016_fractional_boundary_tests.cmake"
        )
        self.guard = self.root / "tools" / "ci" / "native_continuation_checkout.py"

        self.guard.parent.mkdir(parents=True)
        shutil.copyfile(GUARD_SOURCE, self.guard)
        shutil.copyfile(
            Path(__file__),
            self.root / 'tools/ci/test_native_continuation_checkout.py',
        )

        harness_lines = []
        for index in range(9):
            name = f"harness_{index:02d}.cpp"
            harness_lines.append(
                f"add_executable(xyo-nested-warp-case{index}-smoke"
                f" platform/{name})"
            )
            self.write(f"tests/platform/{name}", "int run() { return 0; }\n")

        cases = []
        for index in range(33):
            fixture = f"fixture_{index % 21:02d}.py"
            cases.append(
                f"add_test(NAME xyo-nested-warp-case{index:02d}"
                f" COMMAND python3 differential/{fixture})"
            )
        cases.append(
            "add_test(NAME xyo-native-simple-deopt-parity"
            " COMMAND python3 differential/native_simple_resume.py)"
        )
        self.cmake.parent.mkdir(parents=True, exist_ok=True)
        self.cmake.write_text(
            "\n".join(harness_lines + cases) + "\n", encoding="utf-8"
        )

        self.osr_cmake.parent.mkdir(parents=True, exist_ok=True)
        self.osr_cmake.write_text(
            "\n".join(
                f"add_test(NAME xyo-nested-warp-fractional-test{index}"
                " COMMAND python3 differential/fixture_00.py)"
                for index in range(4)
            ) + "\n",
            encoding="utf-8",
        )

        for index in range(21):
            self.write(
                f"tests/differential/fixture_{index:02d}.py",
                "# a required Python fixture\n",
            )
        self.write(
            "tests/differential/native_simple_resume.py",
            "# another required Python fixture\n",
        )

        self.git("init", "-q")
        self.git("add", ".")
        self.commit("initial fixtures")

    def write(self, relative: str, content: str) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")

    def git(self, *args: str) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(
            ["git", *args],
            cwd=self.root,
            text=True,
            capture_output=True,
            check=False,
        )
        if result.returncode != 0:
            self.fail(f"git {args} failed: {result.stderr}")
        return result

    def commit(self, message: str) -> None:
        self.git(
            "-c", "user.name=Test",
            "-c", "user.email=test@example.invalid",
            "commit", "-qm", message,
        )

    def run_guard(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(self.guard), *args],
            cwd=self.root,
            capture_output=True,
            text=True,
            check=False,
        )

    def make_script_uncommitted(self) -> str:
        relative = "tests/differential/fixture_00.py"
        self.git("rm", "-q", "--cached", relative)
        self.commit("remove fixture from HEAD without deleting working copy")
        return relative

    def test_complete_committed_checkout_passes(self) -> None:
        result = self.run_guard()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(
            "38 tests, 9 C++ harnesses, 22 Python scripts", result.stdout
        )
        self.assertIn("exist and match HEAD", result.stdout)

    def test_existing_but_uncommitted_python_is_rejected(self) -> None:
        missing_from_head = self.make_script_uncommitted()
        result = self.run_guard()
        self.assertEqual(result.returncode, 1)
        self.assertIn("UNTRACKED: " + missing_from_head, result.stderr)

    def test_staged_but_uncommitted_python_is_rejected(self) -> None:
        missing_from_head = self.make_script_uncommitted()
        self.git("add", missing_from_head)
        result = self.run_guard()
        self.assertEqual(result.returncode, 1)
        self.assertIn("UNTRACKED: " + missing_from_head, result.stderr)

    def test_allow_untracked_is_only_a_local_diagnostic_override(self) -> None:
        self.make_script_uncommitted()
        result = self.run_guard("--allow-untracked")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("uncommitted files allowed locally", result.stdout)

    def test_modified_committed_script_is_rejected(self) -> None:
        script = 'tests/differential/fixture_01.py'
        self.write(script, '# unsaved local edit\n')
        result = self.run_guard()
        self.assertEqual(result.returncode, 1)
        self.assertIn('UNCOMMITTED_CHANGE: ' + script, result.stderr)

    def test_staged_edit_to_committed_script_is_rejected(self) -> None:
        script = 'tests/differential/fixture_01.py'
        self.write(script, '# modified and staged\n')
        self.git('add', script)
        result = self.run_guard()
        self.assertEqual(result.returncode, 1)
        self.assertIn('UNCOMMITTED_CHANGE: ' + script, result.stderr)

    def test_modified_ctest_registration_is_rejected(self) -> None:
        self.cmake.write_text(
            self.cmake.read_text(encoding='utf-8') + '# dirty\n',
            encoding='utf-8',
        )
        result = self.run_guard()
        self.assertEqual(result.returncode, 1)
        self.assertIn('UNCOMMITTED_CHANGE: tests/CMakeLists.txt', result.stderr)

    def test_local_diagnostic_can_inspect_modified_sources(self) -> None:
        self.write('tests/differential/fixture_01.py', '# modified locally\n')
        result = self.run_guard('--allow-uncommitted')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('uncommitted files allowed locally', result.stdout)

    def test_missing_checkout_selftest_is_rejected(self) -> None:
        (self.root / 'tools/ci/test_native_continuation_checkout.py').unlink()
        result = self.run_guard()
        self.assertEqual(result.returncode, 1)
        self.assertIn(
            'MISSING: tools/ci/test_native_continuation_checkout.py',
            result.stderr,
        )

    def test_missing_python_fixture_is_rejected(self) -> None:
        (self.root / "tests/differential/fixture_01.py").unlink()
        result = self.run_guard("--allow-untracked")
        self.assertEqual(result.returncode, 1)
        self.assertIn("MISSING: tests/differential/fixture_01.py", result.stderr)

    def test_missing_cpp_harness_is_rejected(self) -> None:
        (self.root / "tests/platform/harness_03.cpp").unlink()
        result = self.run_guard()
        self.assertEqual(result.returncode, 1)
        self.assertIn("MISSING: tests/platform/harness_03.cpp", result.stderr)

    def test_missing_osr016_include_is_rejected(self) -> None:
        self.osr_cmake.unlink()
        result = self.run_guard()
        self.assertEqual(result.returncode, 1)
        self.assertIn("Source audit failed", result.stderr)

    def test_incomplete_ctest_registration_is_rejected(self) -> None:
        lines = self.cmake.read_text(encoding="utf-8").splitlines()
        self.cmake.write_text("\n".join(lines[:-5]) + "\n", encoding="utf-8")
        result = self.run_guard()
        self.assertEqual(result.returncode, 1)
        self.assertIn("Expected at least 38 native continuation CTests", result.stderr)


if __name__ == "__main__":
    unittest.main()
