# Limba — The front end of Luxia as a library (draft)

`include/limba/limba_luxia.h` compiles a Luxia source into the IR of
`include/limba/ir.h`, in memory, and gives it to a **consumer** one
function at a time, as soon as each is complete. It is what a program
that runs the IR without writing a `.lir` needs, such as a back end
linked with Limba into a single executable; the `limba` command is itself
one such consumer, whose function callback writes the `.lir`.

The name reads "the Luxia language": *limba* is Sardinian for language.
Each language Limba compiles has its own header, `limba_<language>.h`,
and its own prefix.

## Compiling

```c
int limba_luxia_compile_file(const char *path,
                             const limba_luxia_options *o,
                             const limba_luxia_consumer *c,
                             limba_module **out);
int limba_luxia_compile_text(const char *name, const char *text, size_t len,
                             const limba_luxia_options *o,
                             const limba_luxia_consumer *c,
                             limba_module **out);
```

The second compiles bytes in memory (copied), named `name` in the
diagnostics and in the positions of the IR. `o` and `c` may be `NULL`:
the defaults, and no consumer. With the result `LIMBA_LUXIA_OK` and `out`
not `NULL`, `*out` is the module, freed by the caller with
`limba_module_free`; it holds the declarations, strings and types, and the
function bodies only if the consumer keeps them. Otherwise `*out` is
`NULL`.

The units the source uses (specification § 11) are read with it, and the
module holds them all, as one program: their names in the IR follow the
unit (`Geometry.Distance`; a unit of the standard library after
`$std.`), and only what an initialisation reaches is in it. A source
that is a unit is checked, with the units it uses: nothing is given to
the consumer, and `*out` is `NULL`.

| Result | Meaning |
|---|---|
| `LIMBA_LUXIA_OK` (0) | compiled |
| `LIMBA_LUXIA_ERRORS` (1) | the source has errors, or cannot be read |
| `LIMBA_LUXIA_OPTIONS` (2) | an option is not valid |
| `LIMBA_LUXIA_INTERNAL` (3) | the IR made is not valid, or a pass broke it: a fault of Limba |
| `LIMBA_LUXIA_STOPPED` (4) | the consumer asked to stop |

The numbers 0 to 3 are also the exit statuses of `limba`.

## Options

| Field | Meaning | `limba` option |
|---|---|---|
| `level` | 0 or 1: optimise or not | `-O0`, `-O1` |
| `opt` | the optimiser's options (`limba/opt.h`), `NULL` for the defaults; `verify_each` also verifies each function | `--skip`, `--stats`, `--verify` |
| `verify` | verify each function before it is given; the builds that are not release always do | `--check` |
| `suppress` | checks off in the whole source, separated by commas | `--suppress` |
| `target` | the platform the C types by name follow, `NULL` for the one that compiles | `--target` |
| `no_external` | no boundary with C | `--restrict=no_external` |
| `max_errors` | errors reported before giving up, 0 for 20 | |
| `diag_out` | where the diagnostics are printed, as `limba` prints them; `NULL` for nowhere | standard error |
| `no_free` | leave the memory of the compilation unfreed, for a process that ends right after (freeing piece by piece costs 3 %) | always in a release build |
| `unit_path` | the directories where the units of the program are looked for after the directory of the program, in order, a list ended by `NULL` | `-I DIR` |
| `stdlib_path` | the directory of the units of the standard library, `NULL` for none | `--stdlib=DIR` |
| `read_unit`, `read_ctx` | a reader of units held in memory, below | |

`read_unit(ctx, space, name, &text, &len, &path)` is asked for a unit
before the directories of its space: `space` is
`LIMBA_LUXIA_PROGRAM_UNIT` or `LIMBA_LUXIA_STDLIB_UNIT` (the same name may
be a unit of the program and one of the library), `name` the name of the
file without `.luxia`, in lowercase. It answers `LIMBA_LUXIA_UNIT_FOUND`,
with `text` and `len` read during the call and `path` the name of the
file in the diagnostics and in the positions of the IR, used as it is
(`geometry.luxia`, `<std>/geometry.luxia`); `LIMBA_LUXIA_UNIT_ABSENT`,
and the directories of that space are tried; or
`LIMBA_LUXIA_UNIT_UNREADABLE`, an error. A program that embeds the
standard library serves it with the reader and leaves the units of the
program on disk. The three fields are at the end of
`limba_luxia_options`: a caller that fills the structure by position
adds them.

An option that is not valid gives a diagnostic of no place and
`LIMBA_LUXIA_OPTIONS`, with the message `limba` gives.

## The consumer

| Field | Called |
|---|---|
| `begin(ctx, m)` | once, when the module is declared, before the first function |
| `func(ctx, m, fid)` | for each function, complete, verified if asked, optimised at the level asked; it returns 0 to go on, anything else to stop |
| `end(ctx, m, status)` | once, if `begin` was: `status` is the result, `m` the module if it is `LIMBA_LUXIA_OK`, `NULL` otherwise |
| `diag(ctx, d)` | for each diagnostic, printed on `diag_out` or not |
| `keep_bodies` | not a call: false frees each body (`limba_func_clear`) right after `func`, true keeps them all |

Any callback may be `NULL`. `ctx` is passed back unchanged.

A **diagnostic** (`limba_luxia_diag`) has its severity (error, warning,
note), its code (`"L0052"`, empty for none: see
[`diagnostics.md`](diagnostics.md)), the file, line and column from 1
(columns in characters) and the number of bytes marked, or no file for a
diagnostic of no place, and the message. The strings are valid during the
call only.

## What a consumer may rely on

1. **Declarations first.** Every function, global and extern of the module
   is declared before the first function is given; none is added later.
   Function 0 is `main`.
2. **Ids are stable, arrays are not.** While the bodies are made, strings,
   types and positions are added, at the end only: an id never changes.
   The arrays of the module (types, members, positions, the bytes of the
   strings) may be reallocated, so a consumer keeps ids from one call to
   the next, never pointers into the module.
3. **Callees may come later.** When a function is given, everything its
   body refers to exists (types, strings, globals, externs, the
   declarations of the functions with their types) except the bodies of
   the functions it calls: a routine declared after it, a recursion, and
   `main`, given last. Calls are resolved by index, once all are given.
4. **A function given is final.** It is never changed again.
5. **Errors may follow functions given.** Some errors are found while the
   bodies are made: a variable read before it has a value, an `out`
   parameter left without one, a missing return, a construct not
   translated yet. The functions given before stay given, none is given
   after the first error, and `end` comes with `LIMBA_LUXIA_ERRORS` and
   no module: the consumer throws away what it received.
6. **Errors before any function** (lexical, syntactic, semantic, an
   option, a file not read) call neither `begin` nor `end`.
7. **Stopping.** After `func` asks to stop, no other function is made:
   the front end ends at once, and errors in the bodies not made are not
   found. The result and the status of `end` are `LIMBA_LUXIA_STOPPED`.
8. **No shared state.** Two compilations share nothing: two threads may
   compile at once.
9. **Out of memory**, the process ends with status 70, as `limba` does:
   the front end does not give it back as an error.

## Example

```c
#include "limba/limba_luxia.h"

static int take(void *ctx, limba_module *m, limba_id fid)
{
    (void)ctx;
    /* translate m->funcs[fid] now: its body is freed on return */
    (void)m->funcs[fid];
    return 0;
}

int compile(const char *path)
{
    limba_luxia_options o = {0};
    o.level = 1;
    o.diag_out = stderr;
    limba_luxia_consumer c = {0};
    c.func = take;
    limba_module *m;
    int r = limba_luxia_compile_file(path, &o, &c, &m);
    if (r == LIMBA_LUXIA_OK) {
        /* resolve the calls, run */
        limba_module_free(m);
    }
    return r;
}
```
