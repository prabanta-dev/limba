# Limba

[![Status: experimental](https://img.shields.io/badge/status-experimental-orange.svg)]()
[![Platform: Linux x86-64](https://img.shields.io/badge/platform-Linux%20x86--64-lightgrey.svg)]()
[![Licence: GPL-3.0-or-later](https://img.shields.io/badge/licence-GPL--3.0--or--later-blue.svg)](LICENSE)
[![LinkedIn](https://img.shields.io/badge/LinkedIn-Maurizio%20Cammalleri-0077B5?logo=linkedin)](https://www.linkedin.com/in/maurizio-cammalleri-80a89a11/)
[![Substack](https://img.shields.io/badge/Substack-Maurizio%20Cammalleri-FF6719?logo=substack)](https://cammalleri.substack.com/)

Limba is the part of the Prabanta family of compilers that every language
shares, written in C with no dependencies beyond the C library: an SSA
intermediate representation (IR), its text and binary forms, a verifier,
the optimisation passes and a reference interpreter. Each language has its
own repository with its front end, which makes this IR:
[Luxia](https://github.com/prabanta-dev/luxia), an extended Pascal with the
rigour of Ada, is the first. **Meri**, the back end (bytecode compiler,
virtual machine, AOT and JIT), executes it.

Limba takes its ideas from SedaiBasic2, an earlier compiler of the author
written in Free Pascal, not its code.

---

## ⚠️ Read this first

**This is a project in development, not a usable compiler.**

| | |
|---|---|
| **Stage** | Early development. The IR, its file formats and the command line **change without notice**. |
| **Running programs** | Limba stops at the IR. It comes with a reference interpreter, slow on purpose, used by the tests; the fast engine is **Meri**, developed alongside in its own repository and not released yet. |
| **Platform** | Developed and tested on Linux x86-64 only (Debian 13, GCC and Clang). |

Nothing here is ready for use in real work. It is public so that the design
and its progress can be followed.

## What works now

- **The IR**, its text and binary forms, a verifier, and the optimiser:
  inlining of short functions, CFG simplification, constant folding,
  global value numbering, loop-invariant code motion, dead code
  elimination.
- **A generator of random IR** (`lir_gen`) and the reference interpreter
  (`lir_run`), which the front ends use as their oracle.

```
limba program.lit            # writes program.lir, verified
limba -O1 program.lir -o opt.lir   # optimised
limba program.lir            # prints it as text
lir_run program.lir          # runs it in the reference interpreter
```

## How Limba is built

```
 a front end (Luxia: luxia)
      │  one function at a time, in memory (or a .lit / .lir file)
      ▼
   verifier ──► optimiser ──► writer ──► program.lir
                     │                        │
                     └──► Meri ◄──────────────┘     lir_run (reference)
```

**The IR** (`include/limba/ir.h`, `src/ir/`) is the contract between the
front ends and Meri, and does not know any language:

- SSA with **block parameters** instead of phi nodes; a function is a list
  of blocks, a block a list of instructions ending with one terminator;
- everything refers to everything else **by index**, never by pointer, so
  the binary form (`.lir`) is the arrays written out, and the text form
  (`.lit`) is readable and writable by hand;
- the operations, the run-time library and the run-time errors are tables
  (`ir_ops.def`, `runtime.def`, `traps.def`) shared with Meri, each entry
  saying what an optimisation may assume of it (pure, may trap, reads or
  writes memory);
- the checks of a language such as Luxia (overflow, index, range, `nil`, dangling pointer,
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

**One function at a time.** A front end gives each function as soon as it
is complete; Limba verifies, optimises, writes and frees it; only the
copies of the short functions stay, for inlining. The memory used stays
small on large files. A front end also gets from Limba the SSA
construction (`src/ir/ssa.c`, Braun et al., *Simple and Efficient
Construction of Static Single Assignment Form*) and exact integers and
rationals (`src/common/bigint.c`).

**How it is tested** (`tests/`, `tools/`):

- a **reference interpreter** of the IR (`src/eval/`), simple and slow on
  purpose, which also counts the strings kept in memory and stops with an
  error, never a crash, when memory or stack run out;
- **OPTDIFF**: every program runs without optimisation, with all the
  passes and with each pass alone, and every run must print the same and
  end the same way, a run-time error at the same place of the source;
- a **generator of random IR**, run through the same net (the front ends
  have their own generators, with the same oracle);
- **sabotage**: every net is tried with a bug put in on purpose, and must
  catch it;
- the release, debug, AddressSanitizer and UndefinedBehaviorSanitizer
  builds all run the suites; a fuzzer reads broken IR.

**Where things are:**

| | |
|---|---|
| `include/limba/` | the public API: the IR, its operations, the run-time table, the optimiser, the printing and reading of numbers shared with Meri |
| `src/common/` | hash tables, interned strings, LEB128, allocation, exact integers and rationals |
| `src/ir/` | the IR: building, SSA construction, verifier, CFG and dominators, text and binary forms |
| `src/opt/` | the pass manager and the passes |
| `src/eval/` | the reference interpreter and the generator of random IR |
| `src/limba/` | the `limba` command |
| `tools/` | `lir_run`, `lir_gen` |
| `tests/` | the suites and their data |

## Roadmap

Done:

- [x] The IR, its forms, the verifier and the reference interpreter
- [x] Random programs for the IR, OPTDIFF
- [x] Optimisation: folding, GVN, LICM, inlining
- [x] Integers of arbitrary precision in the IR and its run time
- [x] The first front end, Luxia, in its own repository

Next:

- [ ] More optimisation: promotion of memory to registers, removal of
      checks that range analysis proves useless
- [ ] **Meri**, the back end: a register-based virtual machine, then AOT
      and JIT compilation
- [ ] What **Luxia 1** needs of the IR: exceptions, objects, generics

## Building

    ./build.sh               # release build: bin/<cpu>-<os>/limba
    ./build.sh release test  # and the tests
    ./build.sh --help        # variants (debug, asan, ubsan, tsan), actions

A C11 compiler (GCC or Clang) and a POSIX shell are all it needs.

## Licence

GPL-3.0-or-later, see [`LICENSE`](LICENSE).
