ctest --test-dir board/.osr016-ci-gui-sim -N -R '^(xyo-nested-warp-|xyo-native-simple-deopt-parity$)' > nested-osr-tests.txt
cat nested-osr-tests.txt
count=$(sed -n 's/^Total Tests: //p' nested-osr-tests.txt)
test -n "$count" && test "$count" -ge 38
# Guard both native root deopt and the four OSR-016 registrations.
for name in xyo-native-simple-deopt-parity xyo-nested-warp-fractional-boundary xyo-nested-warp-fractional-boundary-fast xyo-nested-warp-fractional-boundary-late xyo-nested-warp-fractional-boundary-late-fast; do
  grep -Eq "Test +#[0-9]+: ${name}$" nested-osr-tests.txt
done
# CTest may list a test even when its Python fixture or native
# executable is absent. Fail before running the expensive suite.
python3 - <<'PY'
import json
import pathlib
import re
import subprocess
import sys

suite = re.compile(r"^(xyo-nested-warp-|xyo-native-simple-deopt-parity$)")
tests = json.loads(subprocess.check_output([
    "ctest", "--test-dir", "board/.osr016-ci-gui-sim", "--show-only=json-v1"
], text=True))["tests"]
selected = [test for test in tests if suite.search(test["name"])]
missing = []
for test in selected:
    for index, argument in enumerate(test.get("command", [])):
        path = pathlib.Path(argument)
        required = (
            index == 0
            or argument.endswith(".py")
            or (path.name == "xyo" and path.is_absolute())
            or (path.name.startswith("xyo-nested-warp-")
                and path.name.endswith("-smoke"))
        )
        if required and path.is_absolute() and not path.is_file():
            missing.append((test["name"], argument))
if missing:
    for test_name, filename in missing:
        print(f"Missing dependency for {test_name}: {filename}", file=sys.stderr)
    sys.exit(1)
print(f"Verified {len(selected)} native continuation tests and their executables/scripts")
PY
