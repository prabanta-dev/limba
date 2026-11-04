# The Limba IR, version 9

This is the normative description of the intermediate representation of
Limba, version 9 (`LIMBA_IR_VERSION` in `include/limba/ir.h`). A front end
that writes the IR, an engine that runs or compiles it and an optimisation
that rewrites it follow this text.

Four sources agree with it and with each other:

- `include/limba/ir_ops.def`: the operations, their operand shapes and
  what an optimisation may assume of them;
- `include/limba/runtime.def`: the functions of the run-time library;
- `include/limba/traps.def`: the run-time errors and the predefined
  families of exceptions;
- the reference interpreter, `lir_run` (`src/eval/eval.c`): the oracle.
  Every engine gives the same output, the same traps at the same
  positions and the same exit status on every program it runs.

Where this text and one of them disagree, one of them is wrong and is
corrected; nothing is decided by the code alone. The reasons behind the
rules are kept in the author's design notes; this text says what the
rules are. Section 18 lists what version 9 leaves open.

Words used with a fixed meaning:

- **module**: what a front end writes, the whole program (§ 3).
- **engine**: whatever runs a module: `lir_run`, the virtual machine of
  Meri, a compiler ahead of time or just in time.
- **trap**: a run-time error of the program, with a code of `traps.def`
  (§ 16). A trap is an exception (§ 13).
- **wrong module**: a module that breaks a rule the verifier cannot see,
  because it depends on run-time values (an access outside every object,
  a run-time call without the check the IR owes it). Its behaviour is not
  defined. `lir_run` stops with the status "wrong module"; Meri stops with
  an error of its own and a message, never a crash. A wrong module is an
  error of the front end, not of the program.
- **may**, **must**, **never**: as in any standard.

## 1. Overview

- A module holds interned strings, types, globals, external C functions,
  functions, a table of source positions, and a few header fields.
- A function is a list of blocks; block 0 is the entry. A block is a list
  of instructions: its parameters first, one terminator last.
- The IR is in SSA form with **block parameters** in place of phi nodes:
  a branch passes arguments to the parameters of its target.
- Everything refers to everything else **by index** (`limba_id`, 32 bits),
  never by pointer. An instruction produces at most one value, named by
  its index in the function.
- The control-flow graph is read from the terminators only.
- The IR does not know any source language. The checks of a language
  (overflow, index, range, nil, dangling pointer, division, conversion)
  are instructions (`check`, the `.ov` arithmetic), visible to the
  optimiser.

## 2. Types

### 2.1 Scalar types

The scalar types have fixed ids, equal to their kind:

| Id | Type | Size (bytes) | Alignment (bytes) | What it holds |
|---:|---|---:|---:|---|
| 0 | `void` | 0 | 1 | nothing: the result of an instruction or function without one |
| 1 | `i1` | 1 | 1 | a truth value, 0 or 1 |
| 2 | `i8` | 1 | 1 | an integer of 8 bits |
| 3 | `i16` | 2 | 2 | an integer of 16 bits |
| 4 | `i32` | 4 | 4 | an integer of 32 bits |
| 5 | `i64` | 8 | 8 | an integer of 64 bits |
| 6 | `f32` | 4 | 4 | an IEEE 754 binary32 |
| 7 | `f64` | 8 | 8 | an IEEE 754 binary64 |
| 8 | `ptr` | `ptr_bits` / 8 | `ptr_bits` / 8 | an address, opaque |
| 9 | `str` | `ptr_bits` / 8 | `ptr_bits` / 8 | a string of the run time, a counted handle |
| 10 | `ref` | `ptr_bits` / 8 | `ptr_bits` / 8 | another object of the run time (a `BigInt`), a counted handle |

`ptr_bits` is a field of the module header (§ 3.1): 64 by default, or 32.

- **Integers have no sign.** The sign is in the operation (`sdiv` and
  `udiv`, `icmp.slt` and `icmp.ult`, `sext` and `zext`). A value of
  `iN` is N bits. Where an operation reads it with a sign, the bits are
  two's complement.
- **`i1`** is 0 or 1. In memory it is one byte; a `load i1` reads the
  lowest bit of that byte, a `store` of an `i1` writes 0 or 1.
- **`f32` is the 32 bits of its float everywhere**: in a register, in
  memory, in a constant. No engine widens it to a double where the IR
  does not convert, so a signalling NaN keeps its bits through `load`,
  `store`, `bitcast`, `fneg`, copies and calls.
- **`ptr`** is opaque: it has no pointee type (the type is the one of the
  `load` or `store`). In STRICT memory (§ 9) a pointer returned by
  `mem_alloc` may carry tag bits that are not part of the address (Meri
  keeps a generation there); see `ptrtoint` (§ 7.5).
- **`str`** and **`ref`** are handles counted by the engine (§ 12). The
  handle 0 is a valid value: the `str` 0 is the empty string `""`, the
  `ref` 0 is the `BigInt` 0. Zeroed memory therefore already holds valid
  strings and `BigInt`s. No instruction reads a `ref` as a number (§ 12).

### 2.2 Aggregate types

Aggregates exist in memory only: no value has an aggregate type.

- **Struct**: a named type with an **explicit layout**, written by the
  front end: a size, an alignment, and fields `(type, offset)`. The rules:
  - the alignment is a power of two and the size a multiple of it;
  - every field type is a type defined **before** the struct (a smaller
    id), not `void`, not a function type;
  - every field lies inside the struct and is aligned to its own type;
  - fields may overlap (a union is a struct whose fields share offsets),
    and their order is free;
  - two structs never share a name.
- **Array** `[N x T]`: N elements of type T, one after the other, with no
  padding but the one T has. T is defined before the array, not `void`,
  not a function type. Its size is N times the size of T, its alignment
  that of T. N may be 0 (an array whose length is computed at run time is
  written `[0 x T]`), except where § 12.3 forbids it.
- Arrays and function types are **interned**: two equal ones have the
  same id. A struct is identified by its name.

### 2.3 Function types

`fn(T1, T2, ...) -> R`, with an optional `...` at the end (variadic).

- The function types of the **functions of the module** and of
  `call.ind` take and return scalars only (`R` may be `void`), and are
  never variadic.
- The function type of an **extern** (§ 3.5) may also take and return a
  struct (passed by value as C does), may be variadic, and may mark each
  parameter and the result of type `i8`, `i16` or `i32` with `sext` or
  `zext` (§ 3.5). The marks are part of the identity of the type.

## 3. The module

### 3.1 Header

| Field | Values | Meaning |
|---|---|---|
| `memory` | `strict` (default), `fb` | the rules of pointers and memory (§ 9). `fb` is accepted by the verifier but no engine runs it yet: `lir_run` refuses such a module as unsupported |
| `ptr_bits` | 64 (default), 32 | the bits of `ptr`, `str` and `ref`; their sizes and alignments follow it, and so does every layout the front end computes, the relocations and the slots. Set before any type is made (`limba_module_ptr_bits`). `lir_run` refuses 32 as unsupported until a target of 32 bits exists |
| name | a string, optional | the name of the module, informative |
| `language` | a string, optional | the prefix of the messages of the program's traps (`luxia`); without it an engine uses its own (`lir_run`, `meri`) |
| `target` | a platform, optional | the platform whose C the externs follow: `x86_64-linux`, `aarch64-linux`, `x86_64-windows`. Written when the externs pass C types that differ between platforms. Its pointer width must equal `ptr_bits` (all three are 64 today). An engine refuses a module of another platform |
| messages | `code → text` | how the language words the traps: each code exists in `traps.def`, appears once, and has a non-empty text, which replaces the text of `traps.def`. A module cannot make new traps |
| exceptions | `id, name, family` | the exceptions the language names (§ 13.1) |

### 3.2 Strings

A table of interned byte strings, any bytes, by id. Names, string
constants, symbols of C, file names of positions all point into it. Two
equal strings have the same id.

### 3.3 Names

Functions, globals and externs share one name space: a name is used once.
A name is any sequence of bytes (in the text form a name outside
`[A-Za-z0-9_.$]`, or that starts with a digit, is quoted, § 14).

### 3.4 Globals

A global has a name, a type (any type but `void` and function types),
flags, and an initial value of one of these kinds:

| Kind | Allowed for | Initial value |
|---|---|---|
| `ZERO` | any type | all bytes zero (the value field is 0) |
| `INT` | an integer type or `ptr` | the integer, written in the width of the type |
| `FLOAT` | `f32`, `f64` | the bits of the float (for `f32` the 32 bits, zero-extended in the field) |
| `STR` | `str` | an immortal string of the module (§ 12.1) |
| `DATA` | any type | bytes, fills and relocations, below |

A `DATA` initial value is, in this order:

1. its first `len` bytes, in the layout of the type: little endian,
   reals as their bits; `len` is at most the size of the type;
2. **fills**: a block of `len ≥ 1` bytes written `count ≥ 1` times one
   after the other from `offset`; each fill starts after the bytes and
   after the fill before it, and ends inside the type;
3. **relocations**, in increasing offsets, never overlapping, each at
   the start of a `ptr` (for `func` and `global`) or of a `str` (for
   `str`) of the layout, searched inside structs and arrays, where the
   bytes are zero:
   - `func f`: the value `faddr f` gives;
   - `global g + addend`: the value `gaddr g` gives, plus `addend` bytes,
     with `0 ≤ addend ≤` the size of `g`;
   - `str "s"`: an immortal string of the module.

   A relocation is as wide as the type at its place (`ptr_bits`).

Everything else is zero. Every `str` and `ref` of the layout that no
relocation writes has zero bytes. Two globals never share the same data.
A relocation may name any global, also one that comes later: an engine
first places every global, then writes the initial values.

Flags: `export` and `const` are carried by both forms; version 9 gives
them no meaning an engine or the optimiser relies on (§ 18).

Globals live for the whole run and are never released (§ 12).

### 3.5 Externs

An extern is a C function: a name in the IR, a function type (§ 2.3),
the C symbol, and a library (a logical name, `"c"` for the C library of
the process; none means resolved from the process).

- Its parameters and result are integers, reals, `ptr`, or structs of
  them of at least one byte (arrays of them inside), never a counted
  handle nor a struct that holds one; the result may be `void`.
- `i1` is C's `_Bool`: one byte, 0 or 1.
- `sext` and `zext` on an `i8`, `i16` or `i32` parameter or result: the
  caller extends the integer in its register with the sign or with
  zeros, as the C ABI of the target wants (LLVM's `signext` and
  `zeroext`). Without a mark the sign is used, the canonical form.
- An engine opens every library and resolves **every** symbol before the
  first instruction runs; if one is missing it stops at once, naming the
  symbol and the library. The paths of the libraries come from the
  engine (command line, configuration), never from the module.

### 3.6 Functions

A function has a name, a function type of scalars (§ 2.3), flags
(`export`, informative), slots, and blocks. The rules of the body are in
§ 4.

The program starts at the function named **`main`**, which takes no
parameters. Its result, if any, is not the exit status (§ 5.3).

### 3.7 Positions

A table of source positions `(file, line, column)`, the file a string
id, line and column from 1. A position is referred to by its index **from
1**; 0 means none. Every instruction may carry one; an engine uses it in
the message of a trap. The positions do not change the meaning of the
program.

## 4. Functions, blocks and values

### 4.1 Blocks

- A function has at least one block. **Block 0 is the entry**: its
  parameters are the parameters of the function, with their types, and
  nothing branches to it.
- A block is not empty. Its parameters come first, then ordinary
  instructions, then exactly one terminator, and no terminator before
  the end.
- The terminators are `br`, `cbr`, `switch`, `ret`, `unreachable`,
  `trap` and `raise`.
- A block may be unreachable from the entry; it is still verified, but
  the dominance of its operands is not checked.

### 4.2 Values and dominance

- An operand is a value: an instruction that produces one (not
  `NO_RESULT`, not of type `void`), defined in the same function.
- Every operand **dominates** its use: defined earlier in the same block,
  or in a block that dominates the block of the use. The error edges of
  the handlers (§ 13.4) count for dominance.
- A **branch argument** is a value passed to a parameter of the target
  block; its type equals the type of the parameter. All the arguments of
  a branch are read first and then written, as one parallel copy.

### 4.3 Slots

A slot is memory of one call of the function: a size, an alignment (a
power of two), and an optional type.

- `slot $k` gives its address. Each call has its own slots; they live
  until the call ends, by `ret` or by an exception that leaves it.
- **Every slot is zeroed at the entry of the function**, typed or not.
- A **typed slot** names a type that holds a counted handle (§ 12.3), no
  larger than the slot and no more aligned; at every exit of the call,
  normal or by exception, the engine releases the handles it holds.
- A trap that no handler catches ends the program (§ 5.3): nothing is
  released after it.

### 4.4 Handlers

A block may have a **handler**: another block of the same function that
receives what the block raises (§ 13). The entry block is never
protected and never a handler. A handler has exactly three parameters,
`(i32 identity, i32 position, str message)`, and is reached by errors
only: no branch names it. A handler may be protected in turn.

## 5. Execution

### 5.1 One thread of control

Version 9 has one thread of control: there are no atomic operations, no
fences, no thread-local storage. `par_for` (§ 15.10) runs its iterations
in sequence, from `lo` to `hi`, in every engine. A version that runs them
in parallel will add the attribute `thread_safe` to the run-time table and
the rules of the bodies with it.

### 5.2 Running a function

A call zeroes the slots of the callee, gives the arguments to the
parameters of block 0 and runs block 0. Instructions run in order; a
terminator chooses the next block (and the arguments of its parameters)
or ends the call.

An argument passed to `call`, `call.ind` or `call.rt`, or to a block
parameter, is a new value: if it is a `str` or a `ref`, it owns its
reference, like any other value (§ 12.1). Whether an engine takes a new
reference or lends the caller's for the duration of the call cannot be
observed.

### 5.3 How a program ends

| End | Exit status | Message |
|---|---|---|
| `main` returns | 0 | none |
| `halt(c)` | `c` | none |
| a trap or an exception no handler catches | 1 | yes (§ 13.7) |
| a write to a closed output (`EPIPE`) | 141 | none (§ 15.1) |
| an error of writing found at the end (the output, a file left open) | 1 | the trap `IO` without a position |
| an error of the engine: `unreachable` executed, a wrong module, a module the engine does not support, a limit of the engine | not 0 (1 in `lir_run`) | the engine's own, with its own prefix |

Engine errors are not exceptions: no handler catches them.

### 5.4 Limits of the engines

- **Depth of calls.** The deepest nesting of calls depends on the engine;
  past it is always the trap `STACK` (28), at the position of the call,
  never a crash. `lir_run`: 10 000 calls, or what its own stack of
  256 MiB holds.
- **Memory.** How much memory a program may use depends on the engine;
  past it is always the trap `NOMEM` (7), never a crash. `lir_run` counts
  the live blocks of `mem_alloc`, the slots of the live calls and the
  strings and `BigInt`s still held, against 1 GiB; a collection frees the
  strings and `BigInt`s no one holds before the budget runs out.
- **Steps.** `lir_run` stops after 100 million instructions (an engine
  error, "limit"); other engines have no such limit.

Where `STACK` and `NOMEM` happen is not part of the program's meaning: an
optimisation (inlining) may move them, and the differential tests do not
compare them.

### 5.5 The oracle

`lir_run` runs a verified module from `main` (or another function without
parameters, `--entry=`), slowly and simply on purpose. Its statuses:
returned, trap (with its code), halt, unreachable, limit, unsupported,
wrong module, and "memory rule broken" (with `--check-mem`, § 12: a
handle read or written as another type or in part, a release below zero,
counts that do not match memory at the end).

- It does not run `call.ext` (unsupported): the calls to C are compared
  only with expected outputs, which Meri runs. It does not run a module
  of `memory fb` or of 32 bits (unsupported).
- It reports a wrong module for an access outside every live object
  (§ 9.1) and for the run-time calls that skip their checks (§ 15.1).
- **Common mode.** `lir_run`, the folding of Limba and the engines share
  `limba/math.h`, the exact numbers of `BigInt` (`src/common/bigint.c`),
  and the text of numbers (`limba/fmt.h`, `limba/val.h`). An error there
  is the same in all of them and invisible to the differential tests
  (unoptimised against optimised, `lir_run` against Meri). Only
  independent references watch them: `test_math` (against mpmath) and
  `test_common` (against `strtod` and exact identities).

## 6. Operations

Each operation has a name in the text form, an operand **format**, and
**flags** (`ir_ops.def`). The number of an operation in the binary form is
its position in `ir_ops.def`, from 0 (appendix A).

### 6.1 Flags

| Flag | What an optimisation may assume |
|---|---|
| `PURE` | no effect: the result depends on the operands only; it may move, merge with an equal one, or be removed when unused |
| `COMMUTATIVE` | the two operands may swap |
| `MAY_TRAP` | may raise a trap; never removed, merged or moved across what it could reorder with, except as § 17 says |
| `READS_MEM` | reads memory |
| `WRITES_MEM` | writes memory |
| `TERMINATOR` | ends a block |
| `NO_RESULT` | produces no value |
| `CALL` | a call: what it does is that of the callee (§ 8, § 15) |

### 6.2 Formats

| Format | Operands | Immediates |
|---|---|---|
| `ICONST` | none | `imm` = the value |
| `FCONST` | none | `imm` = the bits of the float of the result type (for `f32` the 32 bits, zero-extended), never widened |
| `SCONST` | none | `imm` = a string id |
| `TYPED` | none | the result type only |
| `UN` | one value | |
| `BIN` | two values | |
| `TERN` | three values | |
| `CMP` | two values | the condition `cc` (appendix B) |
| `CONV` | one value | the result type is the target |
| `LOAD` | an address | |
| `STORE` | a value, an address | |
| `SLOT` | none | `imm` = slot index |
| `GADDR` | none | `imm` = global id |
| `FADDR` | none | `imm` = function id |
| `ADDR` | base, index | `imm` = scale, `imm2` = displacement |
| `MEM3` | destination, source (or byte), length | |
| `CALL` | arguments | `imm` = function id |
| `CALL_IND` | callee, arguments | `imm` = function type |
| `CALL_EXT` | arguments | `imm` = extern id |
| `CALL_RT` | arguments | `imm` = run-time id |
| `BR` | target block, n, n arguments | |
| `CBR` | condition; then (block, n, args); else (block, n, args) | |
| `SWITCH` | value, default block, n, n × (low 32 bits of the case value, high 32 bits, block) | |
| `RET` | zero or one value | |
| `NONE` | none | |
| `TRAP` | none | `imm` = code |
| `CHECK` | condition | `imm` = code |
| `PARAM` | none (a block parameter) | |
| `RC` | pointer, count | `imm` = a type |
| `RAISE` | identity, position, message | |

The condition `cc` is 0 for every operation that is not `icmp` or `fcmp`.
Immediates a format does not use are 0 (§ 18).

## 7. Values and arithmetic

In this section N is the width of the integer type, `T` the result type.

### 7.1 Constants

| Operation | Result | Meaning |
|---|---|---|
| `iconst T k` | an integer type | `k`, written in N bits. `k` must fit: in `[-2^(N-1), 2^N - 1]` (either spelling of the same bits), 0 or 1 for `i1` |
| `fconst T x` | `f32`, `f64` | the float whose bits the immediate carries (for `f32` at most 32 bits) |
| `sconst str "s"` | `str` | an immortal string with those bytes (§ 12.1) |
| `null T` | `ptr`, `str`, `ref` | 0: the null pointer, the empty string, the `BigInt` 0 |

### 7.2 Integer operations

Operands and result have the same integer type `T`. The `.ov` and
division operations may trap; the others never do.

| Operation | Meaning |
|---|---|
| `add`, `sub`, `mul` | modulo 2^N |
| `neg a` | `0 - a` modulo 2^N |
| `sdiv a, b` | `a / b` with sign, truncated toward zero. Trap `DIVZERO` (11) if `b = 0` or `a = -2^(N-1), b = -1` |
| `srem a, b` | the remainder of `sdiv`, with the sign of `a`. Trap 11 in the same two cases |
| `udiv a, b`, `urem a, b` | without sign. Trap 11 if `b = 0` |
| `add.ov`, `sub.ov`, `mul.ov` | as `add`, `sub`, `mul` on numbers with a sign; trap `OVERFLOW` (6) if the exact result is outside `[-2^(N-1), 2^(N-1) - 1]` |
| `uadd.ov`, `usub.ov`, `umul.ov` | as `add`, `sub`, `mul` on numbers without a sign; trap `OVERFLOW` (6) if the exact result is outside `[0, 2^N - 1]` |
| `and`, `or`, `xor`, `not` | bitwise |
| `shl a, c` | `a` shifted left by `c mod N` |
| `lshr a, c` | `a` shifted right by `c mod N`, zeros in |
| `ashr a, c` | `a` shifted right by `c mod N`, copies of the sign bit in |

The shift count is read **without a sign** and taken **modulo the width
of the type**, for every type, `i8` and `i16` included (decision N4 of
2 Nov 2026). A target whose instruction masks the count to 5 or 6 bits
masks it to the width instead.

There is no `neg.ov`: a checked negation is `sub.ov 0, a`.

On `i1` the signed operations are not consistent in version 9 (§ 18).

### 7.3 Floating point

IEEE 754, in the precision of the type, rounding to nearest, ties to
even. No operation traps; no flag of IEEE 754 is observable.

| Operation | Meaning |
|---|---|
| `fadd`, `fsub`, `fmul`, `fdiv` | the IEEE operations (a division by zero gives an infinity or a NaN) |
| `fneg a` | `a` with its sign bit flipped: copies the other bits, a NaN's included |
| `fma a, b, c` | `a × b + c`, rounded once |
| `fround a` | to an integral value, ties to even (`roundToIntegralTiesToEven`, WebAssembly's `nearest`) |
| `fround.away a` | to an integral value, ties away from zero (`roundToIntegralTiesToAway`): Ada's rounding |

**NaN.** The sign and the payload of a NaN that an arithmetic operation
**produces** are not specified, as in IEEE 754-2019 § 6.3 and WebAssembly;
engines and CPUs may differ there. The operations that copy bits (`fneg`,
`bitcast`, `load`, `store`, copies, arguments) keep them. Printing writes
every NaN as `nan`.

`fadd` and `fmul` are not marked commutative: with two NaN operands the
result is the NaN of one of them, and which one may depend on the order.

`fma` and `fround.away` may be slow on some targets (`fma` is software on
Cortex-M0+ and on WebAssembly without relaxed SIMD; `fround.away` has no
single instruction in WebAssembly nor in SSE): the optimiser never
introduces either (§ 17).

### 7.4 Comparisons and select

`icmp.cc a, b` and `fcmp.cc a, b` give an `i1`; the two operands have the
same type: an integer type or `ptr` for `icmp`, a float type for `fcmp`.

| `icmp` | True when |
|---|---|
| `eq`, `ne` | the bits are equal, different |
| `slt`, `sle`, `sgt`, `sge` | the order of the numbers with a sign |
| `ult`, `ule`, `ugt`, `uge` | the order of the numbers without a sign |

`eq` and `ne` on `ptr` are exact: a dangling pointer is different from a
pointer to a new object, and the comparison never traps. The other
conditions on `ptr` compare addresses within one object (§ 18).

| `fcmp` | True when |
|---|---|
| `oeq`, `one`, `olt`, `ole`, `ogt`, `oge` | neither operand is a NaN and the relation holds (ordered) |
| `ord` | neither operand is a NaN |
| `uno` | an operand is a NaN |
| `ueq`, `une`, `ult`, `ule`, `ugt`, `uge` | an operand is a NaN, or the relation holds (unordered) |

`select c, a, b`: `a` if the `i1` `c` is 1, else `b`; `a`, `b` and the
result have the same scalar type.

### 7.5 Conversions

The result type is the target.

| Operation | From → to | Meaning |
|---|---|---|
| `trunc` | wider integer → narrower integer | the low bits |
| `zext` | narrower integer → wider integer | zeros above |
| `sext` | narrower integer → wider integer | copies of the sign bit above (an `i1` 1 gives all ones) |
| `fptrunc` | `f64` → `f32` | rounded to nearest, ties to even |
| `fpext` | `f32` → `f64` | exact |
| `sitofp` | integer → float | the number with a sign, rounded once |
| `uitofp` | integer → float | the number without a sign, rounded once |
| `fptosi` | float → integer | toward zero, **saturating**: past the largest value of the type with a sign, the largest; below the smallest, the smallest; NaN gives 0 |
| `fptoui` | float → integer | toward zero, **saturating** without a sign: negatives and NaN give 0, past `2^N - 1` the largest |
| `bitcast` | `i32` ↔ `f32`, `i64` ↔ `f64` | the same bits |
| `ptrtoint` | `ptr` → integer | below |
| `inttoptr` | integer → `ptr` | below |

`fptosi` and `fptoui` are pure: a language that wants an error checks the
operand first (`fcmp` and `check`). They are WebAssembly's `trunc_sat`
and Rust's `as`.

No conversion takes or gives a `str` or a `ref`.

**`ptrtoint p`** gives an integer that tells live objects apart: equal for
equal pointers, different for pointers to different live objects or to
different places of one, when the target type is as wide as `ptr`. It is
**not an address**: in STRICT memory a
pointer may carry tag bits (§ 2.1). It is truncated to the target type.
It is not folded, except `ptrtoint null`, which is 0. (`lir_run` gives
the address, which satisfies these rules.)

**`inttoptr k`** reads `k` without a sign. `inttoptr (ptrtoint p)` at the
full width of `ptr` gives `p` back. Any other result of `inttoptr` is an
address the program reaches outside the rules of § 9: no front end of
the family emits it in STRICT memory; reading or writing through it is a
wrong module unless it lands in a live object.

## 8. Calls

| Operation | Callee | Rules |
|---|---|---|
| `call T @f(args)` | the function `f` of the module | the arguments have the types of the parameters, the result type is that of `f` |
| `call.ind T S p(args)` | the function whose address `p` holds; `S` its function type | `p` is a `ptr` made by `faddr` or by a `func` relocation of a function whose type is exactly `S`; anything else is a wrong module. `S` has scalars only |
| `call.ext T @e(args)` | the extern `e` (§ 3.5) | the C ABI of the platform of the module |
| `call.rt T name(args)` | a function of the run-time library (§ 15) | the signature of `runtime.def` |

- The result type of the instruction is the result type of the callee
  (`void` for none).
- A call to a variadic extern may pass more arguments than parameters,
  of any scalar type.
- **`call.ext`**:
  - a struct parameter is passed as a `ptr` to its bytes; the engine
    copies them as the ABI wants (in registers up to 16 bytes on SysV,
    in memory past them);
  - a struct result: the instruction has no value (`void`) and its
    **first operand** is the `ptr` where the engine writes the result;
  - a `ptr` argument is any address: an opaque pointer of C (memory of
    C: the IR never applies `ptr_live`, `load` or `store` to it), or the
    address of an object of the program, valid only during the call;
  - `call.ext` never raises an exception (§ 13.3).
- What a callee does to memory is unknown, unless the run-time table says
  otherwise (§ 15).

## 9. Memory

### 9.1 Objects

The memory of a program is made of **objects**:

- the slots of the calls running (§ 4.3);
- the globals (§ 3.4);
- the blocks of `mem_alloc` not freed (§ 15.4);
- the C strings of `cstr_new` not freed (§ 15.8);
- the bytes of a string, with the NUL after them, while the string is
  held (`str_ptr`, § 15.2).

Every access of n > 0 bytes (`load`, `store`, `load.inv`, `memcpy`,
`memset`, `retain`, `release`, and the run-time functions that take a
pointer) lies **entirely inside one object alive**. An access that does
not is a wrong module. An access of 0 bytes reads and writes nothing, and
its pointers may be null.

Memory is **little endian**; a real is its bits. A load or store needs no
particular alignment, except the counted handles in memory (§ 12.3).

### 9.2 Addresses

| Operation | Result | Meaning |
|---|---|---|
| `slot ptr $k` | `ptr` | the address of slot k of this call |
| `gaddr ptr @g` | `ptr` | the address of global g |
| `faddr ptr @f` | `ptr` | the address of function f, for `call.ind`, `par_for` and data |
| `addr ptr p, i, scale, disp` | `ptr` | `p + i × scale + disp`, modulo `2^ptr_bits`; `i` is any integer type read with a sign, `scale` in `[0, 2^31 - 1]`, `disp` any 64-bit integer |

### 9.3 Loads and stores

- `load T p`: the value of type T (any scalar but `void`) at `p`.
- `load.inv T p`: the same, for memory that does not change while the
  object that holds it is alive (the bounds at the head of an array of
  `mem_alloc`). An engine runs it as `load`; an optimisation may move it
  past stores, never past what may free the object (`mem_free`, a call).
- `store v, p`: writes the value `v` (any scalar) at `p`.
- `memcpy dst, src, n`: copies `n` bytes; **the ranges may overlap**
  (it is `memmove`).
- `memset dst, b, n`: writes the low 8 bits of `b` into `n` bytes.
- In `memcpy` and `memset`, `n` is any integer type, read **without a
  sign**; `b` any integer type.
- `load`, `store`, `memcpy` and `memset` have no trap of their own; the
  traps of an access are the `check`s before it (§ 9.4). They carry
  `MAY_TRAP` so that no pass removes or moves them until the rules of
  § 9.5 are in use.

### 9.4 Checks before an access (STRICT)

The checks of nil, bounds and dangling pointers are instructions **before
the access**, never part of it:

- **nil**: `icmp.ne p, null`, then `check …, 102`;
- **dangling**: `v = call.rt i1 ptr_live(p)`, then `check v, 105`, right
  before the access, after everything the access evaluates, with no call
  in between (a call could free the block). `ptr_live` takes the pointer
  `mem_alloc` returned (the start of the block), never an interior one.
  The pair `ptr_live` + `check 105` written one after the other is a
  canonical form: an engine may fuse it into one instruction;
- **bounds**: a `check` with code 100 (or the language's).

A check is needed only for a pointer that can be nil or dangling: the
address of a slot, of a global, a `var` parameter, the environment of a
nested routine need none. An access that skips a check it needed is a
wrong module, not a trap. The verifier cannot refuse it statically: a
valid program contains `store` to `null` after a `check 102` that always
fails.

**Canonical access to an array of `mem_alloc`** (Luxia's `new`), emitted
contiguous by the front end: nil (`null`, `icmp.ne`, `check 102`);
dangling (`ptr_live`, `check 105`); `load.inv i64 p` (the low bound),
`addr p, 0, 0, 8`, `load.inv i64` (the high bound); the index check
(100); `addr` of the element. The head holds, for an array of n indices,
`lo_k` at `16·(k−1)` and `hi_k` at `16·(k−1) + 8`; the cells start at
`16·n`, by rows. Before `mem_alloc` the front end checks that
`16·n + cells × size` does not exceed `2^63 - 1` (`check …, 7`).

### 9.5 Rules for producers and optimisations

Decided on 2 Nov 2026 (N3). They say what an optimisation may assume;
the optimiser of version 9 does not use them yet, and a front end already
respects them.

1. A slot whose address never reaches a `store` (as the value stored),
   a `ret`, a `ptrtoint`, a block argument or a call is **private** to
   its call: nothing but the instructions of that call reads or writes
   it.
2. An address computed by `addr` from `p` stays **inside the object of
   `p`**. (`lir_run` checks only that every access lies inside some live
   object, § 18.)
3. The result of `inttoptr` may point anywhere.
4. A `call.ext` reads and writes only memory whose address it receives
   or that has escaped (rule 1).
5. The error edges (§ 13.4) read all memory: a store before them is
   observable by the handler. A promotion of slots to values (mem2reg)
   therefore never turns a slot that a handler reads into a value defined
   inside a protected block: the front end keeps there the variables a
   handler reads (Luxia, step 4).

### 9.6 The `fb` model

`memory fb` (a pointer is a plain address, `mem_free` is C's `free`,
no checks) is reserved for another language; no engine runs it in version
9, and `lir_run` refuses it as unsupported.

## 10. Control flow

| Operation | Meaning |
|---|---|
| `br bN(args)` | jumps to block N, passing the arguments |
| `cbr c, bT(args), bE(args)` | to `bT` if the `i1` `c` is 1, else to `bE` |
| `switch v, bD [k1: b1, k2: b2, ...]` | to the block of the case equal to `v`, else to `bD`. `v` is an integer; each case is a value that fits the type of `v` (as `iconst`), stored as two 32-bit halves; no case appears twice. The targets of a `switch` take no arguments |
| `ret` / `ret v` | ends the call, with the value of the function's result type (none for `void`) |
| `unreachable` | executing it is an error of the engine (§ 5.3) |

No branch names block 0 or a handler.

## 11. Traps and checks

| Operation | Meaning |
|---|---|
| `trap k` | raises the exception `k` (a terminator) |
| `check c, k` | raises the exception `k` if the `i1` `c` is 0 |

`k ≥ 0` is an exception identity (§ 13.1): a code of `traps.def`, or an
identity the module's table names. The trap carries the position of the
instruction.

## 12. Strings and counted handles

The rules of this section hold for both counted handles, `str` and
`ref`.

### 12.1 Ownership

- A value of type `str` or `ref` owns one reference.
- `sconst`, a `STR` global, a `str` relocation and the message of a trap
  (§ 13.6) are **immortal**: the counts never touch them and they are
  never freed. An engine makes the string of an `sconst` once, however
  often it runs.
- `ret` and `raise` give the reference of their value away; an argument
  owns its own (§ 5.2).
- A `ref` (`BigInt`) is **immutable**: every function gives a new value,
  none changes an existing one. No operation reads a `ref` as a number:
  `icmp` takes integers and `ptr` only, no conversion takes a `ref`;
  equality of two `BigInt`s is `big_cmp`. An engine may therefore share
  or reuse values unseen.

### 12.2 Handles in memory

A handle reaches memory only by:

1. `store` of a `str` or `ref`;
2. `read_line` (a `str`) and `str_to_big` (a `ref`);
3. `memcpy` from memory that holds handles;
4. the initial value of a global (`STR`, a `str` relocation), immortal;
5. zero bytes: the handle 0.

The bytes of a handle in memory are read only by a `load` of its own type
or by `memcpy`; never by a `load` or `store` of another type, never in
part. A handle in memory starts at an address aligned to its size.
`call.ext` is outside this rule.

| Operation | Counts |
|---|---|
| `store v, p` of a handle | takes a reference on `v`, releases the handle that was at `p`, then writes |
| `load T p` of a handle | the value takes a reference |
| `read_line(p)`, `str_to_big(s, p)` | as `store` of the handle made |
| `memcpy`, `memset`, `mem_free` | no count: bytes only |
| `retain T p, n` | one reference more on every handle of `n` elements of type T from `p` |
| `release T p, n` | one reference less on each |
| end of a call | the typed slots release what they hold |

`retain` and `release` do not change the bytes: whoever reuses the
memory rewrites it.

### 12.3 `retain` and `release`

- `T` holds at least one counted handle and **no array of 0 elements**,
  at any depth (it would count nothing silently). The same holds for the
  type of a typed slot.
- `n` is any integer type, read with a sign. A negative `n` is the trap
  `RANGE` (101) in every engine; so is an `n` whose `n × size(T)`
  overflows.
- The `n` elements lie in one live object (§ 9.1).
- An array whose length is computed at run time passes the element type
  as `T` and the length as `n`.

How a front end lowers a type that holds handles: assignment,
initialisation, `return` of a record, a parameter by value are
`retain T src, n`, `release T dst, n`, `memcpy dst, src` in this order (so
`r := r` frees nothing before the copy); a `dispose` is `release` right
before `mem_free`. Globals are never released.

### 12.4 The string pointer

`str_ptr(s)` gives the address of the bytes of `s`, followed by a NUL. It
and every address derived from it are valid while `s` holds a reference:
an engine that counts keeps `s` alive where they are used. `str_ptr` is
pure: it may move out of a loop.

## 13. Exceptions

### 13.1 Identities and the table

- An exception identity is an `i32`. **Below 1000** they are Limba's:
  the codes of `traps.def` (§ 16) and the four predefined families
  900-903. **From 1000** they are the program's: exceptions and families.
- A program (its library included) may raise the predefined ones too.
- The module's table has one entry per identity the language uses:
  `exception ID "Name" [parent ID]`. The verifier checks that:
  - identities are distinct and not negative;
  - those below 1000 exist in `traps.def`, and their family, if given,
    is the one of `traps.def`;
  - names are not empty;
  - families exist (in the table, or predefined) and form no cycle;
  - no identity of the program (≥ 1000) has 901 (`StorageError`) among
    its families.
- Families are static: the `when` branches of a language are a `switch`
  of the front end on the identity.

### 13.2 An occurrence

An exception on its way carries three values: its identity (`i32`), its
position (`i32`, an index of the positions, 0 for none) and its message
(`str`).

`raise id, pos, msg` (a terminator) raises one; it gives the reference of
`msg` away. A position past the table counts as none. Raising again in a
handler is `raise` with the handler's three parameters.

### 13.3 What goes to the handler

In a protected block, these go to the block's handler:

- every trap an instruction of the block raises, if it can be caught;
- every exception that leaves a call to a function of the program
  (`call`, `call.ind`, `call.rt par_for`) or a trap of a run-time call;
- `raise`.

Never caught, anywhere: the codes 7 (`NOMEM`) and 28 (`STACK`) and the
family 901, even when the program raises them (the program stops, no
handler runs, no `finally`); `halt`; a closed output (141); `unreachable`;
the errors of the engines (wrong module, limits). `call.ext` never raises.
`IO` (107) and `INVALID_HANDLE` (109) can be caught.

Outside a protected block the exception leaves the function, to the
handler of the block of the call in the caller; at the top it ends the
program.

### 13.4 Handlers and dominance

For dominance, each error edge starts from the **predecessors** of the
protected block (repeated for handlers that are protected in turn), not
from the block itself: a handler sees nothing the protected block
defines, and receives everything through its three parameters.

### 13.5 Unwinding

- A call left by an exception gives back its slots and releases what its
  typed slots hold.
- The handlers crossed (a `finally` raises again) run in every engine,
  also when the exception is in the end not caught.
- After an exception no handler catches, the counts of live values are
  not compared.

### 13.6 The message of a trap

When a trap goes to a handler, its message is the text the module gives
its code, or else the text of `traps.def`. That `str` is **immortal**: it
takes no memory, counts neither in the memory limit nor among the live
strings, and the counts never touch it. Raising an exception never takes
memory.

### 13.7 An exception no handler catches

The program ends with exit status 1 and one line on standard error:

- `<prefix>: <Name>: <message> at <file>:<line>:<column>`;
- `<prefix>: <Name> at <position>` when the message is empty;
- without `at …` when there is no position;
- a trap whose identity the table does not name keeps the form
  `<prefix>: <text> at <position>`, the text the module's or that of
  `traps.def`;
- an identity with neither a name nor a text: `<prefix>: exception <id>`.

`<prefix>` is the module's `language`, else the engine's name. Every
engine words it with `limba_report_uncaught` (`limba/ir.h`), and a
position with `limba_pos_print`. The table may name every code of
`traps.def`, whether the program raises it or not: then every trap of
the program has its name in this line.

### 13.8 The set "under a handler"

Limba computes, for each function, whether an exception may cross it on
its way to a handler (`limba_module_handled`): the roots are the
functions with a protected block and the bodies given to `par_for`; then
whatever a function of the set calls directly, and, for a `call.ind` of
signature S, every function of signature S whose address the module
takes (`faddr` or a `func` relocation). The whole program is in the
module. The set is not part of the `.lir`; it is computed when asked.

## 14. The file forms

Both forms carry the same content. A module read from a file is always
verified before it runs; reading and verifying trust nothing: a damaged
file makes them say no, never crash.

### 14.1 The text form (`.lit`)

The text form carries the same content as the binary one and reads back
to it exactly. It is UTF-8 text; `;` starts a comment to the end of the
line.

**Tokens.** `@name` (a function, global or extern), `%name` (a struct),
`v12` (a value), `b3` (a block), `$0` (a slot), `!5` (a position),
integers (`-12`, `0x1f`), reals (`1.5`, `0x1.8p+1`, `inf`, `-inf`,
`nan.0x7ff8000000000000`), strings `"..."` with the escapes `\n`, `\t`,
`\"`, `\\`, `\xNN`. A name outside `[A-Za-z0-9_.$]`, or starting with a
digit, is quoted: `@"Lists.Max[Float64]"`, `%"a b"`.

**Module lines**, printed in this order (`ptr_bits` must come before any
type, `pos` before the functions): an optional `module "name"`; `memory
strict|fb`; `ptr_bits 64|32` (before any type); `language "luxia"`;
`target "x86_64-linux"`; `message CODE "text"`; `exception ID "Name"
[parent ID]`; then the `type`, `global`, `extern`, `pos` and `func`
declarations:

```
type %rec = { i64 @0, str @8 } size 16 align 8
global @g : i64 = 42 export const
global @s : str = "immortal"
global @t : %rec = data "\x01" reloc 8 str "s"
global @a : [4 x i32] = data "" fill 0 4 "\x07\x00\x00\x00"
extern @puts : fn(ptr) -> i32 sext = "puts" in "c"
pos 1 "prog.luxia" 12 5
```

**Types**: `i1` … `ref`, `%name`, `[N x T]`, `fn(T1, T2 zext, ...) -> R
sext`.

**Functions**:

```
func @main : fn() -> i64 export {
  slot 16 align 8 type %rec
b0:
  v0 = iconst i64 1
  v1 = slot ptr $0
  store v0, v1 !1
  br b1(v0)
b1(v2: i64) handler b2:
  v3 = icmp.sgt i1 v2, v0
  check v3, 101
  ret v2
b2(v4: i32, v5: i32, v6: str):
  raise v4, v5, v6
}
```

A value-producing instruction is `vN = op[.cc] TYPE operands`; one
without a value omits `vN =` and the type, except calls, which always
write their result type (`call.rt void print_nl()`). Operands by format:
`iconst i64 5`; `fconst f64 0x1.8p+1`; `sconst str "s"`; `null ptr`;
`add i64 v1, v2`; `icmp.slt i1 v1, v2`; `fcmp.olt i1 v1, v2`; `trunc i8
v1`; `load i64 v1`; `store v1, v2` (value, address); `slot ptr $0`;
`gaddr ptr @g`; `faddr ptr @f`; `addr ptr v1, v2, 8, 16`; `memcpy v1, v2,
v3`; `call i64 @f(v1)`; `call.ind i64 fn(i64) -> i64 v0(v1)`; `call.ext
i32 @puts(v1)`; `call.rt i64 str_len(v1)`; `br b1(v1)`; `cbr v1, b2,
b3(v4)`; `switch v1, b9 [1: b2, -1: b3]`; `ret v1`; `unreachable`; `trap
11`; `check v1, 100`; `retain %rec v1, v2`; `raise v1, v2, v3`. A suffix
`!k` gives the instruction position k.

**Canonical printing.** `limba` prints values `v0, v1, …` and blocks `b0,
b1, …` in their order, floats in hexadecimal (`%a`) or, for a NaN, as
`nan.0x` followed by its bits, so the value survives exactly. Reading
the printed text and printing it again gives the same text; text → binary
→ text gives the same text.

### 14.2 The binary form (`.lir`)

Integers are LEB128, unsigned unless said signed; a fingerprint is 8
bytes, little endian. "Optional id" is 0 for none, else the id + 1. In
order:

1. the magic `LIR\0` (4 bytes);
2. the version, 9; a reader refuses any other;
3. the fingerprint of `runtime.def`, then that of `ir_ops.def`: FNV-1a of
   64 bits (seed `0xcbf29ce484222325`, prime `0x100000001b3`) over every
   entry in order, its text with the NUL, then for the run time its
   signature with the NUL and its attributes (4 bytes), for the
   operations its format (1 byte) and its flags (2 bytes), little endian.
   A reader refuses a module written against other tables;
4. `memory` (0 strict, 1 fb), `ptr_bits`, the module name (optional id);
5. the strings: their number, then for each its length and bytes, in id
   order, none twice;
6. `language` (optional id); the messages: their number, then code
   (signed) and text id each; the exceptions: their number, then identity
   (signed), name id and family (signed, −1 for none) each; `target`
   (optional id);
7. the types after the scalars, in id order, none twice: a kind byte
   (11 struct, 12 array, 13 function), then for a struct its name, size,
   alignment, number of fields and `(type, offset)` of each; for an
   array its element and count; for a function a byte "variadic" (0 or
   1), the result type, a byte of the result's extension (0 none, 1
   `sext`, 2 `zext`), the number of parameters and `(type, extension
   byte)` of each;
8. the globals: their number, then for each name, type, flags (bit 0
   `export`, bit 1 `const`), a byte for the kind of initial value (0
   zero, 1 int, 2 float, 3 str, 4 data) and the value (signed); for data
   the value is 0 and is followed by the length and the bytes, the number
   of fills and `(offset, count, length, bytes)` of each, the number of
   relocations and `(offset, kind byte: 0 func, 1 global, 2 str, id,
   addend signed)` of each;
9. the externs: their number, then name, type, symbol and library
   (optional id) each;
10. the functions: their number, then for each name, type, flags, the
    number of slots and `(size, alignment, type as optional id)` of
    each, the number of blocks, and for each block its handler (optional
    block), the number of parameters and their types, the number of the
    other instructions and each of them: operation, type, `cc` (1 byte),
    `imm` (signed), `imm2` (signed), the number of operands and the
    operands. A value operand is the index of its instruction in the
    **canonical order** (block by block, parameters first), which is the
    order the reader creates them in; the block operands, the argument
    counts and the halves of the switch cases are written as they are;
11. the positions: their number and `(file, line, column)` of each; then
    for every function a byte (0 or 1) saying whether its instructions
    have positions, and if so the position of each, in canonical order;
12. nothing more: bytes after the end are refused.

Writing is deterministic: writing what was read gives the same bytes.

### 14.3 Verification

`limba_verify` checks the module level (types, layouts, globals and their
data, externs, signatures, the header, messages, exceptions, positions)
and every function: the shape of the blocks and of the operands of every
instruction, their types, the targets of the branches and their
arguments, the handlers, and the dominance of every use. The rules are
those of §§ 2-13; what depends on run-time values is left to the engines
(a wrong module).

## 15. The run-time library

`call.rt name(args)` calls a function of `runtime.def`. Its number in the
binary form is its position in `runtime.def`, from 0 (appendix C), and
the whole table has a fingerprint in every `.lir`. Signatures are written
in the types of the IR.

### 15.1 Attributes

| Attribute | What an optimisation may assume |
|---|---|
| `PURE` | depends on its arguments only (it may still allocate, with `ALLOC`) |
| `IO` | talks to the outside: never removed, moved or merged |
| `ALLOC` | returns a new object |
| `WRITES_MEM` | writes memory of the program (through its pointer arguments) |
| `MAY_TRAP` | may raise a trap other than `NOMEM` and `STACK` |
| `NORETURN` | does not return |
| `READS_HEAP` | reads which blocks of `mem_alloc` are alive: changed by `mem_free` and by any call that may free |
| `CALLS_PROGRAM` | calls a function of the program, and does what it does, exceptions included |

A function without attributes reads memory, may not write it, and has
no other effect. Every function that allocates (`ALLOC`) may end the
program with `NOMEM`, which no handler catches.

**Checks before the call.** Where a function needs a condition on its
arguments (a divisor not 0, a pointer not nil, a range inside its
array), the IR checks it **before** the call, with a `check`; a call that
breaks the condition is a wrong module, never a trap. Where n = 0 a
function reads no byte through its pointer arguments, which may then be
null.

**Output.** Every function of output writes to the standard output, in
the order of the calls. It may end the program at an error of writing,
found when its buffer is written, maybe later than the call that filled
it: the trap `IO` (107) **without a position**. On a closed output
(`EPIPE`) the engine ignores `SIGPIPE` and ends at the write that finds
it, with status 141 and no message. A differential test compares the
output only on runs without such errors.

### 15.2 Output and strings

| Function | Signature | Attributes | Meaning |
|---|---|---|---|
| `print_i64` | `(i64) -> void` | IO, MAY_TRAP | the decimal text of the number with a sign |
| `print_u64` | `(i64) -> void` | IO, MAY_TRAP | the decimal text of the number without a sign |
| `print_f64` | `(f64) -> void` | IO, MAY_TRAP | the text of `limba_fmt_f64` (`limba/fmt.h`), byte for byte |
| `print_f32` | `(f32) -> void` | IO, MAY_TRAP | the text of `limba_fmt_f32`: the shortest that reads back as the same `f32` |
| `print_str` | `(str) -> void` | IO, MAY_TRAP | the bytes of the string |
| `print_str_w` | `(str, i32) -> void` | IO, MAY_TRAP | spaces before the string up to the width, counted in code points (bytes that are not UTF-8 continuation bytes); none if the width is not larger |
| `print_nl` | `() -> void` | IO, MAY_TRAP | a newline, byte 10 |
| `print_bool` | `(i1) -> void` | IO, MAY_TRAP | `true` or `false` |
| `print_char` | `(i32) -> void` | IO, MAY_TRAP | the UTF-8 of the code point |
| `print_byte` | `(i8) -> void` | IO, MAY_TRAP | the byte itself |
| `str_concat` | `(str, str) -> str` | ALLOC | the bytes of the first, then those of the second |
| `str_len` | `(str) -> i64` | PURE | the number of bytes |
| `str_cmp` | `(str, str) -> i32` | PURE | -1, 0 or 1: the bytes compared without a sign, a prefix before the longer string |
| `str_mid` | `(str, i64, i64) -> str` | ALLOC | from byte `start` (from 0), `len` bytes; `start` below 0 is 0, past the end is the end; `len` below 0 or past the end takes the rest |
| `str_ptr` | `(str) -> ptr` | PURE | the address of the bytes, a NUL after them (§ 12.4) |
| `str_from_i64` | `(i64) -> str` | ALLOC | as `print_i64` |
| `str_from_u64` | `(i64) -> str` | ALLOC | as `print_u64` |
| `str_from_f64` | `(f64) -> str` | ALLOC | as `print_f64` |
| `str_from_f32` | `(f32) -> str` | ALLOC | as `print_f32` |
| `str_from_f64_fixed` | `(f64, i32) -> str` | ALLOC | the real in fixed notation with that many decimals, `limba_fmt_f64_fixed` (a `Float32` is widened exactly first) |
| `str_from_char` | `(i32) -> str` | ALLOC | the UTF-8 of the code point |
| `str_from_bool` | `(i1) -> str` | ALLOC | `true` or `false` |
| `pos_str` | `(i32) -> str` | ALLOC | position k of the table as `limba_pos_print` writes it, `file:line:column`; `""` for 0 or past the table (k read without a sign). A new string, as `str_from_i64` |

### 15.3 Input and the command line

| Function | Signature | Attributes | Meaning |
|---|---|---|---|
| `read_line` | `(ptr) -> i1` | IO, ALLOC, WRITES_MEM, MAY_TRAP | reads a line of the standard input and stores it at the pointer as a `str` (§ 12.2), without its `\n` and a `\r` before it; true. At the end of the input it stores `""` and gives false |
| `io_read` | `(ptr, i64) -> i64` | IO, WRITES_MEM, MAY_TRAP | reads up to n bytes of the standard input at `a`, from the stream of `read_line`, fewer only at its end; gives how many. The bytes past them are unchanged |
| `io_write` | `(ptr, i64) -> void` | IO, MAY_TRAP | writes the n bytes at `a` on the output of `print_*`, in order with them |
| `arg_count` | `() -> i32` | IO | the number of the program's arguments |
| `arg` | `(i32) -> str` | IO, ALLOC | argument i, from 1; `""` outside `1 .. arg_count` |
| `halt` | `(i32) -> void` | IO, NORETURN | ends the program with that exit status (§ 5.3); never 1 nor 141 (the front end checks it) |
| `str_to_i64` | `(str, ptr, i64, i64) -> i1` | WRITES_MEM | `val`: if the text is an integer of the language (spaces and tabs around, a sign, a literal) in `lo .. hi` (the last two operands), stores it as an `i64` at the pointer and gives true; else gives false and stores nothing. The rules of the text are `limba/val.h` |
| `str_to_u64` | `(str, ptr, i64, i64) -> i1` | WRITES_MEM | the same without a sign: `lo .. hi` read without a sign, `-0` is 0 |
| `str_to_f64` | `(str, ptr) -> i1` | WRITES_MEM | the same for an integer or a real, rounded once to `f64` (or `inf`, `-inf`, `nan`) |
| `str_to_f32` | `(str, ptr) -> i1` | WRITES_MEM | the same, rounded once to `f32` |

`n ≥ 0` for `io_read` and `io_write`, checked before. **The end of the
input is final**: once `read_line` gave false or `io_read` fewer than n,
`io_read` gives 0 and `read_line` false for ever. An error of reading
(not the end) is the trap `IO` at the position of the call.

### 15.4 Memory of the program

| Function | Signature | Attributes | Meaning |
|---|---|---|---|
| `mem_alloc` | `(i64) -> ptr` | ALLOC | a new block of n bytes, whose contents are **not specified** (`lir_run` zeroes them; the front end zeroes them with `memset`); n = 0 gives a valid block. Past the engine's memory, or for a negative n, the trap `NOMEM` |
| `mem_free` | `(ptr) -> void` | WRITES_MEM, MAY_TRAP | frees the block `mem_alloc` returned; `null` does nothing; anything else that is not a live block of `mem_alloc` (freed already, interior, another object) is the trap `INVALID_FREE` (106) |
| `ptr_live` | `(ptr) -> i1` | READS_HEAP | is `p` the start of a live block of `mem_alloc`? False for `null`, a freed block, a slot, a global |

A freed block is never confused with a new one: a dangling pointer stays
different from every pointer to a new object (`lir_run` never reuses the
addresses of freed blocks; Meri compares a generation).

### 15.5 Mathematics

| Function | Signature | Attributes |
|---|---|---|
| `math_sqrt`, `math_sin`, `math_cos`, `math_tan`, `math_atan`, `math_exp`, `math_ln`, `math_trunc`, `math_floor`, `math_ceil` | `(f64) -> f64` | PURE |
| `math_pow` | `(f64, f64) -> f64` | PURE |
| `math_asin`, `math_acos`, `math_log2`, `math_log10`, `math_sinh`, `math_cosh`, `math_tanh` | `(f64) -> f64` | PURE |
| `math_atan2` (`y`, `x`), `math_hypot` | `(f64, f64) -> f64` | PURE |

They give **the same bits on every engine and every machine**, and in the
folding of Limba: one library computes them, `limba/math.h` (musl 1.2.5,
error below 1 ulp, 2 for `math_sinh`, `math_cosh` and `math_tanh`, not
always correctly rounded; `sqrt`, which `asin`, `acos` and `hypot` use, is
exact), compiled into every
engine; no engine calls the C library of the system for them. Special
cases as IEEE 754 § 9.2.1 and C99 Annex F (`atan2(±0, −0)` is ±π,
`hypot(±inf, nan)` is +inf, `log2(±0)` is −inf); no `errno`, no observable flag. A function on
`f32` is the function on `f64` followed by `fptrunc`. Between versions
the bits may change only for the better, and the change is declared.

### 15.6 Powers

| Function | Signature | Attributes | Meaning |
|---|---|---|---|
| `int_pow` | `(i64, i64) -> i64` | PURE, MAY_TRAP | base with a sign, exponent without; trap `OVERFLOW` past 64 bits with a sign |
| `uint_pow` | `(i64, i64) -> i64` | PURE, MAY_TRAP | base and exponent without a sign; trap `OVERFLOW` past `2^64 - 1` |
| `bits_pow` | `(i64, i64) -> i64` | PURE | modulo 2^64 |

`x ** 0` is 1. A negative exponent is refused before, in the IR.

### 15.7 `BigInt`

A `BigInt` is a `ref` (§ 12): counted, immutable, 0 for the handle 0.
The checks are in the IR, before the call: a divisor is never 0 (trap 11),
an exponent never negative (101), a real finite and integral (103, after
`fround.away`), a conversion to an integer checked with
`big_to_i64`/`big_to_u64` and the result (103). A call that skips them is
a wrong module.

| Function | Signature | Attributes | Meaning |
|---|---|---|---|
| `big_from_i64` | `(i64) -> ref` | ALLOC | the number with a sign |
| `big_from_u64` | `(i64) -> ref` | ALLOC | the number without a sign |
| `big_lit` | `(str) -> ref` | PURE, ALLOC | a constant past 64 bits: its decimal digits, a `-` first if negative |
| `big_from_f64` | `(f64) -> ref` | ALLOC | a finite integral real, exactly |
| `big_to_i64` | `(ref, ptr) -> i1` | WRITES_MEM | if it fits an `i64`, stores it and gives true; else false, nothing stored |
| `big_to_u64` | `(ref, ptr) -> i1` | WRITES_MEM | the same without a sign |
| `big_to_bits` | `(ref) -> i64` | PURE | the low 64 bits, in two's complement |
| `big_to_f64`, `big_to_f32` | `(ref) -> f64`, `(ref) -> f32` | PURE | rounded **once**, to nearest even, straight to the type (never through `f64` for `f32`); past the largest finite value an infinity with the sign |
| `big_add`, `big_sub`, `big_mul` | `(ref, ref) -> ref` | PURE, ALLOC | exact |
| `big_neg`, `big_abs` | `(ref) -> ref` | PURE, ALLOC | exact |
| `big_div` | `(ref, ref) -> ref` | PURE, ALLOC | the quotient, truncated toward zero |
| `big_rem` | `(ref, ref) -> ref` | PURE, ALLOC | the remainder, with the sign of the dividend |
| `big_mod` | `(ref, ref) -> ref` | PURE, ALLOC | the modulus, with the sign of the divisor |
| `big_pow` | `(ref, i64) -> ref` | PURE, ALLOC | the exponent read without a sign; exact for the bases 0, 1, −1 with any exponent; `0 ** 0` is 1 |
| `big_cmp` | `(ref, ref) -> i32` | PURE | exactly −1, 0 or 1 |
| `big_sign` | `(ref) -> i32` | PURE | −1, 0 or 1 |
| `print_big` | `(ref) -> void` | IO | the decimal text, a `-` first if negative |
| `str_from_big` | `(ref) -> str` | ALLOC | the same text |
| `str_to_big` | `(str, ptr) -> i1` | WRITES_MEM | `val`: an integer of the language of any size (`limba_val_big` of `limba/val.h`), stored as a `ref` at the pointer; false and nothing stored for another text |
| `big_gcd` | `(ref, ref) -> ref` | PURE, ALLOC | ≥ 0; 0 for 0 and 0 |
| `big_lcm` | `(ref, ref) -> ref` | PURE, ALLOC | ≥ 0; 0 if an operand is 0 |
| `big_sqrt` | `(ref) -> ref` | PURE, ALLOC | the floor of the square root of `a ≥ 0` |
| `big_root` | `(ref, i64) -> ref` | PURE, ALLOC | the k-th root, `k ≥ 1`, `a ≥ 0` for an even k, truncated toward zero |
| `big_divisible` | `(ref, ref) -> i1` | PURE | does `b ≠ 0` divide `a`? |
| `big_divexact` | `(ref, ref) -> ref` | PURE, ALLOC | `a / b` for a `b ≠ 0` that divides `a` (checked before with `big_divisible`; which trap that gives is the language's) |
| `big_powmod` | `(ref, ref, ref) -> ref` | PURE, ALLOC | `b ** e` modulo the absolute value of `m`, a result from 0 to that value minus 1, for `e ≥ 0` and `m ≠ 0`; `0 ** 0` is 1 |

### 15.8 C strings

Memory of C, not of the program: outside the engine's memory limit.

| Function | Signature | Attributes | Meaning |
|---|---|---|---|
| `cstr_new` | `(str) -> ptr` | ALLOC, MAY_TRAP | a copy with a 0 after it, `malloc` of C; a 0 inside the string is the trap `RANGE` (101), a failed `malloc` `NOMEM` |
| `cstr_value` | `(ptr) -> str` | ALLOC | the bytes up to the first 0; the pointer not nil (checked before) |
| `cstr_value_n` | `(ptr, i64) -> str` | ALLOC | n bytes; the pointer not nil, `n ≥ 0` (checked before) |
| `cstr_free` | `(ptr) -> void` | WRITES_MEM | C's `free` of what `cstr_new` or C made; `null` does nothing |

`lir_run` knows only the C strings `cstr_new` made: `cstr_free` of any
other is a wrong module there.

### 15.9 Routines on arrays of bytes

The arguments are checked before, in the IR (`n ≥ 0`, the range inside
its array, the pattern not empty, nil and dangling); no trap.

| Function | Signature | Attributes | Meaning |
|---|---|---|---|
| `mem_translate` | `(ptr, i64, ptr) -> void` | WRITES_MEM | `a[i] := table[a[i]]` for the n bytes at `a`; the 256 bytes of `table` are read first (`table` may be `a`) |
| `mem_reverse` | `(ptr, i64, i64) -> void` | WRITES_MEM | the n elements of `size ≥ 1` bytes at `a`, in reverse order, in place; no count (a permutation: the handles move with their element) |
| `mem_count` | `(ptr, i64, ptr, i64) -> i64` | none | the occurrences of the m ≥ 1 bytes at `p` in the n bytes at `a`, searched from the left, not overlapping |

### 15.10 Parallel for

| Function | Signature | Attributes |
|---|---|---|
| `par_for` | `(ptr, ptr, i64, i64) -> void` | CALLS_PROGRAM, MAY_TRAP, WRITES_MEM, IO, ALLOC |

`call.rt void par_for(body, env, lo, hi)` calls `body(env, i)` for every
`i` of `lo .. hi`, `hi` included (none if `lo > hi`); `body` is `faddr` of
a function of type `fn(ptr, i64) -> void` (anything else is a wrong
module). **Every iteration runs**; then, if some left with an exception,
the one of the lowest index leaves `par_for` and the others are dropped.
An exception that no handler may catch (§ 13.3) stops at once. Engines run
the iterations in sequence (§ 5.1).

### 15.11 Files

| Function | Signature | Attributes | Meaning |
|---|---|---|---|
| `file_open` | `(str, i32) -> i64` | IO, MAY_TRAP | opens the file named by the bytes of the string: mode 0 reads, 1 writes (made, or emptied), 2 appends (made if missing); gives its handle |
| `file_close` | `(i64) -> void` | IO, MAY_TRAP | writes the buffer and closes |
| `file_flush` | `(i64) -> void` | IO, MAY_TRAP | writes the buffer |
| `file_read_line` | `(i64, ptr) -> i1` | IO, ALLOC, WRITES_MEM, MAY_TRAP | as `read_line`, on the file |
| `file_read` | `(i64, ptr, i64) -> i64` | IO, WRITES_MEM, MAY_TRAP | as `io_read`, on the file |
| `file_write` | `(i64, ptr, i64) -> void` | IO, MAY_TRAP | as `io_write`, into the buffer of the file |
| `file_exists` | `(str) -> i1` | IO, MAY_TRAP | is there anything of that name? |
| `file_delete` | `(str) -> void` | IO, MAY_TRAP | removes it |

- **Handles.** A handle is an `i64`: the n-th file opened successfully in
  the run is n, and a handle is never given again. At most **256** files
  are open at once. A handle never opened, or closed, is the trap
  `INVALID_HANDLE` (109), in the family 902.
- **Names.** The bytes of the string go to the system as they are; a 0
  byte in a name is the trap `RANGE` (101). A mode other than 0, 1, 2 is
  checked before, in the IR: a wrong module.
- **Errors.** Failing to open is the trap `IO` (107) with a message of
  its own, never the system's words: `not found: <name>` (no such file
  or directory), `not allowed: <name>` (permission, a read-only file
  system), `cannot open: <name>` (anything else, a directory too), `too
  many open files` (the 257th). `file_delete` fails the same way, with
  `cannot delete: <name>` for anything else. Reading a file opened for
  writing, writing one opened for reading, and every error of reading or
  writing are the trap `IO` with the text of `traps.def`. All of them
  can be caught.
- **Buffers.** Every file has a buffer of **65 536 bytes**, written with
  `write(2)` (no other buffer between) when it is full, at `file_flush`
  and at `file_close`, never before: an error of writing is found by the
  same call in every engine. Reading goes through the same buffer; the
  end of a file is final for that handle, as for the standard input.
  The buffers are memory of the run time: outside the memory budget of
  the program and of the count of `--check-mem`.
- **The end of the run.** The files still open are written and closed
  after the run, also after an exception no one caught or `halt`, after
  its message. An error there, after a run that ended by itself
  (returned or `halt`), is the trap `IO` without a position (§ 5.3);
  after an exception no one caught it adds no second message.
- The arguments the IR checks before (the range of `file_read` and
  `file_write`, the mode) are looked at first, then the handle.
- `lir_run` runs the files for real.

## 16. Traps and families

`traps.def`. A program never sees a code as a number unless it catches
it; uncaught, it ends the program (§ 13.7).

| Code | Name | Text | Family |
|---:|---|---|---:|
| 6 | `OVERFLOW` | overflow | 900 |
| 7 | `NOMEM` | out of memory | 901 |
| 11 | `DIVZERO` | division by zero | 900 |
| 28 | `STACK` | stack overflow | 901 |
| 100 | `INDEX` | index out of range | 900 |
| 101 | `RANGE` | value out of range | 900 |
| 102 | `NIL` | nil dereferenced | 900 |
| 103 | `CONVERSION` | conversion out of range | 900 |
| 104 | `SHIFT` | shift count out of range | 900 |
| 105 | `DANGLING` | dangling pointer | 900 |
| 106 | `INVALID_FREE` | invalid dispose | 902 |
| 107 | `IO` | input/output error | 903 |
| 108 | `LENGTH` | length mismatch | 900 |
| 109 | `INVALID_HANDLE` | invalid file handle | 902 |

| Family | Name | Members |
|---:|---|---|
| 900 | `CONSTRAINT` | 6, 11, 100-105, 108 |
| 901 | `STORAGE` | 7, 28: never caught |
| 902 | `PROGRAM` | 106, 109 |
| 903 | `INPUT_OUTPUT` | 107 |

No instruction traps with a family's identity; a program may raise one.

## 17. The optimiser

The passes of Limba (`-O1`: inline, cfg, gvn with folding, licm, dce;
bounds off by default) keep the meaning of every program that is not a
wrong module, with these guarantees, which engines may rely on:

- the output, the traps and their positions, the exit status are those
  of the unoptimised module; only where `STACK` and `NOMEM` happen may
  change (§ 5.4);
- folding gives exactly what an engine would compute: it never folds
  what would trap (a division by zero, an overflowing `.ov`), computes
  floats in their own precision, never folds a float operation whose
  result is a NaN (except `fneg`, on the bits), never converts a NaN
  between `f32` and `f64`, and folds `math_*` only to a result that is
  not a NaN; shifts fold modulo the width;
- no pass introduces `fma` or `fround.away`;
- `check`s may be merged with an equal one that dominates them, and moved
  out of a loop only from the prefix of its header (before any
  instruction with an effect), where they would run first anyway; a
  `load.inv` likewise; `ptr_live(p)` leaves a loop only if nothing in the
  loop may free;
- nothing that may fail, and no call, is moved into or out of a
  protected block; blocks with different handlers are not merged;
  inlined blocks take the handler of the block of the call;
- `call.ind` and `call.ext` are never inlined;
- after the optimiser a check the front end wrote may be gone (folded,
  merged, proven): the canonical forms of § 9.4 hold for the front end's
  IR, and an engine never needs them to be there.

## 18. Open points of version 9

Not decided; listed so that no engine decides them alone. Each will be
decided and written here first.

1. **Signed operations on `i1`.** `sext` reads an `i1` 1 as −1, while
   `icmp.s*`, `sdiv`, `srem`, `sitofp` and the signed `.ov` operations of
   `lir_run` read it as +1 (so `add.ov i1 0, 1` traps, and `fptosi` to
   `i1` saturates to `[−1, 0]`). Front ends should not use them; `lir_gen`
   does, and `lir_run` and the folding agree with each other there.
2. **Order of pointers.** `icmp.ult` and the other order conditions on
   `ptr` compare the bits; their result between different objects is not
   defined (tags may differ).
3. **Unused immediates.** The verifier does not yet check that `imm` and
   `imm2` are 0 where the format does not use them; flags of `load` and
   `store` (N3) will use `imm`.
4. **Flags `export` and `const`.** Carried, without a meaning; `lir_run`
   lets a program write a `const` global.
5. **Alignment** of loads and stores other than the handles: not
   required, not checked.
6. **The memory model** (§ 9.5) is a rule for producers; the checks of
   the oracle are weaker (rule 2).
7. **Codes of `trap` and `check`.** § 11 says the code is an identity of
   `traps.def` or of the module's table; the verifier accepts any code
   ≥ 0, and a code above 2^31 − 1 is cut to 32 bits when it reaches a
   handler.
8. **Cases of `switch`.** The verifier refuses two equal halves, not two
   values that are equal in the type of `v` (`255` and `-1` on `i8`);
   `lir_run` takes the first.
9. **`print_char` and `str_from_char`** of a value that is not a Unicode
   scalar value (a surrogate, past `0x10FFFF`, negative): not defined.
10. **`print_big`** writes output but has no `MAY_TRAP`, unlike the other
    functions of output (§ 15.1).
11. **`big_lit`** of a text that is not decimal digits: not defined
    (`lir_run` skips the other characters).

## Appendix A. Operations

The number is the position in `ir_ops.def`, the operation's number in the
binary form.

| # | Operation | Format | Flags |
|---:|---|---|---|
| 0 | `iconst` | ICONST | PURE |
| 1 | `fconst` | FCONST | PURE |
| 2 | `sconst` | SCONST | PURE |
| 3 | `null` | TYPED | PURE |
| 4 | `add` | BIN | PURE, COMMUTATIVE |
| 5 | `sub` | BIN | PURE |
| 6 | `mul` | BIN | PURE, COMMUTATIVE |
| 7 | `sdiv` | BIN | MAY_TRAP |
| 8 | `udiv` | BIN | MAY_TRAP |
| 9 | `srem` | BIN | MAY_TRAP |
| 10 | `urem` | BIN | MAY_TRAP |
| 11 | `neg` | UN | PURE |
| 12 | `add.ov` | BIN | MAY_TRAP, COMMUTATIVE |
| 13 | `sub.ov` | BIN | MAY_TRAP |
| 14 | `mul.ov` | BIN | MAY_TRAP, COMMUTATIVE |
| 15 | `and` | BIN | PURE, COMMUTATIVE |
| 16 | `or` | BIN | PURE, COMMUTATIVE |
| 17 | `xor` | BIN | PURE, COMMUTATIVE |
| 18 | `not` | UN | PURE |
| 19 | `shl` | BIN | PURE |
| 20 | `lshr` | BIN | PURE |
| 21 | `ashr` | BIN | PURE |
| 22 | `fadd` | BIN | PURE |
| 23 | `fsub` | BIN | PURE |
| 24 | `fmul` | BIN | PURE |
| 25 | `fdiv` | BIN | PURE |
| 26 | `fneg` | UN | PURE |
| 27 | `fma` | TERN | PURE |
| 28 | `icmp` | CMP | PURE |
| 29 | `fcmp` | CMP | PURE |
| 30 | `select` | TERN | PURE |
| 31 | `trunc` | CONV | PURE |
| 32 | `zext` | CONV | PURE |
| 33 | `sext` | CONV | PURE |
| 34 | `fptrunc` | CONV | PURE |
| 35 | `fpext` | CONV | PURE |
| 36 | `sitofp` | CONV | PURE |
| 37 | `uitofp` | CONV | PURE |
| 38 | `fptosi` | CONV | PURE |
| 39 | `fptoui` | CONV | PURE |
| 40 | `bitcast` | CONV | PURE |
| 41 | `ptrtoint` | CONV | PURE |
| 42 | `inttoptr` | CONV | PURE |
| 43 | `load` | LOAD | READS_MEM, MAY_TRAP |
| 44 | `store` | STORE | WRITES_MEM, MAY_TRAP, NO_RESULT |
| 45 | `slot` | SLOT | PURE |
| 46 | `gaddr` | GADDR | PURE |
| 47 | `faddr` | FADDR | PURE |
| 48 | `addr` | ADDR | PURE |
| 49 | `memcpy` | MEM3 | READS_MEM, WRITES_MEM, MAY_TRAP, NO_RESULT |
| 50 | `memset` | MEM3 | WRITES_MEM, MAY_TRAP, NO_RESULT |
| 51 | `call` | CALL | CALL |
| 52 | `call.ind` | CALL_IND | CALL |
| 53 | `call.ext` | CALL_EXT | CALL |
| 54 | `call.rt` | CALL_RT | CALL |
| 55 | `br` | BR | TERMINATOR, NO_RESULT |
| 56 | `cbr` | CBR | TERMINATOR, NO_RESULT |
| 57 | `switch` | SWITCH | TERMINATOR, NO_RESULT |
| 58 | `ret` | RET | TERMINATOR, NO_RESULT |
| 59 | `unreachable` | NONE | TERMINATOR, NO_RESULT |
| 60 | `trap` | TRAP | TERMINATOR, NO_RESULT, MAY_TRAP |
| 61 | `check` | CHECK | NO_RESULT, MAY_TRAP |
| 62 | `param` | PARAM | PURE |
| 63 | `fround` | UN | PURE |
| 64 | `fround.away` | UN | PURE |
| 65 | `retain` | RC | READS_MEM, WRITES_MEM, MAY_TRAP, NO_RESULT |
| 66 | `release` | RC | READS_MEM, WRITES_MEM, MAY_TRAP, NO_RESULT |
| 67 | `load.inv` | LOAD | READS_MEM, MAY_TRAP |
| 68 | `raise` | RAISE | TERMINATOR, NO_RESULT, MAY_TRAP |
| 69 | `uadd.ov` | BIN | MAY_TRAP, COMMUTATIVE |
| 70 | `usub.ov` | BIN | MAY_TRAP |
| 71 | `umul.ov` | BIN | MAY_TRAP, COMMUTATIVE |

`param` is never written in the binary form nor as an instruction of the
text form: a block parameter is written in the block's header.

Operand and result types, as the verifier checks them:

| Operations | Operands | Result |
|---|---|---|
| integer `BIN` (`add` … `umul.ov`, not floats) | two of T | T, an integer type |
| `neg`, `not` | one of T | T, an integer type |
| `fadd` … `fdiv` | two of T | T, a float type |
| `fneg`, `fround`, `fround.away` | one of T | T, a float type |
| `fma` | three of T | T, a float type |
| `select` | `i1`, two of T | T, any scalar |
| `icmp` | two of an integer type or `ptr` | `i1` |
| `fcmp` | two of a float type | `i1` |
| `trunc` | an integer | a narrower integer |
| `zext`, `sext` | an integer | a wider integer |
| `fptrunc` / `fpext` | `f64` / `f32` | `f32` / `f64` |
| `sitofp`, `uitofp` | an integer | a float |
| `fptosi`, `fptoui` | a float | an integer |
| `bitcast` | `i32`, `f32`, `i64`, `f64` | the other of the same width |
| `ptrtoint` | `ptr` | an integer |
| `inttoptr` | an integer | `ptr` |
| `load`, `load.inv` | `ptr` | any scalar |
| `store` | any scalar, `ptr` | none |
| `slot`, `gaddr`, `faddr` | none | `ptr` |
| `addr` | `ptr`, an integer | `ptr` |
| `memcpy` | `ptr`, `ptr`, an integer | none |
| `memset` | `ptr`, an integer, an integer | none |
| `retain`, `release` | `ptr`, an integer | none |
| `cbr`, `check` | `i1` | none |
| `switch` | an integer | none |
| `raise` | `i32`, `i32`, `str` | none |

## Appendix B. Conditions

| # | Text | Of |
|---:|---|---|
| 0-9 | `eq`, `ne`, `slt`, `sle`, `sgt`, `sge`, `ult`, `ule`, `ugt`, `uge` | `icmp` |
| 10-17 | `oeq`, `one`, `olt`, `ole`, `ogt`, `oge`, `ord`, `uno` | `fcmp` |
| 18-23 | `ueq`, `une`, `ult`, `ule`, `ugt`, `uge` | `fcmp` (unordered) |

`fcmp.ult` (20) and `icmp.ult` (6), and the other three pairs, share
their text: the operation tells them apart.

## Appendix C. The run-time table

The number is the position in `runtime.def`.

| # | Function | # | Function | # | Function |
|---:|---|---:|---|---:|---|
| 0 | `print_i64` | 29 | `arg_count` | 58 | `big_mul` |
| 1 | `print_f64` | 30 | `arg` | 59 | `big_neg` |
| 2 | `print_str` | 31 | `halt` | 60 | `big_abs` |
| 3 | `print_nl` | 32 | `math_tan` | 61 | `big_div` |
| 4 | `str_concat` | 33 | `math_atan` | 62 | `big_rem` |
| 5 | `str_len` | 34 | `math_exp` | 63 | `big_mod` |
| 6 | `str_cmp` | 35 | `math_ln` | 64 | `big_pow` |
| 7 | `str_from_i64` | 36 | `math_trunc` | 65 | `big_cmp` |
| 8 | `str_from_f64` | 37 | `math_floor` | 66 | `big_sign` |
| 9 | `str_mid` | 38 | `math_ceil` | 67 | `print_big` |
| 10 | `str_ptr` | 39 | `int_pow` | 68 | `str_from_big` |
| 11 | `mem_alloc` | 40 | `uint_pow` | 69 | `str_to_big` |
| 12 | `mem_free` | 41 | `bits_pow` | 70 | `cstr_new` |
| 13 | `math_sqrt` | 42 | `str_to_u64` | 71 | `cstr_value` |
| 14 | `math_sin` | 43 | `str_to_f32` | 72 | `cstr_value_n` |
| 15 | `math_cos` | 44 | `print_f32` | 73 | `cstr_free` |
| 16 | `math_pow` | 45 | `str_from_f32` | 74 | `mem_translate` |
| 17 | `print_u64` | 46 | `ptr_live` | 75 | `mem_reverse` |
| 18 | `print_bool` | 47 | `big_from_i64` | 76 | `mem_count` |
| 19 | `print_char` | 48 | `big_from_u64` | 77 | `io_read` |
| 20 | `print_byte` | 49 | `big_lit` | 78 | `io_write` |
| 21 | `print_str_w` | 50 | `big_to_i64` | 79 | `big_gcd` |
| 22 | `str_from_u64` | 51 | `big_to_u64` | 80 | `big_lcm` |
| 23 | `str_from_f64_fixed` | 52 | `big_to_bits` | 81 | `big_sqrt` |
| 24 | `str_from_char` | 53 | `big_from_f64` | 82 | `big_root` |
| 25 | `str_from_bool` | 54 | `big_to_f64` | 83 | `big_divisible` |
| 26 | `read_line` | 55 | `big_to_f32` | 84 | `big_divexact` |
| 27 | `str_to_i64` | 56 | `big_add` | 85 | `big_powmod` |
| 28 | `str_to_f64` | 57 | `big_sub` | 86 | `par_for` |

Added on 4 Nov 2026:

| # | Function | # | Function | # | Function |
|---:|---|---:|---|---:|---|
| 87 | `pos_str` | 93 | `file_write` | 99 | `math_hypot` |
| 88 | `file_open` | 94 | `file_exists` | 100 | `math_log2` |
| 89 | `file_close` | 95 | `file_delete` | 101 | `math_log10` |
| 90 | `file_flush` | 96 | `math_atan2` | 102 | `math_sinh` |
| 91 | `file_read_line` | 97 | `math_asin` | 103 | `math_cosh` |
| 92 | `file_read` | 98 | `math_acos` | 104 | `math_tanh` |
