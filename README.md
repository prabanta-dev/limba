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
| **Running programs** | There is no fast way to run a Luxia program yet. Limba stops at the IR; the only thing that executes it is a reference interpreter, slow on purpose, used by the tests. The real back end, Meri, is not written yet. |
| **The language** | Luxia 0 is a draft specification, in Italian and not yet published. Its rules may still change. |
| **Platform** | Developed and tested on Linux x86-64 only (Debian 13, GCC). |

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

- **The IR**: text (`.lit`) and binary (`.lir`) forms, a verifier, source
  positions, a table of run-time functions.
- **The optimiser**: a pass manager and the first passes (CFG
  simplification, constant folding, global value numbering, dead code
  elimination).
- **A reference interpreter** of the IR, and a generator of random IR
  programs: every program runs before and after optimisation, and both
  runs must agree.
- **The Luxia front end**: lexer, parser (recursive descent, with a Pratt
  engine for expressions), semantic analysis, and SSA construction after
  Braun et al.
- **Nine benchmarks from the Computer Language Benchmarks Game**, translated
  to Luxia, print the expected output through the reference interpreter:
  binary-trees, fannkuch-redux, fasta, k-nucleotide, mandelbrot, n-body,
  pidigits, reverse-complement and spectral-norm
  ([`tests/luxia/benchmarks/`](tests/luxia/benchmarks/)).

```
limba program.luxia          # writes program.lir
lir_run -O1 program.lir      # runs it in the reference interpreter
```

## Roadmap

Done:

- [x] The IR, its forms, the verifier and the reference interpreter
- [x] The first optimisation passes and the random-program test net
- [x] Luxia 0: lexer, parser, semantic analysis, generation of the IR
- [x] The nine single-thread benchmarks run with the expected output

Next:

- [ ] A generator of random, well-typed Luxia programs, to test the front
      end the way the IR is already tested
- [ ] Close the known gaps of Luxia 0 (functions returning records and
      arrays, arrays with computed bounds freed on exit, `out` parameters
      checked)
- [ ] Settle the open points of the specification and publish it in
      English in `doc/luxia/`
- [ ] Measure the front end (target: about 1 µs per line) and the cost of
      the run-time checks
- [ ] More optimisation: promotion of memory to registers, removal of
      checks that range analysis proves useless, smaller passes
- [ ] **Meri**, the back end, in a separate repository: a register-based
      virtual machine, then AOT and JIT compilation
- [ ] **Luxia 1**: modules, exceptions, types for hardware (bit layouts,
      fixed addresses, volatile access)

## Building

    ./build.sh               # release build: bin/<cpu>-<os>/limba
    ./build.sh release test  # and the tests
    ./build.sh --help        # variants (debug, asan, ubsan, tsan), actions

A C11 compiler (GCC or Clang) and a POSIX shell are all it needs.

## Licence

GPL-3.0-or-later, see [`LICENSE`](LICENSE).
