# Limba — Diagnostics of Luxia 0 (draft)

This document lists the diagnostics that `limba` gives when it compiles a
Luxia 0 source: every code, the rule it enforces, a minimal program that
produces it and the message that program gives. The rules themselves are
those of the Luxia 0 specification; a reference such as "§ 5.4" points
to a section of it.

## Introduction

### What a diagnostic looks like

A diagnostic names the file, the line and the column, its severity and its
code, and a message; below it come the line of source and a mark under the
part it refers to (`^` at the first character, `~` under the rest):

```
prog.luxia:3:11: error[L0003]: string not closed on its line
 3 |   writeln("hello);
   |           ^~~~~~~~
```

Columns count characters, not bytes, from 1. Some errors are followed by a
**note**, which has no code and points at a second place, for example the
first declaration of a name declared twice. A note that stands alone, with
no error before it, has a code like the others (L0080):

```
prog.luxia:3:5: error[L0023]: 'Total' is already declared here
 3 |     Total: Int64 := 0;
   |     ^~~~~
prog.luxia:2:5: note: the first declaration
```

Diagnostics are written on the standard error, in the order in which they
are found.

### Errors and warnings

An **error** means the program is rejected: nothing is written. A
**warning** reports something legal but suspect; the program is compiled
all the same. In Luxia 0 only one code is a warning, L0020 (an `end` not
aligned with its opening); every other code is an error. There is no
option to silence or promote warnings. (The option `--suppress` has
nothing to do with diagnostics: it turns off run-time checks, § 10.3.)

### Stable codes

Each diagnostic has a code `L` followed by four digits. A code never
changes meaning and is never reused; new codes are added after the last
one. The wording of a message may improve from one version to the next,
the code does not change: tools and tests should rely on the code, the
line and the column.

### The phases, and how many errors are reported

`limba` checks a source in phases:

1. the **lexical** analysis (L0001–L0009) and the **syntax** (L0010–L0020);
2. the **semantic** analysis: names, types, constants, statements;
3. the **translation** to the intermediate representation, which also
   checks definite assignment, `out` parameters and missing `return`s
   (L0052, L0053, L0056) and reports what is not translated yet (L0054).

A phase runs only if the previous one found no errors (a warning does not
count): the semantic errors of a program with syntax errors would mostly
be echoes of those errors. Within a phase `limba` goes on after an error
and reports as many as it can; an error may still be followed by a few
that are its consequence, so the first one is the one to fix first.

After **20 errors** `limba` stops reporting and ends with the line

```
limba: stopped after 20 errors
```

### Exit status

| Status | Meaning |
|---|---|
| 0 | the source is valid (warnings may have been reported) |
| 1 | errors were reported, or the source could not be read |
| 2 | a mistake on the command line (an unknown option, a bad `--suppress` list) |
| 3 | an internal error of the compiler |

`limba --check prog.luxia` runs every phase and writes nothing, which is
the quickest way to have a source checked.

### Run-time errors

This document covers the errors found at compile time. The errors found
while the program runs (overflow, index, range, `nil`, division by zero,
conversion, shift count, an error of input or output) are a separate
matter: they stop the program with a message such as `luxia: index out
of range at prog.luxia:12:5` and exit status 1, and they are described in
§ 10 of the specification. An output closed by its reader ends the
program without a message and with exit status 141.

## Lexical errors

### L0001 — BAD UTF-8

The source must be UTF-8 (§ 2.1). The error points at the first byte that
is not valid UTF-8; the file is then not compiled at all, and no other
error follows.

```pascal
program p;
begin
  writeln("café");   // saved in Latin-1: é is the single byte 0xE9
end.
```

```
prog.luxia:3:15: error[L0001]: the source must be UTF-8: this byte is not, and nothing of the file is compiled
```

### L0002 — BAD CHARACTER

A character that starts no token: the symbols `@ # $ ! ? % | ~ \`, the
braces `{ }`, any non-ASCII character outside comments and literals
(§ 2.1), and `_` at the start of a name (§ 2.2, message "a name starts
with a letter, not with '_'").

```pascal
program p;
var a: Int32 := 1;
begin
  a := a # 2;
end.
```

```
prog.luxia:4:10: error[L0002]: '#' starts no token of Luxia
```

### L0003 — OPEN STRING

A string must be closed on the line where it starts (§ 2.5).

```pascal
program p;
begin
  writeln("hello);
end.
```

```
prog.luxia:3:11: error[L0003]: string not closed on its line
```

### L0004 — OPEN COMMENT

A block comment `(* ... *)` must be closed; block comments nest, so each
`(*` needs its own `*)` (§ 2.6).

```pascal
program p;
(* a comment
begin
end.
```

```
prog.luxia:2:1: error[L0004]: comment never closed: (* needs its *)
```

### L0005 — BAD CHARACTER LITERAL

A character literal holds exactly one character between apostrophes; the
apostrophe itself is `'''` (§ 2.5). A literal runs to the apostrophe that
closes it on the same line, so `'ab'` is a single error. The empty literal
`''` gives the message "a character literal holds one character: 'a',
''' for the apostrophe".

```pascal
program p;
var c: Char := 'ab';
begin
end.
```

```
prog.luxia:2:16: error[L0005]: a character literal must hold one character and be closed by an apostrophe, as in 'a'
```

### L0006 — KEYWORD CASE

Keywords are written in lowercase only; `BEGIN` or `Begin` is an error,
not a name (§ 2.3).

```pascal
program p;
BEGIN
end.
```

```
prog.luxia:2:1: error[L0006]: the keyword 'begin' must be written in lowercase only
```

### L0007 — BAD NUMBER

A malformed number (§ 2.4). The messages are:

- "'_' may appear only between two digits";
- "a number needs digits here" (as in `0x` with no digits);
- "'…' cannot follow the digits of this number" (a letter attached, as in
  `12abc`, or a digit outside the base, as in `0b102`);
- "the base prefix must be written in lowercase: 0x, 0o, 0b";
- "the exponent must be written with a lowercase e".

```pascal
program p;
var n: Int32 := 1__000;
begin
end.
```

```
prog.luxia:2:17: error[L0007]: '_' may appear only between two digits
```

### L0008 — REAL OUT OF RANGE

No longer given. A real literal is an exact value of any size (§ 2.4,
§ 4.2): `1e400 / 1e390` is the constant `1e10`. A constant too large for
the type it takes is L0029:

```pascal
program p;
const Big: Float64 = 1e400;
begin
end.
```

```
prog.luxia:2:22: error[L0029]: the constant is too large for Float64
```

### L0009 — CONTROL CHARACTER IN LITERAL

Character and string literals contain no control characters, the tab
included; they are written with the constants `LF`, `CR`, `TAB`, `NUL` or
with `chr(n)` and joined with `&` (§ 2.5, § 2.7).

```pascal
program p;
begin
  writeln("a	b");   // a real tab between a and b
end.
```

```
prog.luxia:3:13: error[L0009]: no control characters in a literal: use LF, CR, TAB or NUL
```

## Syntax errors

### L0010 — EXPECTED

A token other than the one the grammar wants (§ 12). The message is
"expected …, found …", naming what was wanted and what was found; when a
structured statement is not closed, a note shows where it was opened. Two
messages under this code are more specific: "a constant is given with
'=', not ':='" and "a procedure has no result: declare a function"; a
third says that a comparison, `in`, `and`, `or` or `xor` in an element of
an aggregate goes in parentheses (`{(a < b), true}`, § 6.8).

```pascal
program p;
var a: Int32 := 1
begin
end.
```

```
prog.luxia:2:18: error[L0010]: expected ';' after the declaration, found 'begin'
```

### L0011 — MIXED LOGICAL OPERATORS

`and`, `or` and `xor` are not mixed without parentheses (§ 6.1).

```pascal
program p;
var a, b, c: Boolean := true;
begin
  a := a and b or c;
end.
```

```
prog.luxia:4:16: error[L0011]: 'or' after 'and' needs parentheses: write '(a and b) or c' or 'a and (b or c)'
```

### L0012 — CHAINED OPERATORS

Comparisons and `**` do not associate: `a < b < c`, `a = b <> c` and
`a ** b ** c` are errors (§ 6.1).

```pascal
program p;
var a, b, c: Int32 := 0;
begin
  writeln(a < b < c);
end.
```

```
prog.luxia:4:17: error[L0012]: '<' after '<': these operators do not chain, add parentheses
```

### L0013 — PREFIX OPERATOR NEEDS PARENTHESES

A prefix operator (`-`, `+`, `not`, `abs`) does not follow a binary
operator or another prefix operator directly: `a * -b` is written
`a * (-b)`, `2 ** -1` is written `2 ** (-1)` (§ 6.1).

```pascal
program p;
var a, b: Int32 := 1;
begin
  a := a * -b;
end.
```

```
prog.luxia:4:12: error[L0013]: '-' cannot follow an operator here: put it in parentheses, as in 'a * (-b)'
```

### L0014 — PREFIX OPERATOR BEFORE POWER

`not` and `abs` share the level of `**`, so `not a ** b` would be
ambiguous; the parentheses say which is meant (§ 6.1).

```pascal
program p;
var a: Bits8 := 3;
begin
  a := not a ** 2;
end.
```

```
prog.luxia:4:14: error[L0014]: '**' after 'not' needs parentheses: write '(not a) ** b' or 'not (a ** b)'
```

### L0015 — WRONG NAME AFTER END

The name after the `end` of a routine or of the program is optional; if
it is written, it must be the name of what the `end` closes (§ 8, § 11).
The message shows the name as declared. A name that differs only in case,
such as `end show;` for a routine declared `Show` or `end P.` for
`program p`, is not this error but L0022: the name after `end` is a use
of the name and must be spelt as declared.

```pascal
program p;
procedure Show();
begin
end Shw;
begin
end.
```

```
prog.luxia:4:5: error[L0015]: this end closes 'Show': write that name or none
```

### L0016 — NOT A STATEMENT

A statement is an assignment, a call with parentheses, or a structured
statement (§ 7, § 7.7). The messages are "'=' compares: assign with ':='",
"a statement assigns (x := ...) or calls a routine (P(...), P() without
arguments)" (as for `f;`), and "Luxia has no begin blocks: the statements
go straight after then, do, else".

```pascal
program p;
var x: Int32 := 0;
begin
  x = 1;
end.
```

```
prog.luxia:4:5: error[L0016]: '=' compares: assign with ':='
```

### L0017 — NESTED ROUTINE

Routines are declared only at program level in Luxia 0 (§ 8.4).

```pascal
program p;
procedure Outer();
  procedure Inner();
  begin
  end;
begin
end;
begin
end.
```

```
prog.luxia:3:3: error[L0017]: a routine cannot be declared inside another one in Luxia 0
```

### L0018 — EMPTY STATEMENT

`;` terminates a statement; there is no empty statement, so `;;` is an
error (§ 2.8).

```pascal
program p;
var x: Int32 := 0;
begin
  x := 1;;
end.
```

```
prog.luxia:4:10: error[L0018]: an empty statement: remove this ';'
```

### L0019 — TEXT AFTER THE END

Nothing but comments may follow the final `end.` of the program (§ 11).

```pascal
program p;
begin
end.
writeln(1);
```

```
prog.luxia:4:1: error[L0019]: nothing may follow the end of the program
```

### L0020 — END NOT ALIGNED (warning)

An `end` on a line of its own should stand in the column where the line
of its opening starts (§ 8). This is the only warning of Luxia 0: the
program is compiled all the same and the exit status is 0. There is no
warning when the `end` is on the same line as its opening.

```pascal
program p;
var x: Int32 := 0;
begin
  if x > 0 then
    x := 1;
    end;
end.
```

```
prog.luxia:6:5: warning[L0020]: this 'end' closes the 'if' of line 4: put it in column 3, under the start of that line
```

## Names and declarations

### L0021 — UNKNOWN NAME

A name used but declared nowhere visible (§ 5.4).

```pascal
program p;
begin
  total := 1;
end.
```

```
prog.luxia:3:3: error[L0021]: 'total' is not declared
```

### L0022 — SPELLING

Names are unique regardless of case, but every use is spelt exactly as
the declaration (§ 2.2). The name after the `end` of a routine or of the
program counts as a use: `end show;` closing a routine declared `Show`
gives "'show' is declared as 'Show': write it the same way".

```pascal
program p;
var Count: Int32 := 0;
begin
  count := 1;
end.
```

```
prog.luxia:4:3: error[L0022]: 'count' is declared as 'Count': write it the same way
```

### L0023 — DUPLICATE DECLARATION

A name declared twice in the same scope, even with a different case
(§ 2.2, § 5.4). A note points at the first declaration. The same code is
given to the declaration of a name of the language, such as `writeln` or
`Int32` ("'writeln' is a name of the language", § 9), and to a second
field with the same name in a record ("a second field with this name").

```pascal
program p;
var total: Int32 := 0;
    Total: Int64 := 0;
begin
end.
```

```
prog.luxia:3:5: error[L0023]: 'Total' is already declared here
```

### L0024 — USED BEFORE ITS DECLARATION

Constants and variables are visible only after their declaration; types
and routines are visible in their whole scope (§ 5.4). A note points at
the declaration.

```pascal
program p;
const A = B + 1;
const B = 1;
begin
end.
```

```
prog.luxia:2:11: error[L0024]: 'B' is used before its declaration
```

### L0025 — NOT A TYPE

A name that is not a type where a type is wanted.

```pascal
program p;
var n: Int32 := 0;
var m: n := 0;
begin
end.
```

```
prog.luxia:3:8: error[L0025]: 'n' is not a type
```

### L0026 — NOT A VALUE

A type or a routine used where a value is wanted. For a type the message
suggests a conversion (§ 6.6); for a routine it is "'…' is a routine:
call it with …(...)" (calls always have parentheses, § 7.7).

```pascal
program p;
var x: Int32 := 0;
begin
  x := Int32;
end.
```

```
prog.luxia:4:8: error[L0026]: 'Int32' is a type: to convert a value write 'Int32(x)'
```

### L0047 — SELF REFERENCE

A type or a constant defined in terms of itself; a record that contains
itself other than through a pointer ("a record cannot contain itself:
use a pointer") (§ 5.4).

```pascal
program p;
type A = B;
     B = A;
begin
end.
```

```
prog.luxia:2:6: error[L0047]: 'A' is defined in terms of itself
```

## Types and operators

### L0027 — TYPE MISMATCH

A value of another type than the one wanted: the two operands of a
binary operator or of a comparison, the target of an assignment, a
parameter, the result of a function, a condition that is not `Boolean`
(§ 6.2, § 7.1). There are no implicit conversions (§ 1.1). The same code
covers the other misuses of a type: a real constant where an integer is
wanted, `nil` where no pointer is wanted, a `case` selector or `for`
counter that is not discrete, an array index type that is not discrete,
an argument of a library routine of the wrong kind (`ord` of an integer,
`low` of a real type, `sqrt` of an integer, `write` of an enumeration,
and so on, § 9).

```pascal
program p;
var a: Int32 := 0;
var b: Int64 := 0;
begin
  a := b;
end.
```

```
prog.luxia:5:8: error[L0027]: this is Int64, the variable wants Int32
```

### L0028 — OPERATOR NOT FOR THIS TYPE

An operator applied to a type that does not take it: unary minus on an
unsigned number, `/` on integers, `div` on reals, shifts and bitwise
operators outside the `BitsN` types, and so on (§ 3.2, § 6.2, § 6.3).

```pascal
program p;
var u: UInt32 := 1;
begin
  u := -u;
end.
```

```
prog.luxia:4:8: error[L0028]: '-' negates signed numbers and Bits values: it does not apply to UInt32
```

### L0039 — BAD CONVERSION

A conversion `T(x)` between types that do not convert into each other
(§ 6.6). A conversion takes exactly one value (L0035 otherwise).

```pascal
program p;
var s: String := String(1);
begin
end.
```

```
prog.luxia:2:18: error[L0039]: an integer constant does not convert to String
```

### L0040 — NO SUCH FIELD

A selection `r.x` on a record without a field `x`, or on a value that is
not a record ("… has no fields") (§ 3.9).

```pascal
program p;
type Point = record x, y: Float64; end;
var q: Point;
begin
  q.z := 1.0;
end.
```

```
prog.luxia:5:4: error[L0040]: Point has no field 'z'
```

### L0041 — NOT INDEXABLE

An index `a[i]` on a value that is neither an array, a string nor a
pointer to an array (§ 3.7, § 3.8, § 3.10).

```pascal
program p;
var n: Int32 := 0;
begin
  writeln(n[1]);
end.
```

```
prog.luxia:4:12: error[L0041]: Int32 has no index
```

### L0042 — NOT A POINTER

A dereference `p^` of a value that is not a pointer, or `dispose` of one
("dispose frees a pointer") (§ 3.10, § 9.7).

```pascal
program p;
var n: Int32 := 0;
begin
  writeln(n^);
end.
```

```
prog.luxia:4:12: error[L0042]: Int32 is not a pointer
```

### L0050 — BAD RANGE

A range on a type that has none: only discrete types (integers,
enumerations, `Char`, `Boolean`) take a range (§ 3.4). An empty range
(`lo > hi`) is legal and is not this error.

```pascal
program p;
var f: Float64 range 0.0..1.0;
begin
end.
```

```
prog.luxia:2:8: error[L0050]: Float64 has no range: only discrete types do
```

## Constants

### L0029 — CONSTANT OUT OF RANGE

A value known at compile time that does not fit where it goes (§ 4.1,
§ 4.2): a literal or constant outside the range of its type, an integer
constant not exact in a real type ("this integer is not exact in
Float32: convert it explicitly"), a real constant too large for its type,
a negative exponent on an integer, a shift by the width of the value or
more, a width or number of decimals out of its limits in `write`, a
constant argument of `arg` below 1, the bounds of an array argument
outside the index of an open-array parameter, an array or record larger
than the memory can address.

```pascal
program p;
var b: UInt8 := 300;
begin
end.
```

```
prog.luxia:2:17: error[L0029]: 300 is out of the range of UInt8
```

### L0030 — NOT CONSTANT

A constant needs a value known at compile time; a value computed at run
time is declared with `var` (§ 4.3). The labels of a `case` are constants
too ("the values of a case are known at compile time", § 7.3), and so are
the indices of an aggregate and every component of a typed constant of a
record or array type, where another typed constant is not a constant
(§ 4.3, § 6.8).

```pascal
program p;
var n: Int32 := 3;
const M = n;
begin
end.
```

```
prog.luxia:3:11: error[L0030]: a constant needs a value known at compile time: for a computed one declare a var
```

### L0031 — CONSTANT TOO LARGE

Constant expressions are computed exactly (§ 4.2), up to a limit of
16384 bits for an integer or for the numerator and denominator of a
rational; a number or a result past it is this error.

```pascal
program p;
const Huge = 2 ** 100000;
begin
end.
```

```
prog.luxia:2:16: error[L0031]: this constant is past 16384 bits
```

### L0032 — DIVISION BY ZERO IN A CONSTANT

A constant divided by zero (`/`, `div`, `mod`, `rem`), or zero raised to a
negative power ("zero to a negative power"). Between computed values the
division by zero is a run-time error (§ 6.3).

```pascal
program p;
const D = 1 div 0;
begin
end.
```

```
prog.luxia:2:13: error[L0032]: a constant divided by zero
```

### L0033 — TYPE NEEDED

A constant without a type cannot give a type (§ 4.1, § 5.3): a variable
declared with a constant initialiser and no type, a `for` whose bounds
are both constants without a type (§ 7.5), a shift, `not`, `and`, `or`
or `xor` between constants without a `BitsN` type, `sqrt` and the other
mathematical functions of a constant where no real type is expected, an
aggregate where nothing gives it a type (§ 6.8).

```pascal
program p;
begin
  var x := 0;
end.
```

```
prog.luxia:3:12: error[L0033]: a constant without a type gives none to the variable: write 'var x: T := ...'
```

## Assignment and calls

### L0034 — NOT ASSIGNABLE

The target of an assignment, or an argument for a `var` or `out`
parameter, must be a variable that can change. Other messages: "the
variable of a for is constant in the loop" (§ 7.5), "a parameter without
var or out is read-only" (§ 8.1), "a String does not change: build a new
one" (§ 3.3).

```pascal
program p;
const Limit = 10;
begin
  Limit := 20;
end.
```

```
prog.luxia:4:3: error[L0034]: a constant cannot change
```

### L0035 — WRONG NUMBER OF ARGUMENTS

A call with too many or too few arguments; there are no default
parameters (§ 8.4). The same code is given to a conversion with other
than one value and to a library routine called with the wrong number of
arguments.

```pascal
program p;
procedure Show(n: Int32);
begin
end;
begin
  Show();
end.
```

```
prog.luxia:6:3: error[L0035]: 'Show' takes 1 argument, not 0
```

### L0036 — NOT CALLABLE

A call of something that is not a routine or a type (§ 7.7). When what is
called is not a name at all the message is "only routines are called".

```pascal
program p;
var x: Int32 := 0;
begin
  x();
end.
```

```
prog.luxia:4:3: error[L0036]: 'x' is not a routine
```

### L0037 — RESULT IGNORED

The result of a function cannot be ignored: a function called as a
statement is an error (§ 7.7).

```pascal
program p;
function Five(): Int32;
begin
  return 5;
end;
begin
  Five();
end.
```

```
prog.luxia:7:3: error[L0037]: the result of a function cannot be ignored: use it
```

### L0038 — NO RESULT

A procedure used as a value.

```pascal
program p;
procedure Show();
begin
end;
var x: Int32 := Show();
begin
end.
```

```
prog.luxia:5:17: error[L0038]: a procedure gives no value: call it as a statement
```

## Statements

### L0043 — CASE DUPLICATE

A value may not appear in two branches of a `case`, singly or inside a
range (§ 7.3).

```pascal
program p;
var c: Int32 := 0;
begin
  case c of
    when 1..5: c := 1;
    when 3: c := 2;
    else c := 3;
  end;
end.
```

```
prog.luxia:6:10: error[L0043]: this value is already in another branch
```

### L0044 — CASE COVERAGE

A `case` without `else` must cover every value of the type of its
selector (§ 7.3).

```pascal
program p;
type Colour = (Red, Green, Blue);
var c: Colour := Red;
begin
  case c of
    when Red: c := Green;
  end;
end.
```

```
prog.luxia:5:3: error[L0044]: some values of Colour have no branch: add them or an else
```

### L0045 — OUTSIDE A LOOP

`exit` and `continue` are used only inside a loop (§ 7.6). The program
body and a routine are left with `return`.

```pascal
program p;
begin
  exit;
end.
```

```
prog.luxia:3:3: error[L0045]: 'exit' is allowed only inside a loop
```

### L0046 — RETURN VALUE

Only a function returns a value; a function always returns one (§ 8.2).
The other message is "a function returns a value: return x", for a bare
`return` in a function.

```pascal
program p;
procedure Q();
begin
  return 1;
end;
begin
end.
```

```
prog.luxia:4:3: error[L0046]: only a function returns a value
```

### L0055 — HALT CODE

The exit status given to `halt` is 0 or 2..255 except 141: 1 is reserved
for the run-time errors, 141 for an output closed by its reader (§ 10.1).
A constant outside these values is a compile-time error; a computed one
is checked at run time (§ 9.8).

```pascal
program p;
begin
  halt(1);
end.
```

```
prog.luxia:3:8: error[L0055]: an exit status is 0 or 2..255 but 141: 1 is kept for the errors at run time, 141 for a closed output
```

## Where a construct is allowed

### L0048 — FORMAT OUTSIDE WRITE

`x:width` and `x:width:decimals` are allowed only in the arguments of
`write` and `writeln`, and the decimals only on reals ("decimals are for
real numbers") (§ 9.1).

```pascal
program p;
var s: String := str(1:2);
begin
end.
```

```
prog.luxia:2:22: error[L0048]: 'x:width:decimals' is allowed only in write and writeln
```

### L0049 — OPEN ARRAY OUTSIDE A PARAMETER

An open array `array[I range <>] of T` is the type of a parameter or
the type a pointer points to; a variable, a field or a function result
cannot have it. `range <>` outside the index of an array gives "'range
<>' is the index of an array parameter only" (§ 3.7.1). The same code
reports `new(A)` without the bounds of an open array ("an open array
needs the bounds of its index: new(A range low..high)") and an array
made by `new` used as a whole, outside `low`, `high`, `length`,
`move`, `translate`, `reverse`, `occurrences`, `readbytes` and
`writebytes` ("an array made by new is used through its elements, low,
high, length, move, translate, reverse, occurrences, readbytes and
writebytes: not as a whole", § 3.10).

```pascal
program p;
var v: array[Int32 range <>] of Int32;
begin
end.
```

```
prog.luxia:2:8: error[L0049]: an array with the bounds of its argument is a type for parameters and pointers only
```

### L0051 — COMPUTED BOUNDS OUT OF PLACE

Bounds computed at run time are allowed only in the index of the array
of a variable, not in a type declared with `type` nor in a record field
(§ 3.7).

```pascal
program p;
var n: Int32 := 3;
type Row = array[Int32 range 1..n] of Int32;
begin
end.
```

```
prog.luxia:3:18: error[L0051]: bounds computed at run time are allowed only in the index of the array of a variable
```

## Flow of values

These errors are found on the translation of the program, so they are
reported only when the program has no other errors.

### L0052 — MISSING RETURN

A function must return on every path; the error points at the name of the
function (§ 8.2).

```pascal
program p;
function Sign(x: Int32): Int32;
begin
  if x > 0 then
    return 1;
  end;
end;
begin
  writeln(Sign(1));
end.
```

```
prog.luxia:2:10: error[L0052]: a path reaches the end of the function without return
```

### L0053 — READ BEFORE ASSIGNMENT

A scalar variable, or a scalar `out` parameter, read on a path where it
has not been given a value (§ 5.5, § 8.1).

```pascal
program p;
begin
  var y: Int32;
  writeln(y);
end.
```

```
prog.luxia:4:11: error[L0053]: 'y' may be read before it is given a value
```

### L0056 — OUT PARAMETER WITHOUT A VALUE

A scalar `out` parameter must be assigned on every path out of the
routine (§ 8.1). The error points at the parameter.

```pascal
program p;
procedure Get(out r: Int32; x: Int32);
begin
  if x > 0 then
    r := x;
  end;
end;
var v: Int32 := 0;
begin
  Get(v, 1);
end.
```

```
prog.luxia:2:11: error[L0056]: the out parameter 'r' may leave without a value
```

## Limits of the compiler

### L0054 — UNSUPPORTED

A valid program that this version of `limba` cannot translate yet. Two
cases exist: a `case` label that is a range of more than 4096 values, and
a value too large for the stack ("a value too large for the stack is not
translated yet"). These are limits of the compiler, not rules of the
language.

```pascal
program p;
var c: Int32 := 0;
begin
  case c of
    when 1..10000: c := 1;
    else c := 2;
  end;
end.
```

```
prog.luxia:5:10: error[L0054]: a range of more than 4096 values in a case is not translated yet
```

## Pragmas

### L0057 — UNKNOWN PRAGMA

The pragmas of Luxia 0 are `suppress` and `unsuppress` (§ 10.3),
`convention` (§ 3.13) and `restrictions` (§ 10.4).

```pascal
program p;
pragma supress(range_check);
begin
end.
```

```
prog.luxia:2:8: error[L0057]: the pragmas are suppress, unsuppress, convention and restrictions, not 'supress'
```

### L0058 — UNKNOWN CHECK

A pragma names checks among `index_check`, `range_check`,
`overflow_check`, `division_check`, `conversion_check`, `shift_check`,
`nil_check`, `dangling_check` and `all_checks` (§ 10.3).

```pascal
program p;
pragma suppress(range_chek);
begin
end.
```

```
prog.luxia:2:17: error[L0058]: 'range_chek' is no check: index_check, range_check, overflow_check, division_check, conversion_check, shift_check, nil_check, dangling_check or all_checks
```

### L0059 — ARGUMENT THROUGH A POINTER

In Luxia 0 a record or an array reached through a pointer (`p^`, `p.f`,
`p[i]`, `p.a[i]`, in any chain) is never an argument, in any mode, and a
scalar reached so is no `var` argument, `readline` and `val` included
(§ 3.10): a `dispose` during the call would leave the routine on freed
memory, where no check can see it, or cost a hidden copy. Pass the
pointer itself, or copy the object into a variable first. A scalar `in`
or `out` argument is allowed; only `move` takes arrays reached through a
pointer (§ 9.5).

```pascal
program p;
type R = record a: Int32; end;
var q: ^R;
procedure inc(var x: Int32);
begin
  x := x + 1;
end inc;
begin
  q := new(R);
  inc(q.a);
end p.
```

```
prog.luxia:10:8: error[L0059]: an object reached through a pointer cannot be a var argument: copy it into a variable, pass that, then assign it back
```

With `procedure fill(out r: R)`, `fill(q^)` gives (as `show(q^)` for a
`procedure show(r: R)`):

```
prog.luxia:10:9: error[L0059]: a record or an array reached through a pointer cannot be an argument: pass the pointer, or copy it into a variable first
```

## The boundary with C

### L0060 — DOES NOT CROSS TO C

An external routine (§ 8.5) takes and returns only what has a fixed
meaning in C: integers of fixed size, reals, the C types by name, the
opaque pointers, records with the C convention; arrays as parameters,
`var` and `out` as addresses. A record with the C convention holds only
such fields (§ 3.13). The message says what to use instead.

```pascal
program p;
procedure Puts(s: String);
  external "c" name "puts";
begin
end.
```

```
prog.luxia:2:19: error[L0060]: String does not cross to C: convert it with newcstring and pass a CString
```

### L0061 — BOUNDARY FORBIDDEN

With `pragma restrictions(no_external)` among the declarations of the
program, or `limba --restrict=no_external`, the program may not cross to
C: no external routine, no C type by name, `CPointer`, `CString`, nor
`pragma convention` (§ 10.4).

```pascal
program p;
pragma restrictions(no_external);
procedure Beep();
  external "c" name "beep";
begin
end.
```

```
prog.luxia:3:11: error[L0061]: 'Beep' is an external routine, which pragma restrictions(no_external) forbids
```

### L0062 — C PRAGMA WRITTEN WRONG

`pragma convention(c, R)` names the C convention and a record type `R`
declared in the same declarations; `pragma restrictions(no_external)`
names the one restriction, among the declarations of the program or of a
unit; `pragma hides(Unit.Name)` names a name of a unit the file uses
(§ 11.3). None is a statement.

```pascal
program p;
type Pair = record a, b: CInt; end;
pragma convention(cpp, Pair);
begin
end.
```

```
prog.luxia:3:1: error[L0062]: the convention is C: 'pragma convention(c, R)', R a record type declared here
```

### L0063 — FIELD OR ELEMENT AFTER ^

A field or an element is reached through a pointer without `^`: `p.x`
and `p[i]`, never `p^.x` nor `p^[i]`, one form for each thing (§ 3.10).
`p^` alone is the whole object.

```pascal
program p;
type Node = record v: Int32; end;
var q: ^Node;
begin
  q := new(Node);
  q^.v := 1;
end.
```

```
prog.luxia:6:5: error[L0063]: a field is reached through a pointer without '^': write p.x, not p^.x
```

### L0064 — UNIT NOT FOUND

A unit used is in the file of its name in lowercase, looked for in the
directory of the program, then in the directories of `-I`, then in the
standard library (§ 11.4).

```pascal
program p;
uses Shapes;
begin
end.
```

```
prog.luxia:2:6: error[L0064]: the unit 'Shapes' is not found: no shapes.luxia in the directory of the program
```

### L0065 — FILE OF ANOTHER UNIT

The file `shapes.luxia` holds the unit `Shapes`, in any spelling; a
program is not a unit (§ 11.4).

```pascal
// prog.luxia
program p;
uses Shapes;
begin
end.

// shapes.luxia
unit Forms;
interface
implementation
end Forms.
```

```
shapes.luxia:1:1: error[L0065]: shapes.luxia holds the unit 'Forms', not 'Shapes': a unit is in the file of its name, in lowercase
```

### L0066 — INTERFACES THAT USE EACH OTHER

Two units may use each other only if one of the two `uses` is in an
implementation; a unit does not use itself (§ 11.3).

```pascal
// left.luxia
unit Left;
interface
uses Right;
implementation
end Left.

// right.luxia
unit Right;
interface
uses Left;
implementation
end Right.
```

```
right.luxia:3:6: error[L0066]: the interfaces use each other: Left -> Right -> Left; one of the uses goes in an implementation
```

### L0067 — UNIT USED TWICE

A unit is named once in the `uses` of a file, the two parts of a unit
together (§ 11.3).

```pascal
program p;
uses Shapes, Shapes;
begin
end.
```

```
prog.luxia:2:14: error[L0067]: 'Shapes' is already used here
```

### L0068 — ROUTINE WITHOUT BODY

Every routine of an interface has its body in the implementation, unless
it is external (§ 11.2).

```pascal
unit Shapes;
interface
procedure Draw();
implementation
end Shapes.
```

```
shapes.luxia:3:11: error[L0068]: 'Draw' has no body in the implementation
```

### L0069 — HEADING NOT CONFORMING

The heading of a body conforms to the one of the interface: the same
kind, names, spelling, order and modes; the same types, however written
(§ 11.2).

```pascal
unit Shapes;
interface
function Area(side: Int64): Int64;
implementation
function Area(s: Int64): Int64;
begin
  return s * s;
end;
end Shapes.
```

```
shapes.luxia:5:10: error[L0069]: the heading is not the one of the interface: the parameter 's' is 'side' in the interface
shapes.luxia:3:1: note: the heading in the interface
```

### L0070 — AMBIGUOUS NAME

A name written directly that two units of the same level give is an
error where it is used; the qualified form says which (§ 11.3).

```pascal
// prog.luxia
program p;
uses Shapes, Solids;
begin
  writeln(Volume(2));
end.
```

Both `Shapes` and `Solids` declare `Volume` in their interface.

```
prog.luxia:4:11: error[L0070]: 'Volume' is given by 'Shapes' and by 'Solids': write Shapes.Volume or Solids.Volume
```

### L0071 — NAME THAT HIDES ONE OF A UNIT

A warning. A declaration of the file hides a name of a unit it uses,
unless `pragma hides(Unit.Name)` says it is meant; a name of a unit of
the program written directly hides one of the library (§ 11.3).

```pascal
program p;
uses Shapes;     // Shapes declares Sides
var Sides: Int64 := 3;
begin
  writeln(Sides, Shapes.Sides);
end.
```

```
prog.luxia:3:5: warning[L0071]: 'Sides' hides 'Shapes.Sides': if it is meant, say it with pragma hides(Shapes.Sides)
```

### L0072 — NAME OF A UNIT DECLARED

The name of a unit the file uses, or of the unit itself, is not declared
in the file, at any level but the fields of a record; a unit is not named
as a name of the language (§ 11.3).

```pascal
program p;
uses Shapes;
var Shapes: Int64;
begin
end.
```

```
prog.luxia:3:5: error[L0072]: 'Shapes' is the name of a unit used here: it cannot be declared in this file
```

### L0073 — NOT IN THE INTERFACE

`Unit.Name` names what the interface of the unit declares (§ 11.3).

```pascal
program p;
uses Shapes;     // Hidden is declared in its implementation
begin
  writeln(Shapes.Hidden);
end.
```

```
prog.luxia:4:18: error[L0073]: 'Hidden' is not in the interface of 'Shapes'
```

### L0074 — VARIABLE OF AN INTERFACE WRITTEN

Outside its unit a variable of an interface is read only: it is not
assigned, nor passed as `var` or `out`, nor are its fields and elements
(§ 11.3).

```pascal
program p;
uses Counter;    // its interface declares var count: Int64
begin
  Counter.count := 0;
end.
```

```
prog.luxia:4:11: error[L0074]: 'count' is a variable of an interface: outside its unit it is read only; change it with a routine of the unit
```

### L0075 — RETURN IN AN INITIALISATION

The initialisation of a unit ends at its end (§ 11.5).

```pascal
unit Counter;
interface
var count: Int64;
implementation
begin
  count := 1;
  return;
end Counter.
```

```
counter.luxia:7:3: error[L0075]: the initialisation of a unit has no return: it ends at its end
```

### L0076 — INITIALISATIONS THAT NEED EACH OTHER

The order of the initialisations is computed from what they can read and
write; when the constraints form a circle there is no order. The
constraint comes from the text, a branch that never runs included
(§ 11.5).

```pascal
// left.luxia
unit Left;
interface
var a: Int64;
implementation
uses Right;
begin
  a := Right.b + 1;
end Left.

// right.luxia
unit Right;
interface
var b: Int64;
implementation
uses Left;
begin
  b := Left.a + 1;
end Right.
```

```
left.luxia:1:6: error[L0076]: the initialisations need each other: Left after Right (the initialisation of Left can reach Right.b at left.luxia:7:14); Right after Left (the initialisation of Right can reach Left.a at right.luxia:7:13); the constraint comes from the text, also from a branch that never runs: move the code
```

### L0077 — UNIT NEVER NAMED

A warning: a unit used and never named in the file (§ 11.3).

```pascal
program p;
uses Shapes;
begin
end.
```

```
prog.luxia:2:6: warning[L0077]: 'Shapes' is used but never named
```

### L0078 — PRAGMA HIDES THAT HIDES NOTHING

A warning: `pragma hides(Unit.Name)` and no declaration of the file
hides that name (§ 11.3).

```pascal
program p;
uses Shapes;
pragma hides(Shapes.Corners);
begin
  writeln(Shapes.Corners);
end.
```

```
prog.luxia:3:21: warning[L0078]: pragma hides: no declaration of the file hides 'Corners'
```

### L0079 — ROUTINE OF C OF THE LIBRARY REACHED

Under the program's `pragma restrictions(no_external)` (or `limba
--restrict=no_external`) the standard library may declare external
routines, but the program may reach none of them (§ 10.4).

```pascal
program p;
uses Net;        // of the library: Open calls a routine of C
pragma restrictions(no_external);
begin
  writeln(Net.Open());
end.
```

```
prog.luxia:5:15: error[L0079]: pragma restrictions(no_external) forbids the boundary with C, which the library crosses here: the routine of C 'Connect', called by Open, reached from the program
```

### L0080 — UNIT OF THE PROGRAM WITH A NAME OF THE LIBRARY

A note, not an error: a unit of the program has the name of a unit of the
standard library. The program and its units reach their own unit; the
library keeps its own (§ 11.4). They are two units, with two names in the
IR, and nothing changes in the meaning of the program. To use the unit of
the library too, the program renames its own.

```pascal
program p;
uses Strings;    // strings.luxia in the program's directory, and in the library
begin
  writeln(Name());
end.
```

```
prog.luxia:2:6: note[L0080]: 'Strings' is a unit of the program; the library has one of the same name, which it keeps for itself
```

### L0081 — AGGREGATE WITHOUT A VALUE

Every field of a record and every index of an array gets a value in an aggregate (§ 6.8); for an array, `else` gives one to the indices not named.

```pascal
program p;
type
  Vector = record x, y, z: Float64; end;
  Row = array[Int32 range 1..4] of Float64;
var v: Vector := {x: 1.0; z: 2.0};
begin
end.
```

```
prog.luxia:5:18: error[L0081]: the aggregate gives no value to the field 'y'
```

### L0082 — GIVEN TWICE

A field or an index gets one value only in an aggregate (§ 6.8).

```pascal
program p;
type
  Vector = record x, y, z: Float64; end;
  Row = array[Int32 range 1..4] of Float64;
var r: Row := {1: 1.0, 1: 2.0, else 0.0};
begin
end.
```

```
prog.luxia:5:24: error[L0082]: the index 1 is given twice
```

### L0083 — AGGREGATE FORM

An aggregate written in a form it does not take (§ 6.8): a record not named field by field, an array positional and by index at once, `else` not last or covering no index, `;` between the elements of an array or `,` between the fields of a record, `else` for an open-array parameter, an aggregate where the context wants neither a record nor an array.

```pascal
program p;
type
  Vector = record x, y, z: Float64; end;
  Row = array[Int32 range 1..4] of Float64;
var r: Row := {1.0, 2.0, 3.0, 4.0, else 0.0};
begin
end.
```

```
prog.luxia:5:36: error[L0083]: else covers no index: every one is given
```

### L0084 — AGGREGATE LENGTH

A positional aggregate for an array with fixed bounds has as many elements as the array (at most as many, with `else`); for an open-array parameter, its elements fit the index from where they start, and `{}` needs an index of two values or more (§ 6.8).

```pascal
program p;
type
  Vector = record x, y, z: Float64; end;
  Row = array[Int32 range 1..4] of Float64;
var r: Row := {1.0, 2.0, 3.0};
begin
end.
```

```
prog.luxia:5:15: error[L0084]: Row has 4 elements, the aggregate 3
```

### L0085 — FIELDS OUT OF ORDER

The fields of a record aggregate follow the order of the declaration of the record (§ 6.8): one form for each value.

```pascal
program p;
type
  Vector = record x, y, z: Float64; end;
  Row = array[Int32 range 1..4] of Float64;
var v: Vector := {y: 1.0; x: 2.0; z: 0.0};
begin
end.
```

```
prog.luxia:5:27: error[L0085]: 'x' comes before 'y' in the record: the fields go in the order of the declaration
```

## All the codes

| Code | Name | Meaning |
|---|---|---|
| L0001 | BAD_UTF8 | bytes that are not UTF-8 |
| L0002 | BAD_CHAR | a character that starts no token |
| L0003 | OPEN_STRING | a string not closed on its line |
| L0004 | OPEN_COMMENT | a `(*` comment never closed |
| L0005 | BAD_CHAR_LITERAL | `''` or a character literal not closed |
| L0006 | KEYWORD_CASE | a keyword not in lowercase |
| L0007 | BAD_NUMBER | a malformed number |
| L0008 | REAL_RANGE | no longer given (a real literal is exact: L0029) |
| L0009 | CONTROL_IN_LITERAL | a control character in a literal |
| L0010 | EXPECTED | a token other than the grammar wants |
| L0011 | MIXED_LOGICAL | `and`, `or`, `xor` mixed without parentheses |
| L0012 | CHAINED | `a < b < c`, `a ** b ** c` |
| L0013 | PREFIX_PARENS | `a * -b`, `2 ** -1`, `not -a` |
| L0014 | PREFIX_POWER | `not a ** b` |
| L0015 | END_NAME | the name after `end` is not the one it closes |
| L0016 | NOT_A_STATEMENT | neither an assignment nor a call |
| L0017 | NESTED_ROUTINE | a routine inside a routine |
| L0018 | EMPTY_STATEMENT | `;;` |
| L0019 | AFTER_END | text after the final `end.` |
| L0020 | END_ALIGN | warning: `end` not under its opening |
| L0021 | UNKNOWN_NAME | a name declared nowhere |
| L0022 | SPELLING | a use spelt differently from the declaration |
| L0023 | DUPLICATE | a name declared twice in a scope, or a name of the language |
| L0024 | BEFORE_DECL | a constant or variable used before its declaration |
| L0025 | NOT_A_TYPE | a value where a type is wanted |
| L0026 | NOT_A_VALUE | a type or routine where a value is wanted |
| L0027 | TYPE_MISMATCH | a value of another type |
| L0028 | OPERATOR_TYPE | an operator on a type it does not take |
| L0029 | CONST_RANGE | a constant out of the range of its type |
| L0030 | NOT_CONSTANT | a value not known at compile time |
| L0031 | CONST_OVERFLOW | a constant past the size limit |
| L0032 | DIV_ZERO | a constant divided by zero |
| L0033 | NEED_TYPE | a constant without a type gives no type |
| L0034 | NOT_ASSIGNABLE | not a variable, or one that cannot change |
| L0035 | ARG_COUNT | too many or too few arguments |
| L0036 | NOT_CALLABLE | a call of something that is not a routine |
| L0037 | RESULT_IGNORED | a function called as a statement |
| L0038 | NO_RESULT | a procedure used as a value |
| L0039 | BAD_CONVERSION | `T(x)` between types that do not convert |
| L0040 | NO_FIELD | `r.x` with no field `x` |
| L0041 | NOT_INDEXABLE | `a[i]` on a value that has no index |
| L0042 | NOT_POINTER | `p^` on a value that is not a pointer |
| L0043 | CASE_DUPLICATE | a value in two branches of a `case` |
| L0044 | CASE_COVERAGE | a `case` without `else` missing values |
| L0045 | OUTSIDE_LOOP | `exit` or `continue` outside a loop |
| L0046 | RETURN_VALUE | `return` with a value in a procedure, without one in a function |
| L0047 | SELF_REFERENCE | defined in terms of itself |
| L0048 | FORMAT_PLACE | `x:w:d` outside `write` and `writeln` |
| L0049 | OPEN_ARRAY_PLACE | an open array outside a parameter |
| L0050 | BAD_RANGE | a range on a type that has none |
| L0051 | DYNAMIC_PLACE | computed bounds outside a variable |
| L0052 | MISSING_RETURN | a path out of a function without `return` |
| L0053 | UNASSIGNED | a variable read before any assignment |
| L0054 | UNSUPPORTED | valid, not yet translated |
| L0055 | HALT_CODE | `halt` with 1, 141 or outside 0..255 |
| L0056 | OUT_UNASSIGNED | an `out` parameter left without a value |
| L0057 | PRAGMA_NAME | a pragma that is not `suppress`, `unsuppress`, `convention` or `restrictions` |
| L0058 | CHECK_NAME | a check that has no such name |
| L0059 | THROUGH_POINTER | a record or an array reached through a pointer as an argument, or a scalar as a `var` one |
| L0060 | C_BOUNDARY | a type that does not cross to C |
| L0061 | RESTRICTED | the boundary with C where `pragma restrictions(no_external)` forbids it |
| L0062 | C_PRAGMA | `pragma convention`, `restrictions` or `hides` written wrong |
| L0063 | DEREF_SELECT | a field or an element after `^` (`p^.x`, `p^[i]`) |
| L0064 | UNIT_NOT_FOUND | a unit used that no file holds |
| L0065 | UNIT_FILE | a file named not as its unit, or a program used as a unit |
| L0066 | UNIT_CYCLE | interfaces that use each other |
| L0067 | UNIT_TWICE | a unit named twice in the uses |
| L0068 | NO_BODY | a routine of the interface without its body |
| L0069 | NOT_CONFORMING | a body whose heading is not the one of the interface |
| L0070 | AMBIGUOUS | a name written directly that two units give |
| L0071 | HIDES | warning: a name that hides one of a unit used |
| L0072 | UNIT_NAME | the name of a unit used declared in the file |
| L0073 | NOT_IN_INTERFACE | `Unit.Name`, Name not in its interface |
| L0074 | READ_ONLY | a variable of an interface written outside its unit |
| L0075 | INIT_RETURN | `return` in the initialisation of a unit |
| L0076 | INIT_ORDER | initialisations that need each other |
| L0077 | UNIT_UNUSED | warning: a unit used and never named |
| L0078 | HIDES_NOTHING | warning: `pragma hides` that hides nothing |
| L0079 | LIBRARY_C | a routine of C of the library reached under `restrictions(no_external)` |
| L0080 | SAME_AS_LIBRARY | note: a unit of the program with the name of one of the library |
| L0081 | AGG_MISSING | a field or an index without a value in an aggregate |
| L0082 | AGG_TWICE | a field or an index given twice in an aggregate |
| L0083 | AGG_FORM | an aggregate in a form it does not take |
| L0084 | AGG_LENGTH | a positional aggregate longer or shorter than its array |
| L0085 | AGG_ORDER | the fields of a record aggregate out of order |
