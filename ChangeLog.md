# Changelog

## Unreleased

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
