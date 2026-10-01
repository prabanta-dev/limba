# Limba

[![Status: experimental](https://img.shields.io/badge/status-experimental-orange.svg)]()
[![Platform: Linux x86-64](https://img.shields.io/badge/platform-Linux%20x86--64-lightgrey.svg)]()
[![Licence: GPL-3.0-or-later](https://img.shields.io/badge/licence-GPL--3.0--or--later-blue.svg)](LICENSE)
[![LinkedIn](https://img.shields.io/badge/LinkedIn-Maurizio%20Cammalleri-0077B5?logo=linkedin)](https://www.linkedin.com/in/maurizio-cammalleri-80a89a11/)
[![Substack](https://img.shields.io/badge/Substack-Maurizio%20Cammalleri-FF6719?logo=substack)](https://cammalleri.substack.com/)

Limba is the front end of the Prabanta family of compilers, written in C
with no dependencies beyond the C library: lexer, parser, semantic analysis,
an SSA intermediate representation and its optimisation passes. It reads
**Luxia**, an extended Pascal with the rigour of Ada and without its
verbosity, and writes an intermediate representation (IR) that **Meri**, the
back end (bytecode compiler, virtual machine, AOT and JIT), will execute.

Limba takes its ideas from SedaiBasic2, an earlier compiler of the author
written in Free Pascal, not its code.

---

## ⚠️ Read this first

**This is a project in development, not a usable compiler.**

| | |
|---|---|
| **Stage** | Early development. The language, the IR, its file formats and the command line **change without notice**. |
| **Running programs** | Limba stops at the IR. It comes with a reference interpreter, slow on purpose, used by the tests; the fast engine is **Meri**, developed alongside in its own repository and not released yet. |
| **The language** | Luxia 0 is specified in [`doc/luxia/luxia-0.md`](doc/luxia/luxia-0.md): a draft, whose rules may still change. |
| **Platform** | Developed and tested on Linux x86-64 only (Debian 13, GCC and Clang). |

Nothing here is ready for use in real work. It is public so that the design
and its progress can be followed.

## What Luxia looks like

```pascal
program Hello;

type
  Tree = ^Node;
  Node = record
    left, right: Tree;
  end;

function Check(t: Tree): Int64;
begin
  if t.left = nil then
    return 1;
  end;
  return 1 + Check(t.left) + Check(t.right);
end Check;

begin
  var n: Int32 := 10;
  for var i := 1 to n do
    writeln(i, TAB, i * i);
  end;
end Hello.
```

Some of its rules:

- Only sized types: `Int8`..`Int64`, `UInt8`..`UInt64`, `Bits8`..`Bits64`,
  `Float32`, `Float64`. There is no `Integer` whose size depends on the
  platform, no implicit conversion and no promotion: both operands of an
  operator have the same type.
- Arithmetic on numbers is checked (overflow, index, range, `nil`,
  division by zero, conversion). Wrapping arithmetic and bit operators
  exist only on the `BitsN` types.
- Names are unique without regard to case, but every use must be spelled
  as the declaration.
- Every structured statement ends with `end`; `;` ends every statement, as
  in Ada.
- Constant expressions are computed exactly (integers and rationals of any
  size) and rounded once, when they take a type.

## What works now

- **Luxia 0** as specified: sized integers, reals, Booleans, characters,
  counted Strings, ranges, enumerations, records, arrays (fixed, with
  computed bounds, open, made by `new`), pointers with `nil` and dangling
  checks, routines with `in`, `var` and `out` parameters, functions
  returning records and arrays. Every error has a stable code
  ([`doc/limba/diagnostics.md`](doc/limba/diagnostics.md)).
- **The IR**, its text and binary forms, a verifier, and the optimiser:
  inlining of short functions, CFG simplification, constant folding,
  global value numbering, loop-invariant code motion, dead code
  elimination.
- **Nine benchmarks from the Computer Language Benchmarks Game**, translated
  to Luxia, print the expected output: binary-trees, fannkuch-redux, fasta,
  k-nucleotide, mandelbrot, n-body, pidigits, reverse-complement and
  spectral-norm ([`tests/luxia/benchmarks/`](tests/luxia/benchmarks/)).
- **Speed of the front end**: about 1 µs per line of source without
  optimisation, 1.7 µs with it (a file of 108 000 lines, one core).

```
limba program.luxia          # writes program.lir
limba -O1 program.luxia      # the same, optimised
limba --emit=lit program.luxia -o program.lit   # the IR as text
lir_run program.lir          # runs it in the reference interpreter
```

## How Limba is built

```
 program.luxia
      │
      ▼
   lexer ──► parser ──► semantic analysis ──► IR generation (SSA)
                                                     │  one function
                                                     ▼  at a time
                                  verifier ──► optimiser ──► writer ──► program.lir
                                                                            │
                                                         Meri, lir_run  ◄───┘
```

**The front end** (`src/luxia/`, with the parts any language can use in
`src/front/`):

- the **lexer** reads the tokens from a table (`tokens.def`); positions are
  32-bit offsets into the loaded sources, turned into lines and columns
  only for a message;
- the **parser** is recursive descent; expressions go through a Pratt
  engine driven by a table of operators. The grammar is LL(1). The tree is a flat array of nodes that refer to
  each other by index;
- the **semantic analysis** resolves names in scopes, declarations on
  demand (types and routines may be used before their declaration,
  without `forward`), and types.
  Constant expressions are computed exactly, with integers and rationals
  of any size, and rounded once, when they take a type;
- the **IR generation** builds SSA directly from the tree (Braun et al.,
  *Simple and Efficient Construction of Static Single Assignment Form*),
  and on the way proves that every variable is assigned before it is
  read and that every function returns a value.

The front end is also a library (`include/limba/limba_luxia.h`): a
program that runs the IR in memory compiles a source and receives each
function as soon as it is complete, verified and optimised, as `limba`
does to write its `.lir`.

**The IR** (`include/limba/ir.h`, `src/ir/`) is the contract with Meri and
does not know Luxia:

- SSA with **block parameters** instead of phi nodes; a function is a list
  of blocks, a block a list of instructions ending with one terminator;
- everything refers to everything else **by index**, never by pointer, so
  the binary form (`.lir`) is the arrays written out, and the text form
  (`.lit`) is readable and writable by hand;
- the operations, the run-time library and the run-time errors are tables
  (`ir_ops.def`, `runtime.def`, `traps.def`) shared with Meri, each entry
  saying what an optimisation may assume of it (pure, may trap, reads or
  writes memory);
- the checks of Luxia (overflow, index, range, `nil`, dangling pointer,
  division, conversion) are **instructions of the IR** (`check`, the `.ov`
  arithmetic), so the optimiser sees them, merges them and moves them out
  of loops;
- IEEE 754 arithmetic and saturating conversions, the same on every CPU;
  strings are counted handles of the run time;
- a **verifier** checks types, dominance, the CFG and the tables before
  anything reads a module.

**The optimiser** (`src/opt/`) is one pipeline in one table: every
program that optimises runs the same passes in the same order. A pass
records its changes in one edit, applied once at the end, and wakes only
the passes it may give work to. The passes: `inline` (short functions
optimised before their caller), `cfg` (constant branches, unreachable and
redundant blocks), `gvn` (folding, then equal values computed once),
`licm` (what does not change in a loop, and its checks, before the
loop), `dce` (values nobody uses); `bounds` (checks proved by the facts
before them) is written and off.

**One function at a time.** `limba` verifies, optimises, writes and frees
each function as soon as it is complete; only the copies of the short
functions stay, for inlining. The memory used stays small on large
files.

**How it is tested** (`tests/`, `tools/`):

- a **reference interpreter** of the IR (`src/eval/`), simple and slow on
  purpose, which also counts the strings kept in memory and stops with an
  error, never a crash, when memory or stack run out;
- **OPTDIFF**: every program runs without optimisation, with all the
  passes and with each pass alone, and every run must print the same and
  end the same way, a run-time error at the same place of the source;
- two **generators of random programs**, one of IR and one of well-typed
  Luxia that knows what it must print, run through the same net;
- **sabotage**: every net is tried with a bug put in on purpose, and must
  catch it;
- the release, debug, AddressSanitizer and UndefinedBehaviorSanitizer
  builds all run the suites; two fuzzers read broken IR and broken Luxia.

**Where things are:**

| | |
|---|---|
| `include/limba/` | the public API: the IR, its operations, the run-time table, the optimiser, the front end of Luxia as a library (`limba_luxia.h`), the printing and reading of numbers shared with Meri |
| `src/common/` | hash tables, interned strings, LEB128, allocation |
| `src/ir/` | the IR: building, verifier, CFG and dominators, text and binary forms |
| `src/opt/` | the pass manager and the passes |
| `src/eval/` | the reference interpreter and the generator of random IR |
| `src/front/` | what any front end can use: sources and positions, diagnostics, exact numbers, types, scopes, SSA construction, the Pratt engine |
| `src/luxia/` | Luxia: lexer, parser, semantic analysis, IR generation, random programs |
| `src/limba/` | the `limba` command |
| `tools/` | `lir_run`, `lir_gen`, `lx_gen` |
| `tests/` | the suites and their data |
| `doc/` | the specification of Luxia and the list of diagnostics |

## Roadmap

Done:

- [x] The IR, its forms, the verifier and the reference interpreter
- [x] Luxia 0: lexer, parser, semantic analysis, generation of the IR,
      published specification
- [x] The nine single-thread benchmarks run with the expected output
- [x] Random programs for the IR and for Luxia, OPTDIFF on both
- [x] Optimisation: folding, GVN, LICM, inlining

Next:

- [ ] An integer type of arbitrary precision in Luxia
- [ ] More optimisation: promotion of memory to registers, removal of
      checks that range analysis proves useless
- [ ] A public API of the front end, for a single executable that
      compiles and runs in memory
- [ ] **Meri**, the back end: a register-based virtual machine, then AOT
      and JIT compilation
- [ ] **Luxia 1**: modules, exceptions, types for hardware (bit layouts,
      fixed addresses, volatile access)

## Building

    ./build.sh               # release build: bin/<cpu>-<os>/limba
    ./build.sh release test  # and the tests
    ./build.sh --help        # variants (debug, asan, ubsan, tsan), actions

A C11 compiler (GCC or Clang) and a POSIX shell are all it needs.

## Licence

GPL-3.0-or-later, see [`LICENSE`](LICENSE).
