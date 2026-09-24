# Changelog

## Unreleased

- The Luxia lexer (`src/luxia/`): names folded to lowercase, keywords in
  lowercase only, literals converted on the spot (strings and characters
  as in Ada, no escapes), stable error codes `L0001`-`L0009`;
  `limba --emit=tokens file.luxia`. The first shared front-end pieces
  (`src/front/`): source files with 32-bit positions, and diagnoses
  printed with their line of source. `test_luxia` (30 lexer cases) and a
  fuzzing target.
- Limba is now the front end of Luxia only.
- `fround` (ties to even) and `fround.away` (ties away from zero) in the
  IR. NaN results of arithmetic have no fixed sign or payload, as in IEEE
  754: `fold` leaves NaN-producing operations to run time and `fadd`/`fmul`
  are no longer commutative, so optimising never changes the bits a
  program sees. 20 000 random programs: 100 000 optimised runs agree.
- A generator of random IR programs (`src/eval/gen.c`, `tools/lir_gen`),
  valid by construction and always terminating, and `test_gen`: 200 seeds
  through the OPTDIFF net on every build. Its tenth seed found that
  `fptosi`/`fptoui` were pure in the table but trapping in the
  interpreter: float to integer conversions now saturate (NaN gives 0),
  in the interpreter and in `fold`.
- A reference interpreter of the IR (`src/eval/`), slow and simple on
  purpose, with its own arithmetic: the yardstick of the optimiser.
  Provisional semantics where the IR has not fixed one yet (division by
  zero and INT_MIN / -1 trap 11, overflow traps 6, shift count modulo the
  width). `tools/lir_run` runs a module by hand.
- `test_eval`: eight programs checked against their expected output, and
  the OPTDIFF net: every program with a `@main` must behave the same
  unoptimised, fully optimised and with each pass alone.
- The optimiser: one pipeline in one table, repeated to a fixed point,
  with per-pass counters, passes skipped by name and verification after
  every pass. Passes: `cfg` (constant branches, unreachable blocks),
  `fold` (exact constant folding and integer identities; nothing that
  would trap, overflow or change a float), `gvn` (dominator-scoped value
  numbering of pure operations), `dce` (unused values and effect-free
  run-time calls). `limba -O1`, `--stats`, `--verify-each`, `--skip=`.
- `test_opt`: expected results per pass and the corpus optimised twice.
- The Limba IR: SSA with block parameters, typed scalar values, explicit
  control flow read from terminators, run-time library calls declared in a
  shared table (`include/limba/ir.h`, `ir_ops.def`, `runtime.def`).
- Verifier (types, shapes, dominance), text form `.lit` (canonical,
  exact floats), binary form `.lir` (LEB128, deterministic, reads untrusted
  input), and the `limba` program converting between them.
- `build.sh` with release, debug and sanitizer variants; `test_ir` (text
  fixed point, binary round trip, damaged binaries, invalid modules) and a
  fuzzing target for both readers.
- Project layout, rules and style settings (from Janas).
