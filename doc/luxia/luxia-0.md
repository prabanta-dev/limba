# The Luxia Language — Reference Specification (draft)

## 1. Introduction

Luxia is an **extended Pascal**: the syntactic cleanliness of Pascal (and
of its heirs Modula-2 and Oberon) with the rigour of Ada, without Ada's
verbosity. The rigour lies in the semantics — distinct types, checked
ranges, no implicit conversions, parameter modes, run-time checks — and
Luxia keeps it whole; the redundant syntax goes.

"Extended Pascal" describes the idea, not the ISO standard of the same
name (ISO 10206), which Luxia does not follow.

This document describes **Luxia 0**, the minimal core of the language,
together with **units** (§ 11), the first step of Luxia 1; "Luxia 0" in
the text means this whole language. Exceptions with handlers, objects, generics and
threads belong to later versions (Luxia 1 and beyond). Luxia 0 is enough to write the single-threaded
programs of the Computer Language Benchmarks Game: binary-trees,
fannkuch-redux, fasta, k-nucleotide (with a hand-written hash table),
mandelbrot, n-body, pidigits (multiple-precision arithmetic on arrays),
reverse-complement and spectral-norm.

### 1.1 Principles

1. **No implicit conversion** between different types. Literal constants
   have no type until the context gives them one (as in Ada), so `x := 0`
   is valid for every integer type.
2. **Every error that can be found at compile time is found at compile
   time**; the others are **checked** at run time and never silent
   (overflow, indices, ranges, `nil`, division by zero, conversions out of
   range).
3. **An LL(1), context-free grammar**: the parser never consults the
   symbol table.
4. **One form for each thing**: no synonyms, no alternative keywords.
5. **No hidden cost**: what allocates or copies much is visible in the
   text (`new`, `&` on strings).

### 1.2 Notation

Examples are written in Luxia. The grammar (§ 12) uses Wirth's EBNF.
"Compile-time error" means the program is rejected; "run-time error"
means a failed check that stops the program (§ 10).

The source file extension is `.luxia`.

## 2. Lexical elements

### 2.1 Source text

The source is UTF-8. A UTF-8 byte order mark at the start of the file is
skipped; bytes that are not valid UTF-8 are an error. Outside comments,
character literals and string literals, only ASCII is allowed.

The symbols `@ # $ ! ? % | ~ \` are not allowed anywhere outside
comments and literals. The braces `{ }` delimit aggregates (§ 6.8).

### 2.2 Identifiers

An identifier is a letter followed by letters, digits and `_`. In
Luxia 0 identifiers are ASCII only (letters `A-Z a-z`, digits `0-9`,
`_`).

**Case rule.** Identifiers are **case-insensitive for uniqueness**, as in
Pascal: `Account` and `account` cannot both be declared in the same
scope. But **every use must be spelt exactly as the declaration**; a use
with a different spelling is an error. Luxia thus has Pascal's tolerance
and the consistency of a case-sensitive language.

A name predefined by the language (types, constants, routines of the
library, § 9) cannot be declared again.

### 2.3 Keywords

Keywords are reserved, English, and **lowercase only**. By the case rule
above, `BEGIN` is an error, not an identifier.

The 51 keywords are:

```
abs            and            array          begin          case
const          continue       div            do             downto
else           elsif          end            exit           external
false          for            function       if             implementation
in             interface      loop           mod            new
nil            not            of             or             out
pragma         procedure      program        range          record
rem            repeat         return         shl            shr
then           to             true           type           unit
until          uses           var            when           while
xor
```

`true`, `false` and `nil` are keywords (literals), not predefined names.
`abs` is an operator (§ 6), not a library function. `unit`, `uses`,
`interface` and `implementation` are those of Delphi and Free Pascal
(§ 11).

### 2.4 Numbers

```pascal
123    1_000_000    0xFF    0o17    0b1010
1.0    1.5e-9       2e10
```

- `_` may appear only between two digits.
- The base prefixes (`0x`, `0o`, `0b`) and the exponent letter (`e`) are
  lowercase only; hexadecimal digits may be written in either case.
- A real number has digits on both sides of the point: `1.0`, not `1.`
  nor `.5`. Thus `1..10` is always a range.
- A letter attached to a number (`12abc`) is an error.
- A real literal is an exact value of any size, as an integer literal
  is: `1e400 / 1e390` is the constant `1e10` (§ 4.2). Only when it takes
  a type must it fit that type.
- There are no type suffixes: a number takes its type from the context or
  from a conversion (§ 4).

### 2.5 Characters and strings

Characters and strings follow Ada: `'a'` is a `Char`, `"hello"` is a
`String`, and `"a"` is a string of one character.

- There are **no escape sequences**. A double quote inside a string is
  doubled: `"she says ""hello"""`. The apostrophe as a character is
  `'''`.
- A string does not cross the end of a line.
- Character and string literals contain no control characters, tab
  included. Control characters are written with the predefined constants
  of § 2.7 or with `chr(n)`, and joined with `&`: `"line" & LF`.
- Since the source is UTF-8, a string may contain any Unicode character
  written as it is.

### 2.6 Comments

- `//` starts a comment that runs to the end of the line.
- `(* ... *)` is a block comment; block comments nest.

### 2.7 Control-character constants

| Name | Type | Code point |
|---|---|---|
| `NUL` | `Char` | 0 |
| `TAB` | `Char` | 9 |
| `LF` | `Char` | 10 |
| `CR` | `Char` | 13 |

### 2.8 The semicolon

`;` **terminates every statement**, as in Ada, including the last one
before `end`, `else`, `elsif`, `until` and before the next branch of a
`case`. There is no empty statement: `;;` is an error. A branch or a body
with no statements is allowed. A statement can therefore be moved without
adding or removing a `;`.

## 3. Types

### 3.1 Design

The integer and real types of Luxia have their size in their name;
there is no `Integer` nor `Real`. Compared with C:

- there are **no integer promotions**: an operation is computed in the
  type of its operands, and a result that does not fit is an error;
- **both operands always have the same type**; every change of type is a
  written conversion;
- signed and unsigned arithmetic are both **checked**; wrap-around exists
  only in the bit types (§ 3.2);
- bitwise operators and shifts exist **only on the bit types**, and the
  shift count is checked.

The range of values and the representation are separate notions, as in
Ada. In Luxia 0 the representation is that of the base type, and the
range is narrowed with subtypes (§ 3.4).

### 3.2 Integer types

Three families of integers, separated by **intent**:

| Family | Types | Arithmetic | Bitwise and shifts |
|---|---|---|---|
| signed numbers | `Int8 Int16 Int32 Int64` | checked | no |
| unsigned numbers | `UInt8 UInt16 UInt32 UInt64` | checked: `0 - 1` is an error | no |
| bit patterns | `Bits8 Bits16 Bits32 Bits64` | **modular** (wraps, like Ada's modular types) | yes |

- A counter, an index or a quantity is a **number**; a register, a mask, a
  hash, a checksum or a pseudo-random generator is **bits**. The type says
  which of the two a value is. (For readers coming from Ada: Ada's
  `Unsigned_N` is modular; in Luxia the modular types are called `BitsN`
  precisely so as not to confuse them with unsigned numbers.)
- `Byte` is `Bits8` itself (raw data: files, `s[i]` of a string).

### 3.3 Other scalar types

- **`Float32`, `Float64`**: IEEE 754 binary32 and binary64. Never mixed
  with each other nor with integers without a conversion.
- **`Boolean`**: not a number (no arithmetic, no `Boolean(1)`). Values
  `false` and `true`.
- **`Char`**: a Unicode code point; not a number. `ord(c)` gives a
  `UInt32`; `chr(n)` checks that `n` is at most 0x10FFFF and not a
  surrogate.
- **`String`**: a sequence of UTF-8 bytes, **immutable** and
  reference-counted. See § 3.8.
- **`BigInt`**: a signed integer of any size, **immutable** and
  reference-counted. See § 3.12.

### 3.4 Subtypes with a range

```pascal
type Percent = UInt8 range 0..100;
```

A subtype has the representation of its base type and is compatible with
it; its range is checked whenever a value is stored into it. Storing a
value outside the range — by assignment, as an argument, as the result
of `return`, or as a bound of a `for` — is a **range error**; an explicit
conversion `T(x)` whose value is outside `T` is a **conversion error**
(§ 6.6). The base type is always written: a bare range `0..100` would not
say how many bits it takes.

**Empty ranges** are legal, as in Ada: `lo..hi` with `lo > hi` is an
empty range, whether constant or computed. An empty subtype has no
values (every assignment to it is a range error).

### 3.5 Distinct types

```pascal
type Metres = new Float64;
type Id = new UInt32 range 1..1_000_000;
```

A distinct type has the representation of its origin type but is
incompatible with it without a conversion: metres cannot be added to
seconds.

### 3.6 Enumerations

```pascal
type Colour = (Red, Green, Blue);
```

Enumeration values are not numbers. `ord`, `succ` and `pred` apply to
them, checked.

### 3.7 Arrays

```pascal
array[UInt8 range 1..10] of T
array[Colour] of T
```

- The index type is part of the array type. An array has one index;
  several dimensions are written as arrays of arrays
  (`array[I] of array[J] of T`).
- Indices are **always checked** (the compiler removes the checks it can
  prove unnecessary).
- **Computed bounds**: the bounds may be computed at run time, as in
  Ada: `var a: array[Int32 range 1..n] of Float64`. Computed bounds are
  allowed only in the variables of routines and the `var` statements of
  the body of the program, not in the variables declared among the
  declarations of the program or of a unit (interface or
  implementation), nor in types declared with `type`, nor in record
  fields.
- **An array with an empty index** has `length(a) = 0`; `low(a)` and
  `high(a)` remain the written bounds; every access is an index error;
  `for var i := low(a) to high(a)` does not run its body.
- **Bounds in the index type, lengths in `Int64`**: `low(a)` and
  `high(a)` have the base type of the index of `a`; `length(a)` is an
  `Int64`, whatever the index, as the length of a `String` (Ada's
  `'Length` is a universal integer for the same reason). So every array
  over a whole discrete type has a length: `array[Byte] of Byte` has 256
  elements and `length` 256, `array[Colour]` has `length` 3. An array
  type whose length does not fit in an `Int64` (`array[UInt64] of T`,
  `array[Int64] of T`) is a compile-time error: no such array fits in
  memory; with computed bounds, such a length is "out of memory"
  (§ 10.1).
- A whole array value is written with an **aggregate** (§ 6.8):
  `{1, 2, 3}`, `{Red: 1, Green: 2, Blue: 3}`, `{1: 10, else 0}`.

#### 3.7.1 Open arrays

Open arrays correspond to Ada's unconstrained arrays.

```pascal
type Vector = array[Int32 range <>] of Float64;
```

- An open array is written `array[I range <>] of T`, where `I` is a
  discrete type (§ 3.14) and `<>` means "bounds
  to be defined" (Ada's "box").
- In Luxia 0 an open array is **the type of a parameter**, written there
  or named with `type`, or **the type a pointer points to** (§ 3.10,
  § 9.7). A variable, a field or a function result cannot have an
  open-array type (compile-time error).
- **`range` after an open array type gives the bounds of its index**:
  `new(Vector range 1..n)` creates a `Vector` whose index goes from 1 to
  `n` (§ 9.7). After a scalar type the same words constrain a value
  (`Int32 range 1..10`, § 3.4). Two meanings, told apart by the type
  `range` follows: an open array type has no values to constrain, only
  bounds to fix.
- The argument is any array whose elements have the **same type** `T` and
  whose index has the **same base type** as `I`. The parameter **takes the
  bounds of the argument**: inside the routine `low(a)`, `high(a)` and
  `length(a)` are those of the array passed, the bounds in the base type
  of `I`, the length an `Int64`. An
  `array[Int32 range 5..9]` passed to an `array[Int32 range <>]` has
  `low(a) = 5`.
- If `I` is a subtype (`array[Positive range <>]`), the bounds of a
  non-empty argument must belong to `I`: checked at compile time if they
  are known, otherwise at run time at the call (range error). An empty
  argument is always accepted, as in Ada.
- `a[i]` takes `i` of the base type of `I` and is checked against the
  bounds of the argument (index error).
- Modes: without a mode the elements are read-only; with `var` (or `out`)
  the elements can be written, while the bounds remain those of the
  argument. The array is always passed by address: no copy is made.
- In Luxia 0 an open-array parameter is not assigned as a whole
  (`a := b`): the program works on its elements. It is **compared** as a
  whole (`a = b`, § 6.2).
- An open-array parameter can be passed on to another open-array
  parameter, with the same bounds.
- An aggregate passed to an open-array parameter has the bounds given
  in § 6.8.

### 3.8 Strings as open arrays

A `String` behaves as a read-only `array[Int64 range <>] of Byte` that
**always starts at 1** (Luxia 0 has no slices):

- `low(s) = 1`, `high(s) = length(s)`;
- `s[i]`, with `i: Int64`, is a `Byte`, checked to lie in `1..length(s)`;
- `copy(s, from, count)` counts from 1 and returns a new string, which
  starts at 1 again (§ 9.4).

### 3.9 Records

```pascal
record x, y, z: Float64; end
```

The layout of a record (the order and the place of its fields) belongs
to the implementation; with `pragma convention(c, R)` it is that of C
(§ 3.13). A whole record value is written with an aggregate that names
every field, in the order of the declaration: `{x: 1.0; y: 2.0; z:
0.0}` (§ 6.8).

### 3.10 Pointers

- `^T` is a pointer to an object of type `T` created with `new(T)`;
  `nil` is the null pointer.
- Dereferencing is checked (`nil` error).
- Field access dereferences implicitly: `p.x`, never `p^.x`, which is a
  compile-time error (one form for each thing); indexing likewise: `p[i]`
  for a pointer to an array, never `p^[i]`. `p^` alone is the whole
  object, as an argument (`low(p^)`) or in an assignment.
- `^A`, with `A` an open array type (`array[I range <>] of T`), points to
  an array whose bounds are fixed when `new` creates it (`new(A range
  lo..hi)`, § 9.7) and never change, as Ada's access to an unconstrained array.
  `p[i]` is checked against those bounds (index error), after the `nil`
  and the dangling checks; `low(p^)`, `high(p^)` and `length(p^)` give
  them, in the base type of `I`. As for an open-array parameter, `p^` is
  not assigned as a whole, only compared (§ 6.2): the program works on
  its elements, or copies them with `move`, treats them with `translate`,
  `reverse` and `occurrences` (§ 9.5), and reads and writes them with
  `readbytes` and `writebytes` (§ 9.1, § 9.2).
- There is no pointer arithmetic and no way to take the address of a
  variable.
- A pointer to an object that `dispose` has freed is **dangling**.
  Reading or writing through it (`p^`, `p.f`, `p[i]`, also in a chain) is
  checked: it is a run-time error, "dangling pointer" (`dangling_check`,
  § 10.1). The check is made at the access itself, after everything else
  the statement evaluates: in `p.f := g()`, a `dispose(p)` inside `g` is
  caught.
- Comparing pointers (`=`, `<>`) is not an access: a dangling pointer
  compares without error, and it is **never equal** to a pointer to an
  object created later, even one at the same address.
- In Luxia 0 a record or an array reached through a pointer (`p^`, `p.f`,
  `p[i]`, `p.a[i]`, in any chain) is **never an argument**, in any mode: a
  compile-time error. Pass the pointer itself, or copy the object into a
  variable first. A scalar reached through a pointer may be an `in`
  argument (its value) or an `out` one (it goes back through the pointer
  after the call, checked there), not a `var` one (`readline` and `val`
  included). Only `move`, `translate`, `reverse`, `occurrences`,
  `readbytes` and `writebytes` take arrays reached through a pointer:
  they run no code of the program while they work, and check them
  themselves (§ 9.1, § 9.2, § 9.5). Ada allows such arguments, and a deallocation during the call
  makes the execution erroneous; SPARK allows them without a copy through
  the ownership of pointers. This rule will go with that ownership
  (Appendix A).

### 3.11 Objects without an initial value

This follows Ada's treatment of uninitialised objects in its strict form
(`Normalize_Scalars` with validity checks), applied to all objects.

- `new(T)`, `new(A range lo..hi)` and a record or array variable declared
  without an initial
  value (global, local, including one with computed bounds) receive no
  values from the program. Every scalar inside them (in fields and
  elements too) whose subtype is narrower than its base type receives an
  **invalid value**, outside the subtype: `lo - 1`, or `hi + 1` if `lo` is
  the minimum of the base type — even when 0 would be valid. All other
  scalars are 0 (`false`, `nil`), a `String` is `""` and a `BigInt` is
  0.
- **Reading a component** of a narrow subtype (`r.f`, `a[i]`, `p.f`, `p^`,
  `p[i]`, also in a chain, also of a function result) checks its value;
  an invalid value is a range error, reported at the `.`, `^` or `[`.
- A copy (`r := p^`) carries the invalid values with it; they are found
  when the component is read.
- A **comparison** of records or arrays (`r = s`, § 6.2) reads their
  components: an invalid value it reaches is a range error, reported at
  the operator.
- A value read by another route (a scalar `var` parameter bound to a
  component) is not checked in Luxia 0: it keeps the invalid value.
- A scalar variable needs none of this: reading it before assigning it is
  a compile-time error (definite assignment, § 5.5).
- An aggregate gives every component a value (§ 6.8): its scalar
  components are valid values of their subtypes, while a record or
  array component copied whole carries the invalid values it has.

### 3.12 Integers of any size

`BigInt` is a signed integer of any size, as Ada 2022's `Big_Integer`.

- Its arithmetic **never overflows**: `+`, `-`, `*`, `**`, `abs` and the
  unary minus give the exact value. The only limit is memory: a value
  that does not fit in it stops the program with "out of memory"
  (§ 10.1), as a string does.
- It is **immutable and reference-counted**, as a `String`: an
  assignment shares a value, every operation makes a new one, and the
  sharing is never visible. Two `BigInt` values are compared by value,
  never by identity.
- It goes wherever a `String` goes: variables, constants, parameters of
  every mode, results, fields of records, elements of arrays (made by
  `new` too), globals. Without an initial value it is 0 (§ 3.11).
- It is **not a discrete type**: it is not an index, a bound of `for`, a
  selector of `case`, nor an argument of `succ`, `pred`, `ord`, `low` or
  `high`. It is **not a bit type**: no `and`, `or`, `xor`, `not`, `shl`,
  `shr` (`x * 2 ** k` and `x div 2 ** k` are written instead). It is not
  the exponent of `**`, the count of a shift, nor the argument of `chr`:
  those take an integer type of a fixed size.
- A distinct type may be made of it (`type Money = new BigInt`); a
  subtype with a range may not, in Luxia 0.
- Its operations are those of the integers (§ 6.3), its conversions
  those of § 6.6; `write`, `str` and `val` take it as any integer
  (§ 9.1, § 9.4).

### 3.13 Types for the C language

These types exist to call libraries written in C (§ 8.5). They cross the
boundary with a fixed meaning in C; nothing else in a program needs
them.

**C types by name.** Distinct types (§ 3.5), predefined, each with the
representation of a C type on the target platform:

| Luxia | C | x86-64 and aarch64 Linux | x86-64 Windows |
|---|---|---|---|
| `CChar` | `char` | `Int8` (x86-64), `UInt8` (aarch64) | `Int8` |
| `CSChar`, `CUChar` | `signed char`, `unsigned char` | `Int8`, `UInt8` | the same |
| `CShort`, `CUShort` | `short`, `unsigned short` | `Int16`, `UInt16` | the same |
| `CInt`, `CUInt` | `int`, `unsigned int` | `Int32`, `UInt32` | the same |
| `CLong`, `CULong` | `long`, `unsigned long` | `Int64`, `UInt64` | `Int32`, `UInt32` |
| `CLongLong`, `CULongLong` | `long long`, `unsigned long long` | `Int64`, `UInt64` | the same |
| `CSizeT`, `CSSizeT` | `size_t`, `ptrdiff_t` | `UInt64`, `Int64` | the same |
| `CBool` | `_Bool` | one byte, `false` or `true` | the same |
| `CFloat`, `CDouble` | `float`, `double` | `Float32`, `Float64` | the same |

Being distinct, they convert only when written: `CInt(n)`, `Int64(c)`,
checked as any conversion. `CBool` is a Boolean type of its own
(`CBool(b)`, `Boolean(c)`). A program that uses one of them, or a record
with the C convention, is bound to the platform it is compiled for
(`limba --target`, by default the one that compiles).

**The opaque pointer.** `CPointer` is C's `void *`: an address of memory
Luxia does not know. `type Mpz = new CPointer;` makes an opaque pointer
type of its own, incompatible with the others. An opaque pointer can be
assigned, passed and compared with `=` and `<>`, with another of its type
or with `nil` (C's `NULL`), and nothing else: it is never dereferenced,
has no arithmetic, and converts neither to an integer nor to a `^T`. It
has no dangling check: its memory belongs to C.

**Records with the C convention.** `pragma convention(c, R)`, among the
declarations where record type `R` is declared, gives `R` the layout of
C on the target platform: its fields in order, each at a multiple of its
alignment, the size rounded to the largest alignment. Every field must
be a type that crosses the boundary (§ 8.5), or an array of fixed bounds
of one. Without the pragma a record does not cross the boundary: the
layout of Luxia is not promised to be that of C.

**C strings.** `CString` is a predefined `new CPointer`, a `char *`
ended by a byte 0, in the memory of C (§ 9.9).

### 3.14 Discrete types

The **discrete types** are the types whose values can be counted one by
one:

- the integer types of fixed size: `IntN`, `UIntN`, `BitsN` (`Byte`
  included) and the C integer types by name (§ 3.13);
- `Char`, `Boolean` and `CBool`;
- the enumerations;
- the subtypes with a range (§ 3.4) and the distinct types (§ 3.5) of
  all these.

`BigInt` and the reals are not discrete. Only a discrete type can be
the index of an array, the type of a `for` variable, the type of a
`case` selector, the argument of `succ`, `pred`, `low(T)` and `high(T)`,
or be narrowed with `range`. On a `BitsN`, which wraps in its
arithmetic, `succ` and `pred` are still checked (§ 9.3), and a `for`
ends at its last bound without wrapping (§ 7.5).

## 4. Constants and constant expressions

### 4.1 Literals

A literal has no type: it takes one from the context and **must fit** in
it. `var b: UInt8 := 300` is a compile-time error.

- An integer constant takes a real type only if its value is **exactly
  representable** in that type: `var f: Float32 := 1` is allowed,
  `var f: Float32 := 16_777_217` is an error (2^24 + 1 is not exact in
  `Float32`).
- An integer constant of any size takes the type `BigInt`:
  `var g: BigInt := 123_456_789_012_345_678_901_234_567_890` is allowed.
- A real constant never takes an integer type implicitly:
  `var i: Int32 := 1.0` is an error; the conversion is written,
  `Int32(1.0)`.

There is **no default type** for constants: `var x := 0` is an error
("type cannot be inferred from a constant"), while `var x := a + 1` with
`a: Int32` gives `x` the type `Int32`.

### 4.2 Exact evaluation

Constant expressions are computed **exactly**, like Ada's universal
integers and reals, and are checked only when they take a type:

```pascal
const K = 2**40 div 2**20;   // 1_048_576, no intermediate overflow
```

**Real constants are exact rationals**: `0.1 + 0.2 = 0.3` is true between
constants, and rounding to `Float32` or `Float64` happens once, when the
constant takes its type.

A constant expression of type `BigInt` is computed by the compiler like
any other. A compiler may bound the size of the constants it computes
(Limba: 16 384 bits); a larger one is a compile-time error.

The static operations are `+ - * /`, `**` with an integer exponent,
comparisons, `abs`, conversions, and `low(T)`/`high(T)` of a discrete
type (§ 9.5). `sqrt` and the other library functions are not static, as
in Ada.

### 4.3 Declared constants

A constant's value must be known at compile time; a value computed at
run time is declared with `var`. A constant may be declared with or
without a type:

```pascal
const
  N = 1000;                        // a constant without a type
  Pi: Float64 = 3.141592653589793;
  Origin: Vector = {x: 0.0; y: 0.0; z: 0.0};
```

A constant of a record or array type is a **typed constant**, as in
Delphi: it is declared with its type and an aggregate (§ 6.8) whose
components are all known at compile time.

- A typed constant of a record or array type is **not a constant
  expression**: it is not a bound of an array, a label of `case`, an
  argument of a pragma, nor a component of another typed constant
  (`{Origin, Origin}` is an error: the aggregate is written out again,
  or the value is declared with `var`). It is read as a variable that
  cannot be assigned, nor passed as `var` or `out`.
- A typed constant declared in a routine is one for the whole program,
  as in Delphi; it has its value before any code runs.
- A typed constant of a scalar type is a value, usable in constant
  expressions, as any constant.

## 5. Declarations and scope

### 5.1 Sections

```pascal
const
  N = 1000;
  Pi: Float64 = 3.141592653589793;

type
  Vector = record x, y, z: Float64; end;
  Index = Int32 range 1..N;

var
  total: Float64 := 0.0;
  count: Int32 := 0;
  mean := total / 2.0;   // inferred type: Float64
```

- `const`, `type` and `var` sections appear at the head of the program
  and of routines, as in Pascal; sections (and, at the level of the
  file,
  routines) may appear **in any order and may repeat**.
- Routines are declared only at the level of the file, program or unit
  (§ 8.4, § 11).

### 5.2 Declarations as statements

`var` and `const` may also appear **as statements** inside a statement
list; the name is visible from there to the end of that list. A `var` or
`const` statement declares a single group: `var a, b: Int32 := 0;` — with
several names, the initialiser applies to each.

### 5.3 Type inference

A variable declared with an initialiser and no type takes the type of
the initialiser. The type is still static; only its writing is saved.
An aggregate has no type of its own to give (§ 6.8): `var v := {1, 2}`
is a compile-time error.

### 5.4 Visibility

- **Types and routines** are visible throughout their scope, even before
  their declaration. This serves types that refer to each other and
  mutually recursive routines; no `forward` declaration exists:

  ```pascal
  type
    P = ^Node;
    Node = record next: P; end;
  ```

- **Constants and variables** are visible only after their declaration.
- A name declared in an inner scope (a routine, a statement list, the
  body of a `for`) may **hide** a name of an enclosing scope, as in Pascal
  and Ada. Within one scope the case rule of § 2.2 applies. The names of
  the language (§ 9) cannot be declared again anywhere.
- A type or a constant defined in terms of itself is an error. A record
  cannot contain itself except through a pointer.
- The names of the units a file uses, and the names of their
  interfaces, are visible as § 11.3 says.

### 5.5 Definite assignment

Reading a variable that is not assigned on every path to the read is a
compile-time error.

## 6. Expressions

### 6.1 Precedence

Five levels, from the weakest:

| Level | Operators | Rule |
|---|---|---|
| 1 | `and` `or` `xor` | **not mixed without parentheses**: `a and b or c` is an error, as in Ada |
| 2 | `=` `<>` `<` `<=` `>` `>=` `in` | non-associative: `a < b < c` is an error |
| 3 | binary and unary `+` `-`, `&` (concatenation) | left-associative |
| 4 | `*` `/` `div` `mod` `rem` `shl` `shr` | left-associative |
| 5 | `**` `not` `abs` | `**` non-associative |
| — | postfix `.` `[]` `()` `^` | |

Since relations bind tighter than `and`/`or`, `a < b and c < d` needs no
parentheses. `-2**2` is −4, as in mathematics. The exponent of `**` is a
primary: `2 ** -1` is written `2 ** (-1)`, as in Ada.

### 6.2 Operand types

- The two operands of every binary operator and of every comparison have
  the **same type**, and the result has that type. There are no
  promotions and no "wider" type chosen by the compiler. (The exceptions
  are the exponent of `**`, the count of a shift, and `&`, below.)
- `/` applies only to reals; `div`, `mod` and `rem` only to integers.
- Unary minus on a `UIntN` is a compile-time error; on a `BitsN` it is the
  modular two's complement.
- `abs` applies to signed and unsigned integers and to reals, not to
  `BitsN`. `abs` of the minimum of an `IntN` is an overflow.
- `&` concatenates strings and characters in any combination and yields a
  `String`. `+` is only numeric.

**Comparisons.** Both operands have the same type, and the result is a
`Boolean`.

| Operand type | `=` `<>` | `<` `<=` `>` `>=` |
|---|---|---|
| integers, reals (same type) | yes | yes |
| `Char` | yes | yes, by code point |
| enumerations | yes | yes, in declaration order |
| `Boolean` | yes | yes, `false < true` |
| `String` | yes | yes, byte by byte |
| `BigInt` | yes | yes, by value |
| C types by name (`CInt`, `CDouble`, ...) | yes | yes, as their representation (§ 3.13) |
| `CBool` | yes | yes, `false < true` |
| pointers (and `nil`) | yes | no |
| `CPointer` and opaque pointer types (and `nil`) | yes | no |
| records, arrays | yes, component by component (below) | no |

**Records and arrays** are compared whole with `=` and `<>`, as in Ada:

```pascal
if p = q then ...                         // every field
if p = {x: 0.0; y: 0.0; z: 0.0} then ...  // an aggregate, typed by p
function Same(s, t: Samples): Boolean;    // Samples = array[Int32 range <>] of Float64
begin
  return s = t;                           // same length, same elements
end Same;
```

- The operands have the **same type**, as in every comparison. Types are
  told apart by name: two arrays declared in two places with the same
  shape have different types, so two arrays with computed bounds (each
  declaration its own anonymous type) are compared only through an
  open-array parameter, or each with an aggregate; two variables of the
  **same** declaration (`var a, b: array[Int32 range 1..n] of Float64`)
  have the same type.
- **An open operand** (an open-array parameter, or `p^` of a pointer to
  an open array) is compared with every value that could be passed to
  it (§ 3.7.1): elements of the same type, an index of the same base
  type; open operands of another type with the same element and index
  base are included.
- An **aggregate** operand takes the type of the other operand, on
  either side (§ 6.8); between two aggregates there is no type (a
  compile-time error).
- `a = b` is true when the two values have **the same components**:
  records field by field, in the order of the declaration; arrays when
  they have the **same length** and the same elements **in the same
  position** (the first with the first, and so on), whatever their
  bounds: an `array[Int32 range 5..9]` and an `array[Int32 range 1..5]`
  with the same five elements, passed to `s` and `t`, are equal. Arrays
  of different lengths are not equal; two empty arrays are equal.
- Each component is compared with the `=` of its type, at every depth:
  reals with the `=` of reals, so `-0.0 = 0.0` and a NaN is equal to
  nothing; strings and `BigInt` values by value; pointers by address
  (the objects they point to are not compared); C types by their
  representation. The bytes between fields, if any, do not count.
- **A record with a NaN field is not equal to itself**: after
  `var z: Float64 := 0.0; p.x := z / z;`, `p = p` is false and
  `p <> p` is true. This is the result of comparing the fields one by
  one, which the whole comparison must give.
- `a <> b` is `not (a = b)`.
- **Order**: the operands are evaluated first, left to right (§ 6.7);
  then the components are compared in order (fields in the order of the
  declaration, elements from the first) and the comparison **stops at
  the first difference**. Comparing has no effects; the order decides
  only which invalid value (§ 3.11) is found: one reached before a
  difference is a range error, reported at the operator; one after it is
  not read. Ada leaves both the order and the stop unspecified.
- Records and arrays have no order: `<`, `<=`, `>`, `>=` between them
  are a compile-time error.

### 6.3 Integer semantics

- **Every operation on numbers is checked**: `a * b` between `Int16`
  values whose product does not fit in `Int16` is an overflow, even if
  the result would then be divided. A wider intermediate computation is
  written explicitly: `Int32(a) * Int32(b) div c`. The cost is visible
  and is that of the hardware.
- Operations on `BitsN` are modular: they wrap.
- `div` truncates towards zero; `mod` has the sign of the divisor, `rem`
  the sign of the dividend (as in Ada). `low(IntN) div -1` is an
  overflow: a compile-time error when both operands are constants (the
  exact value does not fit), an overflow error at run time otherwise.
  Division by zero is a run-time error.
- `x ** n` with `x` an integer: `n` may be of any integer type of fixed
  size (not `BigInt`) and must be
  **non-negative**; a negative `n` at run time is a range error (as in
  Ada, where the exponent is a `Natural`). The result must fit in the type
  of `x` (overflow); in a `BitsN` it wraps.
- `x ** n` with `x` a real: `n` is an integer of any type of fixed size
  and any sign.
  The result is the IEEE 754 `pow` computed in `Float64` and rounded once
  to the type of `x`; a negative exponent gives the reciprocal
  (`0.0 ** -1` is `inf`). There is no run-time error.
- On **`BigInt`** (§ 3.12) nothing overflows; `div`, `mod` and `rem`
  follow the rules above, and a division by zero is a run-time error.
  `x ** n` takes `n` of any integer type of fixed size (not `BigInt`),
  **non-negative** (a range error
  otherwise, as above); `x ** 0` is 1, `0 ** 0` included. For the bases
  0, 1 and −1 the result is exact for every `n`, however large (0, 1 or
  ±1); for any other base a result too large for the memory is "out of
  memory".
- **Shifts**: `x shl n` and `x shr n` take `x` of a `BitsN` type only and
  `n` of any integer type of fixed size (not `BigInt`). `shr` is a
  **logical** shift: zeros are shifted
  in. A count below zero, or equal to or above the width of `x`, is a
  shift error at run time.

### 6.4 Logical operators

- `and` and `or` between `Boolean` values **always short-circuit**; there
  is no `and then` / `or else`. Between `BitsN` values they are bitwise.
- `xor` and `not` apply to `Boolean` and `BitsN` values.

### 6.5 Membership

`x in a..b` tests whether `x` lies in the range; `x in T` tests whether
`x` belongs to the range of the subtype `T`, as in Ada.

### 6.6 Conversions

A conversion is written as a call of the target type: `Float64(i)`,
`Int32(x)`, `Metres(d)`.

- **Real to integer**: `Int32(x)` **rounds half away from zero** (as in
  Ada) and is checked (conversion error if the result does not fit).
  `trunc(x)`, `round(x)` (half to even), `floor(x)` and `ceil(x)` make
  the other choices explicit; they return a real of the same type, and
  the integer is then obtained with an exact conversion:
  `Int32(trunc(x))`.
- **Integer to real**: explicit, rounded according to IEEE 754.
  `Float64` of an `Int64` beyond 2^53 loses digits: the conversion is written, so the loss is asked for.
- **Between `UIntN` and `BitsN` of the same width**: explicit, and free
  (same representation).
- **Integer to integer**: the value must fit the target type, its range
  included; otherwise it is a **conversion error**.
- **Integer to `BitsN`** keeps the low bits of the value, in two's
  complement: `BitsN(x)` from a wider type truncates, and a negative value
  wraps. It is the only conversion that cuts bits, and it is written.
- **Integer to enumeration**: `Colour(n)`, from any integer type of
  fixed size, is the value at position `n` from 0, the inverse of `ord`
  (Ada's `'Val`): `Colour(ord(c)) = c`. A position outside the
  enumeration, or outside the range of a subtype of it, is a
  **conversion error**, a compile-time error if `n` is a constant. It
  applies to enumerations declared by the program; `Char` has `chr`
  (§ 9.3), and `Boolean` is not converted from an integer (`n = 1`
  says what is meant).
- **Integer to `BigInt`**: `BigInt(i)` from any integer type is exact (a
  `BitsN` value as an unsigned number). **`BigInt` to integer**:
  `Int32(b)`, `UInt64(b)` and the like follow the rule of integer to
  integer (the value must fit, its range included, or it is a conversion
  error); `BitsN(b)` keeps the low bits, as above.
- **Real to `BigInt`**: `BigInt(x)` rounds as `Int32(x)` does, **half away
  from zero** (`BigInt(2.5)` is 3, `BigInt(-2.5)` is −3); a NaN or an
  infinity is a conversion error. Every finite real that is an integer is
  a `BigInt` exactly.
- **`BigInt` to real**: `Float64(b)` and `Float32(b)` round **once,
  directly to the target type**, to the nearest value, ties to even (IEEE
  754 roundTiesToEven); `Float32(b)` does not pass through `Float64`. A
  value whose rounding exceeds the largest finite value of the type (from
  that value plus half a unit in the last place upwards) gives an
  infinity with the sign of `b`.

### 6.7 Order of evaluation

The operands of an operator and the arguments of a call are evaluated
**from left to right**, then the operation is applied; if two parts could
stop the program, the first one does. `writeln(a, b)` prints `a` before
computing `b`. `and` and `or` between `Boolean` values do not evaluate
the second operand when the first one decides.

In the assignment `a[i] := e`, the **target is evaluated first** (with the
index check), then the value, then the range check of the target.

The components of an aggregate are evaluated from left to right as
written, `else` last (§ 6.8).

### 6.8 Aggregates

```pascal
const
  Origin: Vector = {x: 0.0; y: 0.0; z: 0.0};
  Months: array[Int32 range 1..3] of String = {"Jan", "Feb", "Mar"};
var
  grey: array[Colour] of UInt8 := {Red: 128, Green: 128, Blue: 128};
  row: array[Int32 range 1..4] of Float64 := {1: 1.0, else 0.0};
begin
  v := {x: v.y; y: v.x; z: 0.0};       // swaps x and y
  Draw({x: 0.0; y: 0.0; z: 1.0});
```

An **aggregate** writes a whole record or array value between braces.

- **The type comes from the context**, as in Ada: the declared type of a
  constant or variable, the target of an assignment, a parameter without
  a mode (`var` and `out` want a variable), the result of a function
  (`return`), a component of an enclosing aggregate, the other operand of
  `=` or `<>` (§ 6.2). An aggregate
  anywhere else has no type: a compile-time error. There is no
  conversion of an aggregate (`Vector({...})`): every context where it
  is allowed already gives the type.
- **A record aggregate names every field, in the order of the
  declaration**, separated by `;`: `{x: 1.0; y: 2.0; z: 0.0}`. There is
  no positional form for records: reordering the fields of a record
  would silently change it.
- **An array aggregate is positional** (`{1, 2, 3}`) **or by index**
  (`{Red: 1, Green: 2, Blue: 3}`, also with ranges: `{1..3: 0.0, 4:
  1.0}`), separated by `,`; the two forms do not mix. The indices and
  the bounds of the ranges are **constant expressions**; the values may
  be computed. `else` followed by a value, last, gives that value to
  the indices not named, also after the positional form (`{1, 2, else
  0}`), as `else` does in `case` (§ 7.3); an `else` that covers no index
  is an error. `{}` is an array with no elements.
- A positional element, an index and a bound are `Simple` expressions
  (§ 12), with the grammar of the labels of `case`: a comparison, a
  membership test (`in`) or an expression with `and`, `or` or `xor` as
  an element is written in parentheses (`{(a < b), true}`); `not a` is
  a `Simple` and needs none (`{not a, b}`).
- **Complete**: every field, every index exactly once. A field missing,
  repeated or not in the record, an index missing (without `else`),
  repeated or outside the index type are compile-time errors. The
  positional form for an array with fixed bounds has exactly `length`
  elements (at most, with `else`).
- Every component is checked as in an assignment (§ 7.1): the same
  type, within the range of its subtype; a constant component out of
  range is a compile-time error, a computed one a range error at run
  time.
- **An aggregate is a value**: it is computed whole, then assigned.
  `v := {x: v.y; y: v.x; z: 0.0}` swaps `x` and `y`, and `a := {a[2],
  a[1]}` swaps the two elements, whatever the components read or the
  routines they call do.
- **Order**: the components are evaluated from left to right as
  written, `else` last (once); the first that stops the program does
  (§ 6.7). In an assignment the target is evaluated first, then the
  aggregate, then the value is copied.
- **Computed bounds** (`var a: array[Int32 range 1..n] of Int32 := {1, 2,
  else 0}`): the positional form without `else` has `length(a)`
  elements, and with `else` at most that many; otherwise it is a range
  error at run time. The form by index without `else` names exactly the
  indices from `low(a)` to `high(a)` (range error otherwise); with
  `else`, an index named outside the bounds is an index error.
- **An open-array parameter** (`array[I range <>] of T`, § 3.7.1)
  receives an aggregate with these bounds:
  - by index, the lowest and the highest index named, without gaps;
  - positional, `n` elements from `s`, where `s` is 0 if 0 belongs to
    `I` (the common integers, as Delphi's open arrays), `low(I)`
    otherwise (an enumeration, `Char`, `Boolean`, a subtype without 0).
    `Sum({1, 2, 3})` passes an array from 0 to 2. If the last index
    would pass `high(I)` it is a compile-time error;
  - `{}`: no elements from `s`, that is `s..pred(s)`, or `succ(s)..s`
    where `pred(s)` does not exist in the base type of `I`; if that base
    type has a single value (an enumeration of one value), it is a
    compile-time error;
  - `else` is an error in both forms: there are no bounds to fill.
- `p^ := {...}` assigns a record or an array with fixed bounds; for a
  pointer to an open array it is an error, as every whole assignment of
  `p^` (§ 3.10).
- An aggregate may be an operand of `=` and `<>`, typed by the other
  operand (§ 6.2): `if v = {x: 0.0; y: 0.0; z: 0.0} then`.

## 7. Statements

Every structured statement closes with `end` (as in Modula-2): there is
no `begin` inside statements and no dangling `else`.

```pascal
x := expression;
if c then ...; elsif d then ...; else ...; end;
case k of
  when 1, 2:    ...;
  when 3..9:    ...;
  else          ...;
end;                              // without else it must cover every value
while c do ...; end;
repeat ...; until c;
for var i := 1 to n do ...; end;  // i declared by the loop, constant in the body
for var i := n downto 1 do ...; end;
for var i: Int32 := 1 to 10 do ...; end;  // literal bounds: the type is written
loop ...; exit when c; ...; end;
exit; continue when c; return expression;
P(a, b);                          // call: always with parentheses, even empty
```

### 7.1 Assignment

`designator := expression;`. The value must have the type of the target
(§ 6.2) and lie in its range. An aggregate is computed whole before the
target receives it (§ 6.8).

### 7.2 `if`

`if`, any number of `elsif` branches, an optional `else`, then `end`.

### 7.3 `case`

- The selector is a discrete value with a type (§ 3.14); a constant
  without a type is not a selector.
- Branches are introduced by `when`, with a list of labels, each a value
  or a range (`when 3..9:`). Labels are constants, known at compile time,
  and must fit the type of the selector.
- A value may not appear in two branches, singly or inside a range
  (compile-time error).
- A `case` **without** `else` must cover every value of the selector's
  type, from its first to its last value (for a subtype, its range);
  a missing value is a compile-time error. With `else`, the values not in
  any branch go to `else`.
- There is no fall-through into the next branch.

### 7.4 `while`, `repeat`, `loop`

- `while c do ... end` tests before each iteration.
- `repeat ... until c` tests after each iteration.
- `loop ... end` repeats until an `exit` (or a `return`).

### 7.5 `for`

`for var i := a to b do ... end` (or `downto`) declares `i`; `i` is
visible only in the body and is **constant** there; it does not exist
after the loop.

- The type of `i` comes from the bounds. If both bounds are constants
  without a type, the type is written: `for var i: Int32 := 1 to 10`.
- The bounds are evaluated once, on entry.
- `i` is compared with the last bound **before** each step, so it never
  goes beyond it: `for var i: Int32 := 1 to high(Int32)` runs its body
  for every value up to `high(Int32)` and ends without an overflow (as
  in Ada); the same with `downto` and the first value of the type.
- There is no step; a loop with a step is written with `while`.

### 7.6 `exit` and `continue`

`exit` leaves the innermost loop; `continue` goes to the next iteration
(in `repeat` it goes to the `until` test). `exit when c` and
`continue when c` are the conditional forms.

### 7.7 Calls

Calls **always have parentheses**: `P()`, not `P`. This removes Pascal's
ambiguity between a variable and a call and keeps the grammar
context-free.

**The result of a function cannot be ignored**, as in Ada: a function
called as a statement is an error. A designator that is not a call is not
a statement.

### 7.8 Declarations and pragmas as statements

`var` and `const` statements are described in § 5.2; `pragma` statements
in § 10.3.

## 8. Routines

```pascal
function Distance(a, b: Vector): Float64;
begin
  return sqrt((a.x - b.x)**2 + (a.y - b.y)**2 + (a.z - b.z)**2);
end Distance;

procedure Advance(var bodies: array[Int32 range <>] of Body; dt: Float64);
var i: Int64;
begin
  ...
end;
```

- A routine without parameters is declared with empty parentheses too:
  `procedure P();`, symmetrical with the call `P()`.
- The name after the `end` of a routine is optional and, if present,
  checked. The compiler warns when an `end` is not aligned with its
  opening.

### 8.1 Parameter modes

| Mode | Written | Meaning |
|---|---|---|
| in | no keyword | constant inside the routine; passed by copy or by reference at the compiler's choice, as in Ada; the argument may be an aggregate (§ 6.8) |
| in out | `var` | read and written, by reference |
| out | `out` | must be assigned before the routine returns |

- A scalar `out` parameter is passed **by copy on return** (copy-out), as
  in Ada: the routine works on its own copy, which goes back to the
  argument at each `return` and at the end, not before. Reading it before
  assigning it, or leaving the routine on a path that did not assign it,
  is a compile-time error.
- A record or array `out` parameter is passed by address and is not
  checked component by component, neither at compile time nor at run
  time, as in Ada: the parameter is the caller's object, so a component
  the routine does not assign keeps the value it had before the call. If
  it had none, reading it is caught by the validity check on components
  without a value (§ 3.11).

### 8.2 Results

- A function returns its result with `return expression`, not by
  assigning to its name.
- A function must return on every path (checked at compile time).

### 8.3 Order of declaration

A routine can be used before its definition in the same file, with no
`forward` declaration.

### 8.4 Not in Luxia 0

No overloading, no nested routines, no default parameters, no named
association of arguments.

### 8.5 External routines

A routine may be a function of a library written in C (or C++ through
`extern "C"`), as Pascal's `external` and Ada's `pragma Import`:

```pascal
type Mpz = new CPointer;

procedure MulUi(r: Mpz; a: Mpz; b: CULong);
  external "gmp" name "__gmpz_mul_ui";

function StrLen(s: CString): CSizeT;
  external "c" name "strlen";

function sqrtf(x: CFloat): CFloat;
  external "m";              // the symbol is the name as written: sqrtf
```

- In place of its body, `external "library" [name "symbol"];`. Without
  `name`, the symbol is the name of the routine **as written** (C tells
  the case apart). `name` here is not a keyword.
- The library is a **logical name**: `"gmp"`, `"m"`; `"c"` is the C
  library of the process. The engine turns it into the file of the
  platform (`libgmp.so`, `gmp.dll`, `libgmp.dylib`) and looks for it
  where it is told to, by its command line or its configuration: never
  a path in the program. It opens every library and finds **every**
  symbol the program **can call** **before** the program starts; if one
  is missing it stops at once with a message, never halfway through. An
  external routine that no call can reach (a routine of a unit the
  program does not call) asks for nothing.
- An external routine is called as any other; its address cannot be
  taken in Luxia 0.
- **What crosses the boundary**, as parameters and as the result:
  - the integers of fixed size (`Int8`..`Int64`, `UInt8`..`UInt64`,
    `Bits8`..`Bits64`, C's `int8_t`..`uint64_t`), `Float32` (`float`),
    `Float64` (`double`);
  - the C types by name, the opaque pointers and the records with the C
    convention (§ 3.13); a record passes by value as C passes it, or by
    address as a `var` or `out` parameter;
  - as a parameter only: an array, of fixed bounds or open, of such
    elements, passed as a pointer to its first element (`const T *` in
    mode `in`, `T *` in modes `var` and `out`); its length the program
    passes apart;
  - a `var` or `out` parameter of such a type, passed as its address
    (`T *`).
- **What does not**, a compile-time error: `String` and `BigInt`
  (counted: C knows no references; § 9.9 converts strings), `Boolean`
  (C's `_Bool` is `CBool`), `Char`, enumerations, the pointers `^T` of
  Luxia, subtypes with a range (C would not respect the range of what
  it returns: a result converts afterwards), records without the C
  convention. C's `long double` has no type in Luxia: a function that
  takes or returns one cannot be declared (on x86-64 it passes through
  the x87 stack, on Win64 it is a `double`: a wrong translation would be
  silent).
- The arguments are computed and checked **before** the call, from the
  left, as for any routine; the result is converted and checked after
  it. What happens inside the call is the subject of § 10.4.

## 9. The predefined library

The names of the library are visible everywhere and cannot be redeclared
in Luxia 0.

### 9.1 Output

- `write(...)` and `writeln(...)` take any number of arguments; each is a
  number, a `Boolean`, a `Char` or a `String`. `writeln` ends the line.
  Enumerations and pointers cannot be written (an enumeration is written
  through `ord`).
- Integers are written in decimal, a `BigInt` with all its digits; a
  `BitsN` value is written as an unsigned number. A `Boolean` is written `true` or `false`. A constant
  without a type is written as an `Int64` or a `Float64`; one that does
  not fit is a compile-time error (`writeln(2**100)`: write
  `BigInt(2)**100`).
- `x:width` and `x:width:decimals` format an argument as in Pascal. They
  are allowed only in the arguments of `write` and `writeln`, and the
  decimals only on reals (a compile-time error otherwise).
- `x:width` **right-aligns** the value in a field of `width` characters,
  padding with spaces on the left. The width counts characters (code
  points), not bytes, and never truncates: a longer value is written
  whole.
- The width and the decimals are `Int32` values; the width goes from 0
  upwards and the decimals from 0 to 100 (like Ada's `Field`, without its
  upper limit). A value outside these limits is a range error where it is
  written, and a compile-time error if it is a constant. The parts are
  evaluated from the left: the value, then the width (checked), then the
  decimals (checked).
- `x:width:decimals` writes a real in fixed notation with `decimals`
  digits after the point, rounded from the exact value of `x` (for a
  `Float32`, the exact value of the `Float32`).
- A real without a format is written in the **shortest form that reads
  back exactly as a value of its own type**, as Python's `repr`: `0.1`,
  `100.0` (an integral value keeps `.0`), `1e+16`, `1.5e-05`, `-0.0`. For
  a `Float32` it is the shortest form that reads back as that `Float32`:
  `0.1`, `0.33333334`, `3.4028235e+38`. Infinities are written `inf` and
  `-inf`; a NaN is always `nan` (IEEE 754 does not fix its sign).
- `writebyte(b)` writes one byte, for binary output.
- `writebytes(a, from, count)` writes the span `from .. from + count -
  1` of `a`, byte by byte as it is, for binary output in blocks. `a` is
  an array of `Byte` or a `String` (only read; a `String` is indexed from
  1 by `Int64`, § 3.8). `from`, `count` and their checks follow the rules
  of `move` for a span (§ 9.5), checked before anything is written.
- Everything is written on one stream, the standard output, in the order
  of the calls: `write`, `writeln`, `writebyte` and `writebytes` may be
  mixed. The output may be buffered: an error in writing it is found
  when the buffer is written, perhaps after the call (§ 10.1).

### 9.2 Input

`readline(out s: String): Boolean` reads one line and returns `false` at
the end of the file. A line ends with `LF` or `CR LF`, which are removed.
At the end of the file `s` becomes `""`: `s` is an `out` parameter and
always has a value after the call.

`readbytes(a, from, count)` reads up to `count` bytes of the input into
the span `from .. from + count - 1` of `a` and returns how many it read,
an `Int64`. It reads fewer than
`count` only at the end of the input (0 at the end), as C's `fread`, the
same on a file, a pipe or a terminal: a program never repeats a short
read. The bytes come as they are, `CR LF` included; the elements of the
span past those read keep their values. `a` is a writable array whose
elements are `Byte` exactly (a narrower element type is a compile-time
error, so no value out of range enters `a`). `from`, `count` and their
checks follow the rules of `move` for a span (§ 9.5), checked before
anything is read.

- `readline` and `readbytes` read one stream, the standard input, in the
  order of the calls: a line read after a block starts where the block
  ended (after a block that ends between `CR` and `LF`, the line is
  empty).
- **The end of the input is final.** Once `readline` has returned
  `false`, or `readbytes` fewer bytes than asked, every later `readline`
  returns `false` and every later `readbytes` 0, even if a terminal
  gives more.
- An error in reading, which is not the end, is a run-time error
  (§ 10.1).

### 9.3 Characters

- `LF`, `CR`, `TAB`, `NUL` (§ 2.7).
- `chr(n)`: the `Char` with code point `n`, with `n` of any integer type
  of fixed size, checked (§ 3.3).
- `ord(x)`: for a `Char`, its code point; for a `Boolean`, 0 or 1; for an
  enumeration value, its position from 0. The result is a `UInt32`. An
  integer argument is a compile-time error: an integer is converted
  instead, `UInt32(x)`.
- `succ(x)`, `pred(x)`: the next and previous value of a discrete type
  (§ 3.14), checked: `succ` of the last value and `pred` of the first
  are range errors, on a `BitsN` too (they do not wrap).

### 9.4 Strings

- `length(s)`: the length in bytes, an `Int64`.
- `s[i]`: the byte at position `i`, a `Byte`, checked (§ 3.8).
- `copy(s, from, count)`: a new string with `count` bytes of `s` starting
  at position `from` (counted from 1); `from` and `count` are `Int64`.
  The arguments are evaluated from left to right; `from < 1` or
  `count < 0` is a **range error**; past the end the result is truncated, as in Pascal: `copy("abc", 3, 5) = "c"`,
  `copy("abc", 5, 1) = ""`.
- `str(x)`: `x` as a string, written as `write` writes it without a format
  (§ 9.1).
- `val(s, var x): Boolean`: reads the number written in `s` into the
  numeric variable `x`. The rules:
  - `s` must follow **the syntax of a Luxia literal** (§ 2.4): `_` only
    between two digits, lowercase `0x`, `0o`, `0b`, a real with digits on
    both sides of the point and a lowercase `e`;
  - a sign `+` or `-` may precede the number, and spaces and `TAB` around
    it are ignored, as in Ada;
  - an integer is stored if it fits in the type of `x`, ranges included
    (`"-0"` fits in an unsigned type; a `BigInt` holds any integer);
  - a real (or an integer, in a real variable) is rounded **once** to the
    type of `x`, as a literal is; a text beyond the range of the type is
    not valid (`"1e400"`);
  - `inf`, `-inf`, `+inf` and `nan` (without a sign) are accepted: they
    are the forms `str` writes, so that `val(str(x), y)` gives `y` the
    value of `x` when `y` has the type of `x`;
  - a text that is not a number **of the type of `x`** makes `val` return
    `false` and leaves `x` unchanged. As in Ada, wrong syntax and a value
    out of range get the same answer; but external data never stops the
    program.

### 9.5 Arrays and discrete types

- `low(a)`, `high(a)` of an array or a string, in the base type of the
  index; `length(a)`, an `Int64` (§ 3.7, § 3.8); `low(p^)`, `high(p^)`, `length(p^)`
  of an array created by `new` (§ 3.10).
- `move(src, from, dst, to, count)` copies `count` elements of the array
  `src`, from index `from`, into the array `dst`, from index `to`, as
  Ada's slice assignment `dst(to .. to + count - 1) := src(from ..
  from + count - 1)`. The two arrays have elements of the same type and
  indices of the same base type `I`; `from` and `to` are values of `I`
  and `count` an `Int64`, computed from the left. `count < 0` is a range error; `count =
  0` copies nothing and checks no bound; otherwise both ranges must lie
  within the bounds of their array (index error), checked before
  anything is copied. The ranges may overlap: the result is as if the
  elements were first copied aside. `dst` must be writable (a variable, a
  `var` or `out` parameter, an array reached through a pointer); an
  element without a value carries its invalid value (§ 3.11). `src` and
  `dst` may be reached through a pointer: `move` checks `nil` and a
  dangling pointer itself, at the copy.
- `translate(a, from, count, table)`, `reverse(a, from, count)` and
  `occurrences(a, from, count, pattern)` work on the span `from .. from
  + count - 1` of the array `a`, with the rules of `move`: `from` is a
  value of the base type `I` of the index of `a` and `count` an `Int64`,
  computed from the left after `a`; `count < 0` is a range error; `count = 0`
  does nothing and checks no bound; otherwise the span must lie within
  the bounds of `a` (index error), checked before anything is read or
  written. An array reached through a pointer is checked for `nil` and a
  dangling pointer by the routine itself, as by `move`.
  - `translate` replaces each element `x` of the span by `table[x]`, as
    Python's `bytes.translate`. `a` is a writable array of `Byte`;
    `table` is an array indexed by `Byte` whose elements are `Byte`
    (`array[Byte] of Byte`), so that it has a value for every byte: a
    narrower index or element type is a compile-time error. `table` is
    read whole before `a` changes, so `a` and `table` may be the same
    array.
  - `reverse` reverses the order of the elements of the span, in place,
    as Python's `bytearray.reverse`. `a` is a writable array of any
    element type; Strings, BigInts and pointers move with their elements
    (no value is made or lost), and an element without a value carries
    its invalid value (§ 3.11).
  - `occurrences` is the number of non-overlapping occurrences of
    `pattern` in the span, searched from the left, as Python's
    `bytes.count` and Ada's `Ada.Strings.Fixed.Count`: an `Int64`,
    never above `count`. `a` is an array of `Byte` or a `String` (only
    read; a `String` is indexed from 1 by `Int64`, § 3.8); `pattern` is a
    `String` or an array of `Byte`, taken whole, evaluated after
    `count`. An empty pattern is a range error, whatever `count` is, as
    Ada's `Pattern_Error` (Python answers `count + 1`).

  The names `translate`, `reverse` and `occurrences` are names of the
  language: like every other one, they cannot be declared again (§ 5.4).
- `low(T)`, `high(T)` of a discrete type `T` (§ 3.14): the first and the
  last
  value of `T`, a constant of type `T` (as Ada's `T'First` and `T'Last`).
  `low(Int8)` is −128, `high(Boolean)` is `true`. A real type is a
  compile-time error.

### 9.6 Mathematics

`sqrt`, `sin`, `cos`, `tan`, `arctan`, `exp`, `ln`, `trunc`, `round`,
`floor`, `ceil` take a `Float32` or a `Float64` and return a real of the
same type; a constant without a type needs a conversion
(`sqrt(Float64(2))`). They follow IEEE 754 and raise no run-time error:
`sqrt(-1.0)` and `ln(-1.0)` are `nan`, `ln(0.0)` is `-inf`, as for `**`.
`trunc`, `round` (half to even), `floor` and `ceil` keep a real (§ 6.6).

### 9.7 Memory

- `new(T)` creates an object of type `T` and returns a `^T`. `new(T)` is
  syntax, not a function, since `new` is a keyword. The contents of the
  new object follow § 3.11.
- `new(A range lo..hi)`, with `A` an open array type `array[I range <>]
  of T`, creates an array of `T` whose index goes from `lo` to `hi`, and
  returns a `^A`. **Here `range` gives the bounds of the index, not a
  constraint on a value**: after an open array type it fixes where the
  index of the new array goes; after a scalar type (`new(Int32 range
  1..10)`, `var x: Int32 range 1..10`) it constrains the value, as
  everywhere else (§ 3.4). The same words have two meanings, told apart
  by the type they follow (§ 3.7.1):

  ```pascal
  type Vector = array[Int64 range <>] of Float64;
  var v: ^Vector := new(Vector range 0..n - 1);  // n elements, 0 to n - 1
  var k := new(Int32 range 1..10);               // one Int32, from 1 to 10
  ```

  `lo` and `hi` are values of the base type of `I`, computed in this
  order; `hi < lo` gives an empty array. If `I` is a subtype, the bounds
  of a non-empty array must belong to it (range error). If the memory
  for it cannot be had, the program stops with "out of memory"
  (§ 10.1), also when its length would not fit in an `Int64` or its size
  in bytes in the address space (§ 3.7): never a silent overflow. The elements follow § 3.11.
  `new(A)` without a range, for an open array type, is a compile-time
  error.
- `dispose(p)` frees the object `p` points to (a whole array created by
  `new(A range lo..hi)`); `dispose(nil)` does nothing. Disposing of an
  object already disposed is a run-time error, "invalid dispose", which
  is not a check: it cannot be suppressed.

### 9.8 Environment

- `argcount()`: the number of command-line arguments, an `Int32`.
- `arg(i)`: the `i`-th argument, a `String`; `i` is an `Int32`, from 1 to
  `argcount()`. Outside that range it is a range error, reported at the
  name; a constant `i` below 1 is a compile-time error (as Ada's
  `Argument`).
- `halt(code)`: stops the program with `code`, an `Int32`, as its exit
  status. The code is 0 or 2..255 except 141, because **1 is reserved
  for run-time errors** and **141 for a closed output** (§ 10.1). A
  constant `halt(1)`, `halt(141)` (or a constant outside those limits)
  is a compile-time error; a computed value outside those limits is a
  range error at run time.

### 9.9 C strings

As Ada's `Interfaces.C.Strings`, with explicit conversions:

- `newcstring(s: String): CString`: a copy of `s` with a byte 0 after it,
  allocated with the `malloc` of C, so that a C function that takes it
  over may free it. A `s` that holds a byte 0 is a **range error** (C
  would see a string cut short, silently); if `malloc` fails, "out of
  memory".
- `cvalue(p: CString): String`: the bytes up to the first 0, copied into
  a `String`; `nil` is a nil error. `cvalue(p, n)` copies exactly `n`
  bytes (`n` an `Int64` from 0; a negative one is a range error), a 0
  among them included.
- `cvalue(a, n)`, `a` an array of `CChar`, `CUChar` or `Byte` that C
  filled as a buffer: its first `n` elements as a `String`, `n` an
  `Int64`, as a count of `move` (§ 9.5), checked against the length of
  `a`: below 0 it is a range error, as for `move`, past the length an
  index error.
- `freecstring(var p: CString)`: frees `p` with the `free` of C and sets
  it to `nil`; on `nil` it does nothing.

## 10. Run-time errors and checks

### 10.1 Run-time errors

The run-time checks are: overflow, index, range, `nil`, division by zero,
conversion out of range, shift count, and dangling pointer. A failed check stops the
program with a message on the standard error that says which check
failed and where:

```
luxia: index out of range at prog.luxia:12:5
```

and with **exit status 1**, the same for every error. Luxia 0 has no
handlers: every run-time error stops the program.

When the program cannot get the memory it asks for (`new`, a string, a
`BigInt`, a computed array), it stops in the same way with the message "out of
memory" and exit status 1 (as Ada's `Storage_Error`). How much memory a
program may use depends on the implementation, and so does how much of
it the local variables of calls take (an optimiser may put a routine's
body in its caller); going past it is always this error, never a
crash. A computation whose result is not used may be left out, and with
it the memory it would take; a check is never left out. This is not a check: it cannot be
suppressed.

The same holds for calls nested too deeply, recursion without end for
example: the program stops with "stack overflow" (Ada's `Storage_Error`
too). How deep calls may go depends on the implementation; going past it
is always this error, never a crash. A second `dispose` of the same
object stops the program with "invalid dispose" (§ 9.7), which is not a
check either.

An error in reading the standard input or in writing the standard output
(a device error, a full disk; the end of the input is no error) stops the
program in the same way with "input/output error" and exit status 1, as
Ada's `Device_Error`: it is never ignored, and it is not a check either.
A read error is reported where the read is. The output may be buffered,
so a write error may be found after the write that caused it, at a later
output or at the end of the program: it is **always reported without a
place**:

```
luxia: input/output error
```

When the reader of the output has gone (a pipe whose reader ended, as in
`prog | head`), the program stops where it finds it, without a message,
with **exit status 141**, the status a Unix shell shows for a program
ended by a closed pipe: nothing it writes can be read any more, and the
status says why it stopped.

As in Ada, a program sees no error codes. Internally each check has a
code, listed here for reference:

| Code | Message | Check name (§ 10.3) |
|---|---|---|
| 6 | overflow | `overflow_check` |
| 7 | out of memory | — |
| 11 | division by zero | `division_check` |
| 28 | stack overflow | — |
| 100 | index out of range | `index_check` |
| 101 | value out of range | `range_check` |
| 102 | nil dereferenced | `nil_check` |
| 103 | conversion out of range | `conversion_check` |
| 104 | shift count out of range | `shift_check` |
| 105 | dangling pointer | `dangling_check` |
| 106 | invalid dispose | — |
| 107 | input/output error | — |

`range_check` includes the validity checks on components without a
value (§ 3.11).

### 10.2 Checks are on by default

Every check is active by default. Before any suppression, the compiler
removes by itself the checks it can prove unnecessary (for example the
index check in `for var i := low(a) to high(a) do ... a[i]`), without
losing safety.

### 10.3 Suppressing checks

Checks can be turned off where needed, as with Ada's `pragma Suppress` and
the `{$R-}`/`{$Q-}` directives of Delphi and Free Pascal.

```pascal
pragma suppress(index_check, overflow_check);
pragma unsuppress(index_check);
```

- `pragma suppress(...)` turns the named checks off; `pragma
  unsuppress(...)` turns them back on.
- Check names: `index_check`, `range_check`, `overflow_check`,
  `division_check`, `conversion_check`, `shift_check`, `nil_check`,
  `dangling_check`, and `all_checks` for all of them.
- An unknown pragma name and an unknown check name are compile-time
  errors.
- **Scope**: among the declarations of a file (the program or a unit,
  both its parts), a pragma applies to the whole file, wherever it
  stands among them, and not beyond it (§ 11.6); several pragmas there
  apply in the order written, so for one check the last one prevails
  (`suppress(index_check)` then `unsuppress(index_check)` leaves it
  on). Among the declarations of a routine, the same for the whole
  routine. As a statement, a pragma is **positional**: it applies from
  where it stands to the end of its statement list (the body of a loop,
  a branch of an `if`). An inner pragma prevails over an outer one.
- **Compiler option**: `limba --suppress=index_check,...` (or
  `all_checks`) turns checks off in every file of the program, with a
  warning. It
  serves measurement and final builds and does not replace the pragma.

**Semantics, as in Ada**: turning a check off does not make the program
correct. If the condition that the check would have detected occurs, the
**behaviour is undefined** (in Ada the program is *erroneous*). The
compiler may still keep a suppressed check (Ada allows this too).

Compile-time checks cannot be turned off (definite assignment, missing
`return` and `out` values, constants out of range, types): they cost
nothing at run time.

### 10.4 The unchecked boundary

A call of an external routine (§ 8.5) leaves the language:

- **inside the call no guarantee of Luxia holds**: no run-time error is
  raised, the memory it takes is not counted (§ 10.1), no pointer is
  checked; a mistake in C may stop the process without a message of
  Luxia;
- an address given to C (a `var` or `out` parameter, an array, a record
  by address) is valid **only during the call**: a C function that keeps
  it, or frees memory of Luxia, makes the program erroneous, as in Ada;
- the declaration is **not compared** with the library: a wrong
  signature gives wrong data or a crash, as in Ada;
- on either side of the call the rules of Luxia hold as everywhere.

**Forbidding it.** `pragma restrictions(no_external)`, among the
declarations of the program, makes every external routine, every C type
by name, `CPointer`, `CString` and `pragma convention` a compile-time
error in the program and in its units; the option `limba
--restrict=no_external` does the same without touching the source, as
Ada's `Restrictions` for programs to be certified. Among the
declarations of a unit, the pragma applies to that unit. The **standard
library** may declare external routines (it is its boundary with C),
but a program under the restriction may **reach none of them**: a call
that leads to an external routine of the library is a compile-time
error that shows the chain of calls. The routines of the language
(§ 9) are not external. A program that compiles so has no unchecked
boundary.

## 11. Program structure and units

A program is one file, the **program**, and the **units** it uses,
directly or through other units, each in a file of its own. A unit has
an interface, the part other files see, and an implementation, the part
they do not, as in Delphi and Free Pascal; the rules of Ada make the
connection between the two, and between files, free of silent choices.

### 11.1 The program

```pascal
program NBody;
uses Vectors;
const ...
type ...
var ...
function ...
begin
  ...
end.
```

- A program starts with `program Name;`, may continue with a `uses`
  clause (§ 11.3), continues with declarations (§ 5) and routines
  (§ 8) in any order, and ends with the body `begin ... end.`
- The name after the final `end` is optional and, if present, checked.
- Before its body runs, the units are initialised (§ 11.5).
- The program ends when its body ends, or at a `return;` in the body
  (without a value), with exit status 0; or at `halt` (§ 9.8); or at a
  run-time error (§ 10).

Example, the heart of n-body:

```pascal
procedure Advance(var b: array[Int32 range <>] of Body; dt: Float64);
begin
  for var i := low(b) to high(b) do
    for var j := i + 1 to high(b) do
      var dx := b[i].x - b[j].x;
      var dy := b[i].y - b[j].y;
      var dz := b[i].z - b[j].z;
      var d2 := dx*dx + dy*dy + dz*dz;
      var mag := dt / (d2 * sqrt(d2));
      b[i].vx := b[i].vx - dx * b[j].mass * mag;
      ...
    end;
  end;
end;
```

### 11.2 Units

```pascal
unit Geometry;

interface

uses Text;

type
  Vector = record x, y, z: Float64; end;

const
  Tolerance = 1.0e-9;

var
  calls: Int64 := 0;

function Distance(a, b: Vector): Float64;

implementation

function Square(x: Float64): Float64;
begin
  return x * x;
end;

function Distance(a, b: Vector): Float64;
begin
  calls := calls + 1;
  return sqrt(Square(a.x - b.x) + Square(a.y - b.y) + Square(a.z - b.z));
end Distance;

begin
  ...
end Geometry.
```

- A file holds **one unit**: `unit Name;`, the part `interface`, the
  part `implementation`, an optional `begin ... end` (its
  initialisation, § 11.5), and `end [Name].` The name after the final
  `end` is optional and, if present, checked.
- **The interface** declares what other files see: types, constants and
  variables in full, routines by their heading only. An external
  routine (§ 8.5) stands in the interface whole, with no body: the unit
  is then the boundary with the C library, and a file that uses it does
  not see C.
- **The implementation** holds the bodies of the routines of the
  interface and everything that stays private, with the rules of a
  program: sections and routines in any order (§ 5, § 8). What the
  interface declares is visible in the implementation without being
  repeated.
- Every routine of the interface has its body in the implementation;
  a missing body is a compile-time error. The heading of the body
  **conforms** to that of the interface, as in Ada:
  - **textually for names**: the same kind (`procedure` or
    `function`), the same name of the routine and of the parameters,
    spelt alike, in the same order, with the same modes (none, `var`,
    `out`); grouping does not matter (`a, b: Vector` and
    `a: Vector; b: Vector` conform);
  - **semantically for types**: each parameter and the result have the
    same type however written (`Vector` and `Geometry.Vector` are the
    same type); a subtype with a range has the same bounds
    (`Int32 range 1..N` and `Int32 range 1..10` conform if `N` is 10);
    two distinct types (`new`) of the same structure do not.
- **What the interface shows is nameable outside.** A declaration of
  the interface uses only types of the interface itself, of the units
  in the `uses` of the interface, and of the language. A private type
  in a parameter, a result, the type of a variable or constant, or a
  field of a record of the interface is a compile-time error.
- A record of the interface shows all its fields, an enumeration all
  its values. (Private types, with hidden fields, are planned with
  objects: Appendix A.)
- A unit may name its own declarations qualified, `Geometry.calls`
  inside `Geometry`, as in Delphi.

### 11.3 `uses` and visibility

```pascal
program Orbits;

uses Geometry, Text;

var a, b: Vector;            // or Geometry.Vector
begin
  ...
  writeln(Distance(a, b), " ", Geometry.calls);
end.
```

- One clause `uses A, B;` at most stands after `program Name;`, after
  `interface` and after `implementation`, as in Delphi. The `uses` of the implementation serve the
  bodies only and are not seen by a file that uses the unit. Naming a
  unit twice, in one clause or in the two parts, is a compile-time
  error.
- A name of the interface of a used unit is written **directly**
  (`Distance`) or **qualified** (`Geometry.Distance`), as one prefers,
  also in types.
- **Ambiguity.** If a name written directly comes from two used units
  of the same level (below), that use is a compile-time error, which
  asks for the qualified form. Using two units that have names in
  common is not an error; only the ambiguous use is. The order of the
  `uses` never decides.
- **Levels.** Names come from three levels: the declarations of the
  file; the units of the program (§ 11.4); the units of the standard
  library. A name of a level hides the same name of the levels below:
  - a declaration of the file that hides a name of a used unit gets a
    **warning** on the declaration (`'Sort' hides 'Strings.Sort'`),
    unless the file states it with `pragma hides(Strings.Sort);`,
    among the declarations of the file, anywhere. The pragma covers that
    name of that unit only; a pragma that hides nothing is itself a
    warning. The hidden name stays reachable qualified;
  - a direct use of a name of a unit of the program that hides a name of
    a unit of the library gets a **warning** on the use, which the
    qualified form removes.
  A new name in a unit, or a new unit in the standard library, thus
  never changes the meaning of a program and never stops it from
  compiling: at most it adds a warning.
- **A qualifier that names a used unit is always the unit.** If a used
  unit exports a name equal to another used unit (a variable `Text`
  while the file uses the unit `Text`), `Text.X` is `X` of the unit
  `Text`, and the variable is reached as `Strings.Text`. Written alone,
  `Text` is the variable: a unit is not a value.
- **The name of a used unit cannot be declared in the file** at any
  level (global, parameter, local, `for` variable, value of an
  enumeration); only record fields, always reached after a dot, may
  have it. Otherwise `Text.X` in that file could mean something other
  than the unit. For the same reason a unit cannot declare its own
  name. A unit cannot have the name of a name of the language (§ 9).
- The case rule (§ 2.2) holds across files: a name is spelt as declared,
  and a unit is named in `uses` as its file declares it.
- **Variables of an interface are read-only outside their unit**: they
  are read, but not assigned, not passed as `var` or `out`, and neither
  are their fields and elements. Only the unit changes them, with its
  routines. (Delphi and Ada let them be written; Luxia does not, so that
  a reader of the unit knows who writes its variables.)
- `uses` is **not transitive**: to name a type of `Text`, a file uses
  `Text`. A value of that type received from `Geometry` is used without
  naming the type.
- **Cycles.** Two units may use each other if at least one of the two
  `uses` is in an implementation. A cycle made only of `uses` of
  interfaces is a compile-time error: an interface must be understood
  without the units that use it. Two types that refer to each other are
  declared in the same unit.
- A unit used and never named gets a warning.

### 11.4 Where units are found

- A unit `Geometry` is in the file `geometry.luxia`: the name of the
  unit in **lowercase**. A file whose unit has another name is a
  compile-time error. Two units whose names differ only in case cannot
  exist.
- Units belong to two **spaces**: the units of the **program**, found in
  the directory of the program and then in the directories given to the
  compiler, in order; and the units of the **standard library**,
  supplied with the implementation, in a place of their own.
- A `uses` of the program or of one of its units looks in the space of
  the program first, then in the library; a `uses` of a unit of the
  library looks **only** in the library. The program never redirects
  the library, and the library never sees the units of the program. In
  each space a name gives one file for the whole program; a unit of the
  program with the name of a unit of the library is a different unit,
  which the program reaches by that name (the compiler adds a note).
- The program contains **no paths**: where the files are is told by
  whoever compiles, by the command line or the configuration, as for C
  libraries (§ 8.5).
- A unit not found is a compile-time error on the `uses`.
- A run-time error in a unit names the file of the unit:
  `luxia: index out of range at geometry.luxia:12:5`.

### 11.5 Initialisation

- The final `begin ... end` of a unit, optional, is its
  **initialisation**. The variables of the unit take their initial
  values first, in the order they are written; then the `begin ... end`
  runs. In it the rules of the body of a program hold, except that
  `return;` is a compile-time error; `halt` ends the program, and a
  run-time error stops it as anywhere.
- Every unit is initialised **once, before the body of the program**.
  The order is computed by the compiler from what the initialisations
  can do, without pragmas:
  - `U` is initialised **after** `V` when `V` is in the `uses` of the
    interface of `U`, or when the initialisation of `U` (the initial
    values of its variables and its `begin ... end`), following calls
    through all units, **can read or write a variable that the
    initialisation of `V` writes**;
  - a variable whose initial value is a constant of a scalar type or a
    `String`, or an aggregate whose components are all such constants
    (nested aggregates included), and that the initialisation of its
    unit does not write, wholly or in part, has its value before any
    code runs and orders nothing; so has a typed constant (§ 4.3);
  - among units left free by these rules, the order of the `uses`,
    depth first, as written (interface, then implementation);
  - if the constraints form a circle, it is a compile-time error that
    shows the chain of reads and calls; the code is moved, there is no
    pragma to force an order.
- The analysis follows every call to its target (Luxia has no pointers
  to routines). It works on the text: a read inside a branch that never
  runs counts all the same, and the message says so.
- Without cycles of `uses`, every unit is initialised after the units it
  uses, as in Delphi.
- There is no finalisation: the memory of the program is returned when
  the process ends.

### 11.6 Pragmas and units

- `pragma suppress` and `unsuppress` (§ 10.3) apply in the file where
  they stand; their scope does not cross files.
- `pragma restrictions` (§ 10.4) in the file of the program applies to
  the whole program; in a unit, to that unit.
- `pragma convention(c, R)` stands with the record; a record of an
  interface carries its convention with it.

## 12. Grammar

Wirth's notation: `=` defines, `.` ends a rule, `|` separates
alternatives, `[ ]` is optional, `{ }` repeats zero or more times, `( )`
groups; terminals are quoted; token classes are lowercase (`ident`,
`integer`, `real`, `char`, `string`). The grammar is LL(1).

```ebnf
Source     = Program | Unit .
Program    = "program" ident ";" [ Uses ] { Decl }
             "begin" Stmts "end" [ ident ] "." .
Unit       = "unit" ident ";"
             "interface" [ Uses ] { IntfDecl }
             "implementation" [ Uses ] { Decl }
             [ "begin" Stmts ] "end" [ ident ] "." .
Uses       = "uses" ident { "," ident } ";" .
Decl       = ConstSec | TypeSec | VarSec | Routine | Pragma ";" .
IntfDecl   = ConstSec | TypeSec | VarSec | Heading | Pragma ";" .
Heading    = ( "procedure" ident Params | "function" ident Params ":" Type )
             ";" [ "external" string [ ident string ] ";" ] .
Pragma     = "pragma" ident "(" QualIdent { "," QualIdent } ")" .
QualIdent  = ident [ "." ident ] .
ConstSec   = "const" ConstDecl ";" { ConstDecl ";" } .
ConstDecl  = ident [ ":" Type ] "=" Expr .
TypeSec    = "type" TypeDecl ";" { TypeDecl ";" } .
TypeDecl   = ident "=" Type .
VarSec     = "var" VarDecl ";" { VarDecl ";" } .
VarDecl    = IdentList ( ":" Type [ ":=" Expr ] | ":=" Expr ) .
IdentList  = ident { "," ident } .
Routine    = ( "procedure" ident Params | "function" ident Params ":" Type )
             ";" ( { LocalDecl } "begin" Stmts "end" [ ident ] ";"
                 | "external" string [ ident string ] ";" ) .
LocalDecl  = ConstSec | TypeSec | VarSec | Pragma ";" .
Params     = "(" [ Param { ";" Param } ] ")" .
Param      = [ "var" | "out" ] IdentList ":" Type .
Type       = QualIdent [ "range" ( Range | "<>" ) ]
           | "new" Type
           | "(" IdentList ")"
           | "array" "[" Type "]" "of" Type
           | "record" { IdentList ":" Type ";" } "end"
           | "^" Type .
Range      = Simple ".." Simple .
Stmts      = { Stmt ";" } .
Stmt       = ( Designator [ ":=" Expr ]
             | "if" Expr "then" Stmts { "elsif" Expr "then" Stmts }
               [ "else" Stmts ] "end"
             | "case" Expr "of" { "when" Label { "," Label } ":" Stmts }
               [ "else" Stmts ] "end"
             | "while" Expr "do" Stmts "end"
             | "repeat" Stmts "until" Expr
             | "for" "var" ident [ ":" Type ] ":=" Expr ( "to" | "downto" ) Expr
               "do" Stmts "end"
             | "loop" Stmts "end"
             | "exit" [ "when" Expr ]
             | "continue" [ "when" Expr ]
             | "return" [ Expr ]
             | "var" VarDecl
             | "const" ConstDecl
             | Pragma ) .
Label      = Simple [ ".." Simple ] .
Expr       = Relation [ "and" Relation { "and" Relation }
                      | "or" Relation { "or" Relation }
                      | "xor" Relation { "xor" Relation } ] .
Relation   = Simple [ RelOp Simple | "in" Simple [ ".." Simple ] ] .
RelOp      = "=" | "<>" | "<" | "<=" | ">" | ">=" .
Simple     = [ "+" | "-" ] Term { ( "+" | "-" | "&" ) Term } .
Term       = Factor { ( "*" | "/" | "div" | "mod" | "rem" | "shl" | "shr" ) Factor } .
Factor     = Primary [ "**" Primary ] | "not" Primary | "abs" Primary .
Primary    = integer | real | char | string | "true" | "false" | "nil"
           | Designator | "(" Expr ")" | "new" "(" Type ")" | Aggregate .
Aggregate  = "{" [ Component { ( "," | ";" ) Component } ] "}" .
Component  = "else" Expr | Simple [ [ ".." Simple ] ":" Expr ] .
Designator = ident { "." ident | "[" Expr "]" | "^" | "(" [ Arg { "," Arg } ] ")" } .
Arg        = Expr [ ":" Expr [ ":" Expr ] ] .
```

The strictness rules of expressions are in the grammar itself: `and`,
`or` and `xor` do not mix (a single branch of `Expr`); a comparison does
not chain (`Relation` has at most one operator); `**` does not chain and
its exponent is a `Primary`.

The semantic analysis rejects some constructs the grammar lets through,
to give better messages: a function call used as a statement, a
designator that is not a call used as a statement, `x:w:d` outside
`write`/`writeln`, a routine inside another, and a field or an index
after `^` (`p^.x`, `p^[i]`: § 3.10). In an external
routine, the `ident` before the second `string` must be `name`, a
contextual word with that meaning only there, not a keyword.

A qualified name `Geometry.Distance` is a `Designator` in expressions
and a `QualIdent` in types and pragmas; the semantic analysis tells a
unit from a record (§ 11.3). In the interface of a unit a routine is a
`Heading`: its body, if it is not external, follows in the
implementation as a `Routine`.

An `Aggregate` is a record or an array as the type of its context says
(§ 6.8); the semantic analysis checks the separators (`;` between the
fields of a record, `,` between the elements of an array) and that the
indices are constant.

A branch of `case` begins with `when`, and a conditional `exit` or
`continue` is written `exit when c`; since every statement ends with `;`,
the two uses of `when` do not conflict.

## Appendix A. Planned for later versions (informative)

The following are not part of Luxia 0; they are listed so that programs
and tools can anticipate them.

- Exceptions and handlers (`raise`, `try ... except`), grouping the
  run-time checks as Ada's `Constraint_Error` does.
- Named association of arguments (`F(x => 1)`), optional.
- An order (`<`) on arrays of discrete elements, as in Ada.
- Sets; the brackets of Pascal (`[a, b]`) remain free for them.
- Assignment of whole open arrays, with a length check.
- Iteration over the Unicode characters of a string.
- Redefinition of the names of the predefined library.
- Types for hardware: bit layout of records, byte order, alignment,
  variables at fixed addresses, volatile access, sizes imposed on
  subtypes.
- Subtypes of `BigInt` with a range; functions of `BigInt` beyond its
  operators (greatest common divisor, square root, exact division).
- Routines of Luxia given to C as pointers to functions (callbacks),
  and variadic C functions (`printf`); a tool that writes the external
  declarations from the headers of a C library, so that the signature is
  no longer written by hand.
- Ownership of pointers, making `dispose` safe at compile time, and
  lifting the rule of § 3.10 on arguments reached through a pointer.
- Private types in interfaces, with objects; finalisation of units if
  objects need it.
- Generics. Their relation with units is already fixed: a generic of an
  interface is declared there by its heading and has its body in the
  implementation, conforming as a routine (§ 11.2); the names of its
  body are resolved where it is written, never where it is
  instantiated; its body is checked once, at its definition, against
  the contract of its parameters, and each instance against the same
  contract, so that a correct instance always compiles; a variable of
  the unit used by a generic routine is the one of the unit, shared by
  every instance; the order of initialisation (§ 11.5) follows the
  actual routines given to each instance.
