# Limba

Limba is the front end of the Prabanta family, written in C: lexer, parser,
semantic analysis, an SSA intermediate representation and its optimisation
passes. It reads Luxia, an extended Pascal with the rigour of Ada, and
writes an intermediate representation that Meri, the back end (bytecode
compiler, virtual machine, AOT and JIT), executes.

Limba takes its ideas from SedaiBasic2, not its code.

**Status:** the intermediate representation exists: its text (`.lit`)
and binary (`.lir`) forms, a verifier, and the `limba` program that
converts between them. The Luxia front end is not written yet.

## Building

    ./build.sh               # release build: bin/<cpu>-<os>/limba
    ./build.sh release test  # and the tests
    ./build.sh --help        # variants (debug, asan, ubsan, tsan), actions

A C11 compiler (GCC or Clang) and a POSIX shell are all it needs.

## Licence

GPL-3.0-or-later, see `LICENSE`.
