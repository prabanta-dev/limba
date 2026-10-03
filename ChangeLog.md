# Changelog

## Unreleased

- fasta writes its tables as typed constants: the codes and their
  weights as aggregates, the odds added up at the start. The same output;
  the interpreter takes 0.05 % more steps at -O0 and 0.1 % at -O1, the
  filling of the tables.

- The random programs use aggregates: records and arrays given whole
  (positional, by index with ranges, with else), often reading the
  variable they are given to, as results of functions, as arguments of
  records and open arrays; the procedures print the indices of their
  open arrays too. Found that way: the indices of an array past Int64
  (a UInt64 index near its top) were cut to 64 bits, and else filled the
  wrong elements. The programs of every seed change.

- Aggregates in the compiler (specification § 6.8): `{x: 1.0; y: 2.0}`
  for records, `{1, 2, 3}`, `{Red: 1, Green: 2, Blue: 3}` and `{1..3:
  0.0, else 1.0}` for arrays, with the type of their context (a
  declaration, the target, a parameter, the result, an enclosing
  aggregate), complete and checked, computed whole in a slot of their own
  and then copied, straight into a local just declared, a typed
  constant or a variable ready at once. Typed constants of records and
  arrays are globals filled before any code; an aggregate for an
  open-array parameter takes the bounds of § 6.8; arrays with computed
  bounds are checked at run time. A unit's table of constants orders no
  initialisation. Five new codes, L0081 to L0085.

- The specification describes aggregates (§ 6.8): a whole record
  (`{x: 1.0; y: 2.0}`, every field named, in order) or array (`{1, 2,
  3}`, `{Red: 1, Green: 2, Blue: 3}`, `{1: 10, else 0}`) written between
  braces, with its type from the context, complete, computed whole
  before it is assigned; typed constants of records and arrays, as in
  Delphi; open-array parameters, computed bounds and the variables
  ready at once of units follow (§ 2.1, 3.7, 3.9, 3.11, 4.3, 5.3, 6.7,
  7.1, 8.1, 11.5, 12, appendix A).

- The random programs in more files now often come with a web of units
  around them: two to four units of the program whose initialisations
  read and write each other's variables (the order is the compiler's to
  compute), cycles through implementations, a name two units give,
  `pragma hides` in the program and in units, and a small library whose
  unit `Coll` has the name of one of the program, with a name exported
  equal to a unit used. The program prints every value first; the
  warnings and notes are compared with those expected. `lx_gen -d`
  writes the units of the program in `prog/` and those of the library in
  `std/`. Found that way: a declaration of an interface hid a name of a
  unit used only by the implementation without the warning, and a
  `pragma hides` stating it was taken for one that hides nothing.

- `limba` frees the list of its `-I` directories and checks that it
  could have it: under asan the command no longer ends with a leak at
  every compilation of a `.luxia`. `test_luxia` now runs the command of
  the same build too, on the units in files with `-I` and `--stdlib`,
  and runs the `.lir` it writes.

- In the IR the file of the program is its name alone, as its units
  are: the same `.lir` from any directory, and a run-time error says
  `t.luxia:4:4` whatever path was given to `limba` (diagnostics keep the
  real path). A unit of the program with the name of one of the library
  gets a note with a code, L0080, as every diagnosis that stands alone.
  `cvalue(a, n)` with a negative `n` is a range error, as a count of
  `move`, no longer an index error; the specification says so, and no
  longer gives `new` a range error for a length outside the index type
  (§ 9.7, § 9.9). Two more fixed cases of units: a used unit that
  exports a name equal to another used unit, and a unit of the library
  that uses one the program does not.

- The random programs come in two files too: `lx_gen -d DIR SEED`
  writes the program and the unit `Lib`, which holds the routines that
  name nothing of the program (the generator now makes some routines
  sealed: base types, no global in sight), named directly or as
  `Lib.Name`, with an initialisation that prints first; a run-time error
  in `Lib` names its file. `test_luxia` runs every seed in one file and
  in two. A defect of the order of the initialisations found that way
  (a loop one symbol too far) is mended. An example with units in files,
  the library included, is in `tests/luxia/units/`. The checks of units
  are skipped for a file that uses none.

- Units (specification § 11), in the compiler. `limba prog.luxia` reads
  the program and every unit it uses and writes one `.lir`: `unit Name;
  interface ... implementation ... [begin ... end] end Name.`, one `uses`
  clause in each part, names written directly or qualified (in types
  too), an ambiguous name an error where it is used, a declaration that
  hides a name of a unit a warning unless `pragma hides(Unit.Name)` says
  it is meant, bodies whose headings conform to the interface, variables
  of an interface read only outside their unit, cycles of interfaces an
  error and cycles through implementations allowed. The initialisations
  run before the program in an order the compiler computes from what
  they can read and write, through the calls (an error with the chain
  when there is none); a variable ready at once orders nothing. Units are
  found in the directory of the program, then with `-I DIR`; those of
  the standard library (`--stdlib=DIR`) form a space of their own, which
  the program never redirects. In the IR a unit's names follow it
  (`Geometry.Distance`, `$std.` before the library's) and positions name
  its file stably (`geometry.luxia`, `<I1>/...`, `<std>/...`). Only what
  an initialisation reaches goes in the IR: a routine no call reaches is
  still checked, then dropped with the routines of C only it called
  (`limba_module_truncate`). Under `restrictions(no_external)` a routine
  of C of the library reached is an error. The front end library takes
  the directories and a reader of units in memory (three fields at the
  end of `limba_luxia_options`); a unit compiled alone is checked and
  gives nothing. Diagnostics L0064-L0079; keywords `unit`, `uses`,
  `interface`, `implementation`.

- The compiler follows the reviewed specification. `length(a)` is an
  `Int64` whatever the index, and so are the counts of `move`,
  `translate`, `reverse`, `occurrences`, `readbytes` and `writebytes`
  and the results of the last two that count: before, `length` of an
  `array[Byte]` was 0 (256 wrapped in a `Bits8`), and the length of an
  array indexed by an enumeration was a value of that enumeration. An
  array type whose length does not fit an `Int64` is an error; with
  computed bounds such a length, or a size past the memory, is "out of
  memory" (before, a computed array over a whole `UInt64` had no
  elements). Real literals are exact until they take a type (`1e400 /
  1e390` is `1e10`; L0008 is no longer given). `Colour(n)` converts a
  position to an enumeration value, checked. `p^.x` and `p^[i]` are the
  new error L0063. The random programs use counts of `Int64`, negative
  ones on every index.

- The specification describes units (§ 11), the first step of Luxia 1,
  in the form of Delphi and Free Pascal with the rigour of Ada:
  `unit Name; interface ... implementation ... [begin ... end] end.`,
  one `uses` clause in each part, names written directly or qualified,
  an ambiguous name an error where it is used (never decided by the
  order of the clauses), headings of bodies conforming to the interface,
  variables of an interface read-only outside their unit, cycles only
  through implementations, an order of initialisation computed from what
  the initialisations can read and write, two spaces of units (the
  program and the standard library) so that a new version of the
  library never changes nor breaks a program. Keywords: `unit`, `uses`,
  `interface`, `implementation` replace the reserved `module`, `import`,
  `export`. Not yet in the compiler.
- A review of the whole specification: `length` is an `Int64` whatever
  the index (`low` and `high` stay in the index type), as are the counts
  of `move` and the routines on spans, so that `array[Byte] of Byte`
  has a length; real literals are exact values of any size, checked only
  when they take a type; `Colour(n)` converts a position to an
  enumeration value; `p^.x` and `p^[i]` are errors (one form, `p.x`);
  an array type whose length does not fit an `Int64` is an error; a new
  § 3.14 defines the discrete types; the layout of records, the order
  of pragmas in a file, the end of a `for` at the last value of its
  type, comparisons, mathematical functions, `readline(out s)` and
  `cvalue` are stated as the compiler does them. The title is now "The
  Luxia Language". The compiler follows in the next changes.

- The reference interpreter frees the strings and BigInts that nothing
  holds any longer: a mark-and-sweep collection reads the values of the
  calls alive, the memory alive (slots, globals, blocks not freed) and
  what the runtime is making, conservatively. Before, every value stayed
  until the end, and a long computation (pidigits beyond a few thousand
  digits) ran out of memory where an engine that frees dead values does
  not; now 10 000 digits of pidigits finish. The tests run every program
  with a collection before each value made as well, so that the whole
  net, random programs included, tries it.
- Joining two empty constant strings (`"" & ""`) no longer passes a null
  pointer to `memcpy`: undefined behaviour in C, harmless in practice,
  found by the undefined-behaviour sanitizer; a fixed case now covers it.
  The random programs also change two Strings in one loop, so that the
  jump into the loop passes one string twice, a case an engine that
  counts references must keep.
- Input and output in blocks (specification § 9.1, § 9.2), with the rules
  of `move` on the tract: `readbytes(a, from, count)` reads up to `count`
  bytes of the standard input into an array of `Byte` and returns how
  many, fewer only at the end of the input, as C's `fread`, the same on a
  file, a pipe or a terminal; `writebytes(a, from, count)` writes a tract
  of an array of `Byte` or of a String as it is. One input stream for
  `readline` and `readbytes`, one output stream for every writing
  routine, in the order of the calls; the end of the input is final. In
  the IR: `io_read` and `io_write` at the end of the run-time table. The
  random programs read their input in blocks and lines alike.
- Errors of input and output have a rule (specification § 10.1): an
  error in reading (never the end) or in writing stops the program with
  "input/output error", the new trap `IO` (107) of `traps.def`, exit
  status 1, as Ada's `Device_Error`; a write error is reported without a
  place, as the output is buffered. An output closed by its reader ends
  the program silently with exit status 141, which `halt` may no longer
  give. Before, `lir_run` took a read error for the end of the input and
  lost a write error. `read_line` and the `print_*` functions are
  `MAY_TRAP`; the fingerprint of the run-time table changes, the version
  of the IR (6) does not.
- Three routines of arrays (specification § 9.5), with the rules of
  `move`: `translate(a, from, count, table)` replaces each Byte of a
  tract by its entry in an `array[Byte] of Byte`, read whole first so
  that it may be the array itself; `reverse(a, from, count)` reverses a
  tract of any element type in place; `occurrences(a, from, count,
  pattern)` counts the non-overlapping occurrences of a pattern of Bytes
  or a String in a tract of Bytes or a String, and an empty pattern is a
  range error, as Ada's `Pattern_Error`. Narrower types than `Byte` are
  compile-time errors. In the IR: `mem_translate`, `mem_reverse` and
  `mem_count` at the end of the run-time table, which changes its
  fingerprint (version 6 unchanged); the checks stay instructions of the
  IR before the call. The random programs use them on arrays made by
  `new`.
- The front end of Luxia is a library, `include/limba/limba_luxia.h`
  ("limba" is Sardinian for language): `limba_luxia_compile_file` and
  `limba_luxia_compile_text` take the options of `limba` and a consumer
  that receives the module once declared, each function as it is
  complete (verified, optimised, then freed unless kept), the end with
  its outcome, and each diagnosis with its code and place. The header
  states what a consumer may rely on: every declaration comes first, ids
  never change while arrays may move, a function given is never touched
  again, and errors found while the bodies are made arrive after some
  functions were given; a stop ends the front end at once; out of
  memory ends the process. The `limba` command is now one such consumer:
  its `.lir` and its diagnostics are byte for byte the same. Its
  `--emit=lit` and `--check` at `-O1` now optimise a function at a time,
  as the `.lir` does, not the whole module: the `.lit` shows the IR of
  the `.lir`, which differed when a routine called one declared after it
  (the whole module put the later one in line). With `--emit=tokens` or
  `--emit=ast`, `--suppress` and `--target` are not looked at.
- The IR is version 6: in the signature of an extern, a narrow integer
  (`i8`, `i16`, `i32`) says how C wants it extended in a register,
  `sext` or `zext` (LLVM's `signext`, `zeroext`); `i1`, C's `_Bool`, never
  carries one. A callee compiled by Clang trusts the caller to extend, so
  an `unsigned char` of 200 sign-extended arrived as -56. Limba writes
  the marker from the Luxia type; the probe library gains narrow values
  near their top and an `unsigned int` past 2^31.
- Calls of C libraries (specification § 3.13, § 8.5, § 9.9, § 10.4), as
  Pascal's `external` and Ada's `pragma Import`, with no dependency: a
  routine whose body is `external "library" [name "symbol"]`; C types by
  name (`CInt`, `CLong`, `CSizeT`, `CBool`...), distinct types with the
  representation of C on the platform chosen by `limba --target`; opaque
  pointers (`new CPointer`), only assigned, passed and compared; records
  with `pragma convention(c, R)`; arrays, `var` and `out` as addresses.
  Strings, BigInts, Booleans, characters, enumerations, pointers of Luxia
  and ranges do not cross. C strings: `CString`, `newcstring`, `cvalue`,
  `freecstring`. What C does is declared unchecked; `pragma
  restrictions(no_external)` or `limba --restrict=no_external` forbids
  the boundary. The IR, now version 5, writes the platform of a module
  (`target`) and passes structs by value and as results of `call.ext`;
  the reference interpreter runs the C strings, not the calls, which a
  back end tests against `tests/luxia/ffi/probe.c`. `external` is a
  keyword. New diagnostics L0060, L0061, L0062.
- `BigInt`, a signed integer of any size (specification § 3.12, § 6.3,
  § 6.6): no overflow, memory its only limit; immutable and counted as a
  String, it goes wherever a String goes; not discrete, not bits, no
  subtypes. Its operations are those of the integers; conversions are
  exact from integers, half away from zero from reals, rounded once and
  straight to the type towards reals (`Float32(b)` never through
  `Float64`), checked towards integers; `write`, `str` and `val` take it.
  In the IR a BigInt is a `ref`, counted by every rule of the strings,
  compared by value (`big_cmp`, exactly -1, 0 or 1), never by its handle;
  its functions are in `runtime.def`, `val.h` reads its text. The
  reference interpreter runs it on `bigint.c`, whose division is now
  Knuth's algorithm D and whose limit is an argument; the random Luxia
  programs use it. pidigits has a second version with BigInt. The
  specification (§ 10.1) says a computation whose result is not used may
  be left out with the memory it would take; a check never is.
- An exponent or a shift count of the wrong type is reported with its
  operator (`'**' takes an exponent ...`), not the text of the operand.
- Short functions go in line (`inline`, the first pass of `-O1`;
  `LIMBA_OPTSKIP=inline` turns it off). A direct call goes in line when
  its callee was optimised before the caller and, optimised, has at most
  30 instructions, calls only functions optimised before it (so no
  recursion), has no slot holding strings and lets no address of a slot
  out by return or store. Its instructions keep their positions in the
  callee, so a run-time error points where the check is; its slots are
  zeroed each time the copy runs, as a frame would be. A caller grows to
  twice its size, at least 60 instructions more. Without a front end the
  functions are optimised after those they call; the optimizer keeps the
  short ones, so `limba` still frees each function once written.
  spectral-norm runs 25.9 % fewer steps of the reference interpreter,
  fasta 10.4 %; `limba -O1` does 10.4 % more work, most of it in the
  passes after inline, on 8 % more code. The
  specification (§ 10.1) now says how much memory the local variables of
  calls take depends on the implementation.
- Limba builds with `-ffp-contract=off`: `a * b + c` keeps the two
  roundings the IR gives it on every CPU, never fused into one FMA, as
  the reference interpreter must.
- A pass that moves out of loops what does not change in them (`licm`,
  after `gvn`): pure values and `ptr_live` of a pointer from outside
  when nothing in the loop may free a block; the checks and the bounds
  of an array made by `new` when they open the header, before any effect,
  so that a loop over such an array checks it once, not every round. A
  preheader is made on the edge into a loop only when something worth it
  goes into it. The IR gains `load.inv`, a load of memory that does not
  change while its block lives (the bounds of an array made by `new`),
  which a pass may move past stores but not past what may free.
  k-nucleotide runs 8.4 % fewer steps of the reference interpreter;
  `limba -O1` does 5.1 % more work: the pass runs once a function, and a
  function whose jumps all go forward is skipped without a CFG.
- Arrays made by `new` (specification § 3.10, § 9.5, § 9.7): a pointer
  may point to an open array type, and `new(A range lo..hi)` creates an
  array whose index goes from `lo` to `hi` (`range` after an open array
  type gives bounds, after a scalar type it constrains a value: said in
  § 3.7.1 and § 9.7). `p[i]` is checked for `nil`, a dangling pointer and
  the index; `low`, `high` and `length` take `p^`, which is never used as
  a whole; `dispose` frees the array and its Strings; a size in bytes past
  the address space is "out of memory". `move(src, from, dst, to, count)`
  copies elements between arrays as Ada's slice assignment, overlapping
  ranges allowed, every bound checked before a byte moves.
- A record or an array reached through a pointer is no longer an
  argument in any mode (L0059): the copy `in` arguments made is gone, a
  hidden cost; pass the pointer. `move` is a predefined name.
- The IR contract: `memcpy` has the semantics of `memmove`; an array made
  by `new` is a block `{lo, hi, elements}` accessed in one canonical
  sequence (nil, `ptr_live`, the bounds, the index).
- k-nucleotide keeps its counts in a table made by `new` that doubles
  when half full, one slot of 16 bytes read once per probe, instead of
  three arrays sized on the sequence and cleared for every k.
- The random Luxia programs make, fill, write, move within and dispose
  of arrays made by `new`, dangling and invalid disposes included.
- Two holes of the random Luxia programs, found by Meri: `val` now gets
  a text below the minimum of a signed type narrower than 64 bits half
  the time it reads into one; and a new pattern gives one string to two
  variables and carries a String round a loop, read after it (in SSA,
  strings in the arguments of jumps). A record with Strings always has a
  pointer type, and the fields of `new` records get strings made at run
  time, so that `dispose` has something to release.
- The random Luxia programs put Strings in record fields, array elements
  and computed arrays, declare local records and arrays (in loops too),
  and give them strings made at run time: on their own they now catch
  every sabotage of the counts of strings in memory. The reference
  interpreter checks those counts where a run stops too, at a trap or
  halt, not only at its end.
- IR version 4. A module may carry how its traps are presented: the name
  of its language, the prefix of their messages (`language "luxia"`), and
  texts that replace those of `traps.def` (`message 105 "..."`); the
  verifier refuses a code `traps.def` does not have, an empty text, a
  code twice. Luxia writes `luxia`; `lir_run` presents a trap as
  `luxia: index out of range at prog.luxia:12:5`, its own errors as
  `lir_run:`. `input_line` is gone from the runtime: nothing emitted it.
- The numbers `val` reads are public API, `include/limba/val.h`
  (`limba_val_int`, `limba_val_real`), so that every back end reads what
  the reference interpreter reads.
- Specification of Luxia, § 10.1: how much memory a program may use
  depends on the implementation, and going past it is always "out of
  memory", never a crash. The reference interpreter counts blocks, the
  slots of live calls and strings against a budget (1 GiB): a string
  that grows without end is the trap, no longer the end of the process.
  Slots are freed when their call returns, and each `sconst` is made once.
- A signalling NaN of type f32 keeps its bits where the IR copies them
  (bitcast, load, store, memcpy, fneg, calls): the reference interpreter
  held an f32 in a double, and the conversion of the CPU quietened it;
  constant folding did the same with bitcast and fneg. An f32 is now the
  32 bits of its float everywhere: in the interpreter, and in `fconst
  f32` and the initial value of an f32 global, which carry those bits
  (`nan.0x7f800001` in the text form), not a widened double.
- Pointers and strings in memory made rigorous (specification of Luxia
  § 3.10, § 9.7, § 10). Reading or writing through a pointer to a
  disposed object is the run-time error "dangling pointer" (code 105,
  `dangling_check`), checked at the access itself, after everything the
  statement evaluates (`p.f := g()` with a `dispose(p)` in `g`); a
  second `dispose` is "invalid dispose" (106), which cannot be
  suppressed; calls nested too deeply stop with "stack overflow" (28),
  never a crash. A dangling pointer never equals a pointer to a newer
  object. In Luxia 0 an object reached through a pointer cannot be a
  `var` argument, nor an `out` record or array (L0059); as an `in`
  argument it is passed by copy.
- IR version 3: `retain T p, n` and `release T p, n` count the strings of
  `n` values of type `T` in memory; a slot may have a type, and a typed
  slot is released at every return; every slot is zeroed at entry;
  `ptr_live` (a new runtime attribute, reading which blocks are alive)
  gives the dangling check; `mem_free` traps on what is not a live
  block. The verifier refuses a counted type without a string or with an
  array of 0 elements. `limba_type_holds_str` is public. Luxia copies a
  record or an array holding a String with retain and release around the
  `memcpy`, and releases it before `mem_free`.
- The reference interpreter counts the references to strings from memory
  and checks the rules of the IR on them (`check_mem`, on in every test,
  `lir_run --check-mem`): a release below zero, the bytes of a string
  read or written as another type or in part, counts that do not match
  memory at the end are the status BADMEM. The count of bad frees is
  gone: they trap.
- Specification of Luxia 0, § 3.11: a `String` without an initial value
  (in a record, an array or the memory of `new`) is `""`, as it already
  was.
- The text of a real is public API, `include/limba/fmt.h` (it was a
  private header): `limba_fmt_f64`, `limba_fmt_f32` and the new
  `limba_fmt_f64_fixed` give, byte for byte, what `print_f64`,
  `print_f32`, `str_from_f64`, `str_from_f32` and `str_from_f64_fixed`
  write, so that a back end writes what the reference interpreter
  writes. `runtime.def` names the function of each.
- Limba builds with Clang again (`CC=clang ./build.sh`): the pass
  table of the optimizer initializes every field.
- Diagnostics: a rule is stated as one ("the keyword 'begin' must be
  written in lowercase only", "'exit' is allowed only inside a loop"),
  and code in a suggestion is quoted ("write '(a and b) or c' or
  'a and (b or c)'"). The name after `end` must be spelled as declared
  (L0022), and L0015 shows it so. A source that is not UTF-8 gives one
  error, not an echo from the parser; `'ab'` is one error; L0033 no
  longer follows an L0025 on the same declaration.
- `low(T)` and `high(T)` of a discrete type (integers, subtypes with a
  range, enumerations, `Char`, `Boolean`) give its first and last value,
  a constant of `T`, as Ada's `T'First` and `T'Last`.
- `ord` takes a `Char`, a `Boolean` or an enumeration only: an integer
  is converted instead.
- A `Float32` is written in the shortest form that reads back as that
  `Float32` (`0.1`, not `0.10000000149011612`); `str` too. The runtime
  gains `print_f32` and `str_from_f32`.
- The cfg pass sends a jump to a block that only jumps on where that
  block goes, and joins a block to its only predecessor when that one
  jumps to it: the benchmarks run 2 % (binary-trees) to 10 %
  (reverse-complement) fewer instructions, mandelbrot 9.6 %, fannkuch
  7.2 %. It builds one CFG a run and finds the blocks no longer reached
  by a walk along the jumps; a block found dead is emptied, so that its
  jumps are no edges. -O1 runs 4.5 % more instructions: gvn looks again
  at the blocks joined. The bounds pass counted the parameters an edit
  had replaced when it matched a parameter with its argument; it counts
  the live ones now.
- The Luxia front end interns the name of the source file once, not at
  each node it gives a position (a strlen and a hash lookup each), and
  the SSA puts the parameters first only in the blocks where it added
  some: 5.3 % fewer instructions run at `-O0` on 108 203 lines, 3.7 % at
  `-O1`, the output the same bytes.
- Folding drops a check of a constant true: spectral-norm runs 8 % more
  instructions with its checks than without instead of 16 %, pidigits
  27 % instead of 32 %, fasta 31 % instead of 34 %.
- A new pass, bounds, off unless asked (`enable` in the options,
  `LIMBA_OPTON=bounds`): after ABCD, a check whose condition the facts
  before it prove goes (differences of values from constants, sign
  extensions, add.ov and sub.ov of constants, loop variables that only
  grow or shrink, branches and checks passed), and of a check of both
  bounds only the half not proved stays; nothing moves. It takes
  fannkuch-redux from 38 % to 31 % more instructions than without
  checks, and leaves the other benchmarks as they were, for 5 % of the
  time of `-O1`: it waits for the loads made once and the relations
  between loop variables. The tests run it: `tests/opt/bounds.lit` holds
  checks at the very edge of each difference.
- The manager wakes a pass only when a pass that may give it work has
  changed the function (a mask for each pass), in place of the one flag.
- One edit for each function from its SSA to the end of the optimiser:
  the front end leaves its last edit (trivial parameters, unreachable
  blocks) to apply, `limba_ssa_finish_edit`, and the passes add theirs
  to it, reading the operands through it and passing over what it marks
  dead; the function is rebuilt once instead of four or five times.
  `-O1` takes 142 ms instead of 157 on 108 203 lines (1.31 µs a line;
  11.5 % fewer instructions run), the output the same bytes. With
  `--verify` the edit is applied after each pass to verify it, and the
  code must come out the same; the builds for testing verify each
  function after the passes, on the edit applied once. `test_luxia`
  compiles each program a second time as `limba -O1` does, and
  `test_gen` alternates the two ways of applying the edit.
- The Luxia front end gives a pure instruction asked again in the same
  block the value it made already (a small cache keyed by what it
  computes, in `lxl_emit`): constants, extensions, comparisons, addresses.
  At `-O0` the large file has 363 331 instructions instead of 445 901
  and its `.lir` 4.98 MB instead of 5.85; at `-O1` gvn has half the work
  and the output is the same. The time moves little: 1.7 % fewer cycles
  at `-O1`, 0.8 % at `-O0`, where the hash costs about what it saves.
- gvn folds each instruction before it looks for an equal value, in the
  same walk and with the same edit: the fold pass is gone as a pass of
  its own (`LIMBA_OPTSKIP=fold` still turns the folding off). The
  function is rebuilt once where it was rebuilt twice: `-O1` takes 157
  ms instead of 168 on 108 203 lines (1.45 µs a line), and the code has
  the same number of instructions on every program tried (730 655 on
  the benchmarks, the large file and 500 random programs); two random
  programs keep a constant in another block.
- `-O1` takes 168 ms instead of 191 on 108 203 lines (1.55 µs a line
  from 1.77, median of 21 runs on one core), the output the same bytes:
  the passes share the CFG of a function, built once and dropped only
  when the cfg pass changes a branch or a block (the builds for testing
  check that a CFG kept is the one the function has); gvn hashes its
  keys a word at a time instead of a byte at a time; the pipeline stops
  as soon as every pass has run on the function without a change, and a
  change of dce, which gives the others no work, does not ask for one
  more round.
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
