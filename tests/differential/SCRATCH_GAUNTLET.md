# Scratch Semantic Parity Gauntlet

A reproducible Scratch 3 **differential test**, generated as real SB3 projects.
This tests observable behavior, not just a second host-language implementation
of the Scratch operators.

## Quick start

From the xyo-cpp repository root:

    python3 tests/differential/scratch_parity_gauntlet.py

The test runner:
1. Generates deterministic SB3 projects and per-project case manifests in
   build/parity-gauntlet/.
2. Runs each project through the vendored refs/scratch-vm with its optional
   compiler disabled (reference interpreter).
3. Runs the same SB3 through xyo with --jit off, eager, and hotspot.
4. Compares the complete ordered list named out; every case has a stable ID.
5. Exits nonzero for a mismatch, crash, timeout, incomplete script, or invalid
   JSON. A fixed seed (1618) makes fuzz cases reproducible.

To save a machine-readable summary:

    python3 tests/differential/scratch_parity_gauntlet.py \
      --json build/parity-gauntlet/report.json

To generate the SB3 projects without running them:

    python3 tests/differential/scratch_parity_gauntlet.py --generate-only

To test only some modes or shards:

    python3 tests/differential/scratch_parity_gauntlet.py \
      --modes off,hotspot --max-shards 2

For a different seeded corpus:

    python3 tests/differential/scratch_parity_gauntlet.py --seed 20261107

## Domains

- String-to-number conversion: decimal/hex/binary/octal, negative zero, empty
  and whitespace-only strings, special double values, invalid strings.
- Arithmetic: overflow/underflow, round half boundaries, remainder semantics,
  div-by-zero, transcendental and other math operators.
- Scratch comparison: numeric-string coercion, Unicode case folding, mixed
  digit representations, non-BMP character pairs.
- String length, character indexing (UTF-16 semantics), substring containment.
- Lists: out-of-range and noninteger indices, insertion, replacement, deletion,
  list representation, case-insensitive search, numeric-string search.
- Type-changing loops: warp and yielding procedures, loop-count boundaries,
  CFG branches and repeated changes between number, numeric string, invalid
  string and infinity. Includes a one-iteration wrapper loop to trigger
  xyo's HotSpot compilation path.
- Seeded reproducible expression fuzzing.

All probes are labeled in the manifest. When an exact behavior is uncertain,
Scratch VM defines the expected output. No guessed fixture answers.

## Intentional limitations

- The reference is the vendored TurboWarp scratch-vm interpreter, **not** an
  independent server-side copy of the current Scratch website runtime.
- String(value) in the reference and xyo's variable dump are observable text
  representations. An internal number 1 vs string "1" difference that does
  not change observable behavior is not detected by a simple dump.
- Nondeterministic event ordering, sound, graphics, input, and current time are
  omitted: they require separately controlled clocks, renderers and inputs.
- Each shard's full out list is compared; failures are attributed by case ID.
  For diagnosing a failure, open the corresponding generated SB3 and its JSON
  manifest from build/parity-gauntlet/.

Requires Python 3, Node.js, the installed vendored scratch-vm dependencies,
and a built xyo executable. The tool touches only generated build/ files
and its own source; it does not modify existing test fixtures.

## TurboWarp compiled mode

Enable the compiler in vendored TurboWarp and compare its output directly to
the TurboWarp scratch-vm interpreter, without needing a built xyo:

    python3 tests/differential/scratch_parity_gauntlet.py \
      --modes turbowarp --json build/parity-gauntlet/turbowarp-report.json

The runner passes compiled mode to scratch_vm_dump.js. A case reports PASS
only when all observed list entries match the interpreter reference and the
done flag confirms completion.
