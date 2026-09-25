# Changelog

## Unreleased

- `limba -O1` optimises each function as soon as it is complete, then
  writes it and frees it, as `-O0` already wrote it: the pipeline runs to
  a fixed point on each function (no pass looks at another), through the
  new `limba_optimizer_new`, `limba_optimizer_func` and
  `limba_optimizer_free`; `limba_optimize` is that loop on a module. The
  output is the same bytes, the statistics the same numbers; at most 71
  MB of memory instead of 95 on 108 203 lines. The time barely moves:
  the passes themselves are the cost.
- `val` reads a literal of Luxia of the type of its variable, as Ada's
  `'Value`: a sign, spaces and tabs around, `_` between two digits,
  `0x`, `0o` and `0b`, and for a real `inf`, `-inf` and `nan`, what `str`
  writes; a real is rounded once to its type (a `Float32` was rounded
  twice, through `Float64`), a `UInt64` reads up to 2^64 - 1. A text that
  is no number of that type, its range included, gives `false` and
  leaves the variable alone: `"300"` into a `UInt8` stopped the program.
  `str_to_i64` takes the bounds; `str_to_u64` and `str_to_f32` are new.
- `arg(i)` outside 1..`argcount()`, a negative width and decimals outside
  0..100 in `x:w:d` are range errors where they are written, compile
  errors (L0029) when constant: they were an empty string, no padding
  and 0 or 100 decimals.
- The random programs call `val` (texts at the edges of each type, in
  every base, with spaces and misplaced `_`), `readline` on an input of
  their own, `argcount` and `arg` on a command line of their own, `low`
  and `high` of strings, and now and then write with a width or decimals
  out of range; `tools/lx_gen -i` and `-a` print that input and command
  line.
- A NaN written with decimals (`x:w:d`) is `nan`, as without them: the
  reference interpreter wrote `-nan` for 0 / 0, whose sign bit x86 sets.
- The random programs write items with a width (`x:w`, 0 to 12) and
  reals with decimals (`x:w:d`, 0 to 10), and call `halt`: with 0 or
  2..255, or with a status at an edge through a variable (1 and those
  outside 0..255 are a range error at its name); some end by `halt`.
- The gvn pass drops a check that a check of the same condition
  dominates, whatever its code: the first would have stopped the program.
  Repeated nil checks of one pointer go, among others; on the benchmarks
  the checks cost +16.1 % of the instructions run instead of +19.3 %
  (binary-trees +23.4 % to +16.0 %, pidigits +42.2 % to +32.4 %).
- The front end compiles 108 203 lines of Luxia (the benchmarks copied
  100 times) to a `.lir` in 107 ms, 0.99 µs a line (median of 21 runs),
  from 257 ms (2.4 µs) when first measured: each function is written as
  soon as it is complete and its body freed, the heap grows by 64 MiB at
  a time (glibc), and a release does not free its memory before it ends
  (as Clang's `-disable-free`); faster literals, positions, lookups and
  writes. In a release, `limba` no longer verifies the IR it makes
  (as Clang leaves its verifier out): who reads a `.lir` verifies it,
  and so do the tests; `--verify` asks for it (136 ms), `--check` and
  the builds for testing always do it. The output is the same bytes.
- Checks can be turned off where they are not wanted, as Ada's `pragma
  Suppress`: `pragma suppress(index_check, overflow_check);` among the
  declarations of the program holds for the whole file, among those of a
  routine for the routine, as a statement to the end of its list;
  `pragma unsuppress(...)` turns them back on, the innermost wins. The
  checks are `index_check`, `range_check`, `overflow_check`,
  `division_check`, `conversion_check`, `shift_check`, `nil_check` and
  `all_checks`; `limba --suppress=...` turns them off in the whole
  source, with a warning. Where a check turned off would fail, the
  behaviour is undefined. `pragma` is a keyword (49); a wrong pragma is
  L0057, a wrong check L0058.
- An index `x[i]` inside `for var i := low(x) to high(x)` (or down, or
  from `i + c`, or to `j - c`, with `c >= 0` and the variable of an
  outer loop over `x`) is not checked: the variable of a for stays
  within its bounds and an array keeps its own for all its life. A
  string keeps its bounds when nothing assigns it, takes its address or
  can reach it from a routine (no global): `for var i := 1 to length(s)`
  and `length(s) downto 1` then leave out the check of `s[i]`. On the
  benchmarks the checks cost +19.3 % of the instructions run instead of
  +30.6 % (geometric mean; n-body from +36.4 % to 0). The random programs
  loop over their arrays and strings, index other arrays with the same
  variable, run inner loops from `i + c`, `i - c` and `i + (-c)`, and
  shorten strings inside the loop.
- The front end is faster: 108 203 lines of Luxia (the benchmarks copied
  100 times) go to a `.lir` in 200 ms instead of 257 ms, 1.85 µs a line.
  A real literal of up to 19 digits between 10^-38 and 10^19 is read in
  128-bit arithmetic, its common factors with 10^k being only 2 and 5;
  a rational whose terms are exact in the format rounds with one IEEE
  754 division; the positions of the nodes are found once; the writer of
  `.lir` files makes each LEB128 number aside. 2 000 000 random literals
  round as `strtod` and `strtof` do.
- A record or an array variable declared without an initial value
  (global, local, with computed bounds too) starts as what `new` gives:
  every scalar of a range narrower than its base holds a value outside
  it, even where 0 would be valid; and every read of such a field or
  element, of a variable, through a pointer or of a function result, is
  checked (a range error at the `.` or the `[`). Before, a variable held
  0 and was never checked, and only reads through a pointer were: Ada
  treats variables and allocators alike. The random programs leave some
  fields and elements of their globals without a value and read them.
- `copy(s, from, count)` takes its arguments from left to right (the
  count went before), and `from < 1` or `count < 0` is a range error at
  its name; past the end the result is cut short (`copy("abc", 3, 5)` is
  `"c"`). Before, `from` below 1 counted as 1, a negative count meant up
  to the end, and `from - 1` wrapped for the least `Int64`. The random
  programs call `copy` with any `from` and `count`.
- `chr` out of range is reported at its name and `s[i]` out of a string
  at the `[`: both took the place of their argument.
- The random programs use `Char` and `String`: literals with doubled
  quotes, letters of two to four bytes in UTF-8 and the names `LF`,
  `TAB`, `CR`, `NUL`; `&` of strings and characters in any mix,
  comparisons byte by byte, `length`, `s[i]`, `copy` from 1 on, `str` of
  numbers, Booleans and characters, `chr` (checked) and `ord`; strings as
  variables, parameters of every mode and results. The program starts
  printing each String global with its length and bytes.
- The random programs declare ranges whose bounds are exact constant
  expressions of the global constants without a type
  (`Int8 range (-9)..((k10 - k10) * (-(10)))`), for the variables of
  the routines and of the program.
- The random programs use `Float32` (literals within its range,
  arithmetic rounded once to float, conversions both ways), the real
  functions of the library on both real types (`sqrt sin cos tan arctan
  exp ln trunc round floor ceil`, the rounding ones often on a value with
  a fraction; the program starts printing all of them on a real global)
  and `dispose` of a record just made, followed by `p := nil`. A fixed
  case checks that an integer becomes a `Float32` rounded once, not
  through a double.
- Functions may return records and arrays: the caller passes, before the
  arguments, the address of a slot of its own where the result goes
  (L0054 is no longer reported for them). `f(x).field`, `f(x)[i]`,
  `r := f(x)`, a call as the argument of a record parameter and
  `return f(x)` all work; a path without `return` is still L0052.
- Arrays with computed bounds are freed: where their statement list
  ends and on every `return`, `exit` and `continue` that leaves it (they
  were never freed, one more block at every turn of a loop).
- The reference interpreter counts the blocks of `mem_alloc` never
  freed and the frees of what is not a live block; `lir_run` prints
  them, and a Luxia test that ends with blocks alive says how many.
- The random programs have functions returning records and arrays,
  called at the start of the program and printed whole, and local
  arrays with computed bounds (`array[T range x..x + k]`, 0 to 4
  elements) at the start of routines and loop bodies; the oracle counts
  the records `new` made, so a block the compiler leaks shows. The
  array types of the random programs are named `Y<n>` (a parameter
  `a<n>` is the same name).
- `out` parameters as Ada has them: a scalar one is copied back at every
  return, not before; reading it before giving it a value is L0053, and
  leaving on a path without one is the new L0056. A record or an array
  `out` is passed by address and not checked yet. The random programs
  use `out` parameters and print what comes back.
- `succ` and `pred` are checked on the value before the step: `pred` of
  the first value of an enumeration gave 255. The random programs use
  enumerations: literals, `succ`, `pred`, `ord`, comparisons, `case`
  with every value and no `else`, arrays indexed by them, `for` over
  their values.
- A negative exponent of `**` at run time is a range error, as Ada's
  `Constraint_Error` (the exponent is a `Natural`); the random programs
  now use exponents of any integer type.
- Reals: `abs` clears the sign bit as IEEE 754 says (`abs(-0.0)` gave
  `-0.0`); a conversion from a real to an integer or a range fits by
  the exact bounds (`R(2.0 ** 53)` failed for `R = Int64 range
  0..2 ** 53 + 1`, the bound having been rounded). The random programs
  use `Float64`: literals, `+ - * /`, the minus, `abs`, comparisons with
  NaN, conversions both ways; a constant `-0.0` is the rational 0.
- `**` at run time on every integer type: checked on `IntN` and `UIntN`
  (overflow, and no longer a conversion error on `Int8`..`Int32`),
  modular on `BitsN`; the exponent may be any unsigned value
  (`1 ** 18446744073709551615` is 1). New run-time functions `uint_pow`
  and `bits_pow`; the random programs use `**`.
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
