ctest --test-dir board/.osr016-ci-gui-sim -N -R '^(xyo-nested-warp-|xyo-native-simple-deopt-parity$)' > nested-osr-tests.txt
cat nested-osr-tests.txt
count=$(sed -n 's/^Total Tests: //p' nested-osr-tests.txt)
test -n "$count" && test "$count" -ge 38
# Guard both native root deopt and the four OSR-016 registrations.
for name in xyo-native-simple-deopt-parity xyo-nested-warp-fractional-boundary xyo-nested-warp-fractional-boundary-fast xyo-nested-warp-fractional-boundary-late xyo-nested-warp-fractional-boundary-late-fast; do
  grep -Eq "Test +#[0-9]+: ${name}$" nested-osr-tests.txt
done
