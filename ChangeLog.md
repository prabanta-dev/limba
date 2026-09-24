# Changelog

## Unreleased

- What `new` gives, as Ada's bounded error in the strict form of
  `Normalize_Scalars` with validity checks: every scalar of a range
  narrower than its base gets a value outside it (the rest is 0), and a
  read through a pointer (`p.f`, `p^`, `p[i]`) checks it, a range error
  at the `.`, `^` or `[`. Arrays are filled by doubling copies of their
  first element. The random programs leave fields of new records without
  a value and read them.
- A nil pointer is reported where it is gone through, the `.` or the `^`
  (it took the place of the statement before).
- The random programs use records (fields read and written, copies,
  `var` parameters in procedures and `in` parameters in functions) and
  pointers to them (`new`, `nil`, comparisons, fields through a
  pointer, the error of a nil pointer). A reversed record copy is found
  by 5 seeds out of 1000 and by a new fixed case.
- The random programs declare constants, global and inside blocks, with
  or without a type, and use constant expressions whose values pass 64
  bits (`**`, `div`, `mod`, `rem` on negative values), checked only when
  they take a type; the generator computes them exactly too. No fault
  found: a deliberate error in the folding of `mod` fails 516 seeds out
  of 1000, and a fixed case now covers it.
- Open arrays as Ada has them: `array[I range <>] of T`, for parameters
  (written there or named with `type`), takes the bounds of its
  argument; `low`, `high` and `length` are those bounds, in the base
  type of `I`; an index is checked against them; when `I` is a range the
  bounds of a non-empty argument must belong to it (at compile time when
  known, at the call otherwise). The IR passes the address and both
  bounds. `array of T` is gone; spectral-norm and k-nucleotide use the
  new form. A `String` is read as such an array that always starts at 1.
- The random programs pass arrays to open parameters and use `low`,
  `high`, `length` and loops over arrays. They found that a conversion
  from a signed value to a range above `INT64_MAX` never failed (fixed,
  with a case in `test_luxia`).
- Run-time errors as Ada has them: a program stops with exit status 1
  and a message that says which check failed and where; the codes of the
  checks are a table shared with Meri (`include/limba/traps.def`), and
  `lir_run` prints their text. `halt` takes 0 or 2..255: `halt(1)` is
  refused (L0055), a computed 1 is a range error. The benchmarks halt
  with 2 on a bad argument.
- Empty ranges as in Ada: `lo..hi` with `lo > hi` is legal, constant or
  computed; an array over it has length 0 and every index outside.
- A real without a format is printed in the shortest form that reads
  back exactly, as Python's `repr` (`src/common/fmt_f64.c`): `0.1`,
  `100.0`, `1e+16`, `-0.0`, `inf`, `nan`; checked against Python on
  200 000 values and in `test_front`.
- The random Luxia programs gain ranges (in variables, parameters,
  results, `for` loops and conversions), `in` and arrays indexed by a
  range. They found four faults, fixed with a case each in `test_luxia`:
  a constant in an operation took the range of the other operand instead
  of its base type (`x + 100` with `x` in `-5..20` was refused); `in` on
  constants made IR without a type; a conversion from an unsigned value
  to a range below zero never failed; a range check after a computed
  value was reported at the last operator instead of the assignment,
  argument, `return` or `for`. The target of an assignment is now
  evaluated before its value, as the specification now says.
- Random Luxia programs that know what they print (`src/luxia/gen.c`,
  `tools/lx_gen`): well-typed by construction, with integers of every
  family, Boolean, routines with `in` and `var` parameters and the
  statements of Luxia 0; the generator runs each program itself, by the
  rules of the specification, and `test_luxia` checks that the program
  compiled, optimised or not, prints the same and stops with the same
  run-time error at the same place (`LIMBA_LXGEN_SEEDS`,
  `LIMBA_LXGEN_FIRST`). Found at once: `abs` of an unsigned number was
  computed as if it had a sign, fixed.
- The other eight single-thread benchmarks of the Benchmarks Game in
  Luxia (`tests/luxia/benchmarks/`): binary-trees, fannkuch-redux, fasta,
  k-nucleotide, mandelbrot, pidigits, reverse-complement, spectral-norm.
  All nine print the expected output, before and after optimisation.
  `test_luxia` compares the output byte by byte (mandelbrot writes a
  bitmap) and gives a program the `.in` file beside it as standard input.
- README: the state of the project and a roadmap.
- Positions of the source in the IR (version 2 of the binary form): a
  table of (file, line, column) in the module and one index per
  instruction, kept through `edit.c` and every pass, written as
  `pos k "file" line column` and `!k` in the text form. The reference
  interpreter reports where a trap stopped the program, innermost call
  first, and `lir_run` prints it: `trap 11 at err.luxia:5:13`. The Luxia
  generator gives each instruction the place of the construct it comes
  from.
- Luxia to the IR: SSA construction after Braun et al.
  (`src/front/ssa.c`), with block parameters, trivial ones removed, the
  check of definite assignment and of the return on every path; the
  lowering of the checked tree (`src/luxia/lower*.c`) with the checks
  Luxia promises at run time (overflow, unsigned underflow, index, range,
  nil, division by zero, conversion, shift). `limba file.luxia` writes
  `file.lir`. The run-time table gains the output, input, command line
  and mathematics Luxia 0 needs, and the reference interpreter runs them;
  `lir_run` passes arguments and standard input to the program. n-body,
  translated to Luxia, prints the output of the Benchmarks Game.
  `test_luxia` adds 31 run cases, each also optimised (OPTDIFF).
- The semantic phase of Luxia (`src/luxia/sema*.c`): names with the rule
  of consistent spelling, types and routines visible in all their scope,
  constants and variables after their declaration; subtypes with ranges,
  distinct types, records, pointers that refer to each other; constants
  computed exactly and checked when they take a type; the same type for
  both operands of every operator, no implicit conversion, shifts and
  bit operators on `BitsN` only; calls, modes of parameters, the routines
  of the language; `case` with coverage and without overlaps; `exit` and
  `return` where they belong. Errors L0021-L0051. `test_luxia` adds 37
  semantic cases and checks every program of `tests/luxia/benchmarks`
  (for now n-body, translated).
- The shared table of types (`src/front/types.c`): identity through a
  root (a range stays compatible with its base, `new` makes a type of
  its own), records laid out as C lays them out, arrays with overflow
  checks on their size, and types printed for messages. The shared table
  of scopes and names (`src/front/symtab.c`): lookup never declares,
  declaring reports the duplicate.
- Integers and rationals of any size for constant expressions
  (`src/front/bigint.c`), bounded at 16384 bits: constants are exact, as
  in Ada, and are rounded to `Float64` or `Float32` once, to nearest even,
  subnormals included. `test_front` checks 20 000 random literals against
  the rounding of `strtod` and `strtof`.
- The Luxia parser: recursive descent for declarations and statements,
  expressions through a Pratt engine shared by any front end
  (`src/front/pratt.h`), whose table carries the rules of rigour (no
  mixing of and/or/xor, no chained comparisons, no `a * -b`). Statements
  end with `;`, `case` branches start with `when`, loops exit with
  `exit when`, `for var i := a to b` declares its variable. After an
  error the parser goes on, one message per mistake; an `end` out of line
  with its opening gets a warning. `limba --emit=ast`; `test_luxia` adds
  35 expression and 17 program cases.
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
