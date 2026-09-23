# Limba

Limba is the front end of the Prabanta family, written in C: lexer, parser,
semantic analysis, an SSA intermediate representation and its optimisation
passes. It reads Luxia, an extended Pascal with the rigour of Ada, and
writes an intermediate representation that Meri, the back end (bytecode
compiler, virtual machine, AOT and JIT), executes.

Limba takes its ideas from SedaiBasic2, not its code.

**Status:** design phase; there is no code yet.

## Licence

GPL-3.0-or-later, see `LICENSE`.
