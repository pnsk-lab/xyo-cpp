ctest --test-dir board/.osr016-ci-gui-sim -N -R '^xyo-nested-warp-' > nested-osr-tests.txt
cat nested-osr-tests.txt
count=$(sed -n 's/^Total Tests: //p' nested-osr-tests.txt)
test -n "$count" && test "$count" -ge 37
for name in xyo-nested-warp-fractional-boundary xyo-nested-warp-fractional-boundary-fast xyo-nested-warp-fractional-boundary-late xyo-nested-warp-fractional-boundary-late-fast; do
  grep -Eq "Test +#[0-9]+: ${name}$" nested-osr-tests.txt
done
