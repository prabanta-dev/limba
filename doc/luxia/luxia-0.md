# The Luxia 0 Language — Reference Specification (draft)

## 1. Introduction

Luxia is an **extended Pascal**: the syntactic cleanliness of Pascal (and
of its heirs Modula-2 and Oberon) with the rigour of Ada, without Ada's
verbosity. The rigour lies in the semantics — distinct types, checked
ranges, no implicit conversions, parameter modes, run-time checks — and
Luxia keeps it whole; the redundant syntax goes.

"Extended Pascal" describes the idea, not the ISO standard of the same
name (ISO 10206), which Luxia does not follow.

Luxia 0 is the **minimal core** of the language. Modules, exceptions with
handlers, objects, generics and threads belong to later versions
(Luxia 1 and beyond). Luxia 0 is enough to write the single-threaded
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

The braces `{ }` and the symbols `@ # $ ! ? % | ~ \` are not allowed
anywhere outside comments and literals.

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

The 49 keywords are:

```
abs       and       array     begin     case      const     continue
div       do        downto    else      elsif     end       exit
export    false     for       function  if        import    in
loop      mod       module    new       nil       not       of
or        out       pragma    procedure program   range     record
rem       repeat    return    shl       shr       then      to
true      type      until     var       when      while     xor
```

`true`, `false` and `nil` are keywords (literals), not predefined names.
`abs` is an operator (§ 6), not a library function. `module`, `import`
and `export` are reserved for later versions and have no use in Luxia 0.

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
- A real literal that does not fit in `Float64` is an error.
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

Luxia has only types whose size is in their name; there is no `Integer`
nor `Real`. Compared with C:

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
  allowed only in the variables of routines and of the program body, not
  in types declared with `type` nor in record fields.
- **An array with an empty index** has `length(a) = 0`; `low(a)` and
  `high(a)` remain the written bounds; every access is an index error;
  `for var i := low(a) to high(a)` does not run its body.
- **Lengths in the index type**, as Ada's `'Length`: `low(a)`, `high(a)`
  and `length(a)` have the base type of the index of `a`. With an `Int32`
  index everything stays 32-bit. If the length does not fit in the base
  type of the index (an array indexed by `Int8 range -128..127` has 256
  elements) it is a compile-time error, or a run-time error if the
  bounds are computed.

#### 3.7.1 Open arrays

Open arrays correspond to Ada's unconstrained arrays.

```pascal
type Vector = array[Int32 range <>] of Float64;
```

- An open array is written `array[I range <>] of T`, where `I` is a
  discrete type (integer, enumeration or subtype) and `<>` means "bounds
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
  `length(a)` are those of the array passed, in the base type of `I`. An
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
- In Luxia 0 an open-array parameter is neither assigned nor read as a
  whole (`a := b`): the program works on its elements.
- An open-array parameter can be passed on to another open-array
  parameter, with the same bounds.

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

Records have the C layout.

### 3.10 Pointers

- `^T` is a pointer to an object of type `T` created with `new(T)`;
  `nil` is the null pointer.
- Dereferencing is checked (`nil` error).
- Field access dereferences implicitly: `p.x` rather than `p^.x`; indexing
  likewise: `p[i]` for a pointer to an array.
- `^A`, with `A` an open array type (`array[I range <>] of T`), points to
  an array whose bounds are fixed when `new` creates it (`new(A range
  lo..hi)`, § 9.7) and never change, as Ada's access to an unconstrained array.
  `p[i]` is checked against those bounds (index error), after the `nil`
  and the dangling checks; `low(p^)`, `high(p^)` and `length(p^)` give
  them, in the base type of `I`. As for an open-array parameter, `p^` is
  neither assigned nor read as a whole: the program works on its
  elements, or copies them with `move` (§ 9.5).
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
  included). Only `move` takes arrays reached through a pointer: it runs
  no code of the program while it copies, and checks them itself
  (§ 9.5). Ada allows such arguments, and a deallocation during the call
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
  scalars are 0 (`false`, `nil`), and a `String` is `""`.
- **Reading a component** of a narrow subtype (`r.f`, `a[i]`, `p.f`, `p^`,
  `p[i]`, also in a chain, also of a function result) checks its value;
  an invalid value is a range error, reported at the `.`, `^` or `[`.
- A copy (`r := p^`) carries the invalid values with it; they are found
  when the component is read.
- A value read by another route (a scalar `var` parameter bound to a
  component) is not checked in Luxia 0: it keeps the invalid value.
- A scalar variable needs none of this: reading it before assigning it is
  a compile-time error (definite assignment, § 5.5).

## 4. Constants and constant expressions

### 4.1 Literals

A literal has no type: it takes one from the context and **must fit** in
it. `var b: UInt8 := 300` is a compile-time error.

- An integer constant takes a real type only if its value is **exactly
  representable** in that type: `var f: Float32 := 1` is allowed,
  `var f: Float32 := 16_777_217` is an error (2^24 + 1 is not exact in
  `Float32`).
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
```

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
  and of routines, as in Pascal; sections (and, at program level,
  routines) may appear **in any order and may repeat**.
- Routines are declared only at program level (§ 8.4).

### 5.2 Declarations as statements

`var` and `const` may also appear **as statements** inside a statement
list; the name is visible from there to the end of that list. A `var` or
`const` statement declares a single group: `var a, b: Int32 := 0;` — with
several names, the initialiser applies to each.

### 5.3 Type inference

A variable declared with an initialiser and no type takes the type of
the initialiser. The type is still static; only its writing is saved.

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
| pointers (and `nil`) | yes | no |

Records and arrays are not compared as a whole.

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
- `x ** n` with `x` an integer: `n` may be of any integer type and must be
  **non-negative**; a negative `n` at run time is a range error (as in
  Ada, where the exponent is a `Natural`). The result must fit in the type
  of `x` (overflow); in a `BitsN` it wraps.
- `x ** n` with `x` a real: `n` is an integer of any type and any sign.
  The result is the IEEE 754 `pow` computed in `Float64` and rounded once
  to the type of `x`; a negative exponent gives the reciprocal
  (`0.0 ** -1` is `inf`). There is no run-time error.
- **Shifts**: `x shl n` and `x shr n` take `x` of a `BitsN` type only and
  `n` of any integer type. `shr` is a **logical** shift: zeros are shifted
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
  `Float64` of an `Int64` beyond 2^53 loses digits, as the writer asked.
- **Between `UIntN` and `BitsN` of the same width**: explicit, and free
  (same representation).
- **Integer to integer**: the value must fit the target type, its range
  included; otherwise it is a **conversion error**.
- **Integer to `BitsN`** keeps the low bits of the value, in two's
  complement: `BitsN(x)` from a wider type truncates, and a negative value
  wraps. It is the only conversion that cuts bits, and it is written.

### 6.7 Order of evaluation

The operands of an operator and the arguments of a call are evaluated
**from left to right**, then the operation is applied; if two parts could
stop the program, the first one does. `writeln(a, b)` prints `a` before
computing `b`. `and` and `or` between `Boolean` values do not evaluate
the second operand when the first one decides.

In the assignment `a[i] := e`, the **target is evaluated first** (with the
index check), then the value, then the range check of the target.

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
(§ 6.2) and lie in its range.

### 7.2 `if`

`if`, any number of `elsif` branches, an optional `else`, then `end`.

### 7.3 `case`

- The selector is a discrete value with a type (an integer, an
  enumeration, `Char`, `Boolean`, or a subtype of one); a constant without
  a type is not a selector.
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
| in | no keyword | constant inside the routine; passed by copy or by reference at the compiler's choice, as in Ada |
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

## 9. The predefined library

The names of the library are visible everywhere and cannot be redeclared
in Luxia 0.

### 9.1 Output

- `write(...)` and `writeln(...)` take any number of arguments; each is a
  number, a `Boolean`, a `Char` or a `String`. `writeln` ends the line.
  Enumerations and pointers cannot be written (an enumeration is written
  through `ord`).
- Integers are written in decimal; a `BitsN` value is written as an
  unsigned number. A `Boolean` is written `true` or `false`. A constant
  without a type is written as an `Int64` or a `Float64`.
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

### 9.2 Input

`readline(var s: String): Boolean` reads one line and returns `false` at
the end of the file. A line ends with `LF` or `CR LF`, which are removed.
At the end of the file `s` becomes `""`: `s` always has a value, like an
`out` parameter.

### 9.3 Characters

- `LF`, `CR`, `TAB`, `NUL` (§ 2.7).
- `chr(n)`: the `Char` with code point `n`, checked (§ 3.3).
- `ord(x)`: for a `Char`, its code point; for a `Boolean`, 0 or 1; for an
  enumeration value, its position from 0. The result is a `UInt32`. An
  integer argument is a compile-time error: an integer is converted
  instead, `UInt32(x)`.
- `succ(x)`, `pred(x)`: the next and previous value of a discrete type,
  checked.

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
    (`"-0"` fits in an unsigned type);
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

- `low(a)`, `high(a)`, `length(a)` of an array or a string, in the base
  type of the index (§ 3.7, § 3.8); `low(p^)`, `high(p^)`, `length(p^)`
  of an array created by `new` (§ 3.10).
- `move(src, from, dst, to, count)` copies `count` elements of the array
  `src`, from index `from`, into the array `dst`, from index `to`, as
  Ada's slice assignment `dst(to .. to + count - 1) := src(from ..
  from + count - 1)`. The two arrays have elements of the same type and
  indices of the same base type `I`; `from`, `to` and `count` are values
  of `I`, computed from the left. `count < 0` is a range error; `count =
  0` copies nothing and checks no bound; otherwise both ranges must lie
  within the bounds of their array (index error), checked before
  anything is copied. The ranges may overlap: the result is as if the
  elements were first copied aside. `dst` must be writable (a variable, a
  `var` or `out` parameter, an array reached through a pointer); an
  element without a value carries its invalid value (§ 3.11). `src` and
  `dst` may be reached through a pointer: `move` checks `nil` and a
  dangling pointer itself, at the copy.
- `low(T)`, `high(T)` of a discrete type `T` (an integer type, a subtype
  with a range, an enumeration, `Char`, `Boolean`): the first and the last
  value of `T`, a constant of type `T` (as Ada's `T'First` and `T'Last`).
  `low(Int8)` is −128, `high(Boolean)` is `true`. A real type is a
  compile-time error.

### 9.6 Mathematics

`sqrt`, `sin`, `cos`, `tan`, `arctan`, `exp`, `ln`, `trunc`, `round`,
`floor`, `ceil`. `trunc`, `round` (half to even), `floor` and `ceil`
return a real of the type of their argument (§ 6.6).

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
  of a non-empty array must belong to it (range error); if the length
  does not fit in the base type of `I`, it is a range error (§ 3.7). If
  the memory for it cannot be had, the program stops with "out of
  memory" (§ 10.1), also when its size in bytes would not fit in the
  address space: never a silent overflow. The elements follow § 3.11.
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
  status. The code is 0 or 2..255, because **1 is reserved for run-time
  errors**. A constant `halt(1)` (or a constant outside those limits) is a
  compile-time error; a computed value outside those limits is a
  range error at run time.

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
computed array), it stops in the same way with the message "out of
memory" and exit status 1 (as Ada's `Storage_Error`). How much memory a
program may use depends on the implementation, and so does how much of
it the local variables of calls take (an optimiser may put a routine's
body in its caller); going past it is always this error, never a
crash. This is not a check: it cannot be
suppressed.

The same holds for calls nested too deeply, recursion without end for
example: the program stops with "stack overflow" (Ada's `Storage_Error`
too). How deep calls may go depends on the implementation; going past it
is always this error, never a crash. A second `dispose` of the same
object stops the program with "invalid dispose" (§ 9.7), which is not a
check either.

As in Ada, a program sees no error codes. Internally each check has a
code, listed here for reference:

| Code | Message | Check name (§ 10.3) |
|---|---|---|
| 6 | overflow | `overflow_check` |
| 7 | out of memory | — |
| 28 | stack overflow | — |
| 11 | division by zero | `division_check` |
| 100 | index out of range | `index_check` |
| 101 | value out of range | `range_check` |
| 102 | nil dereferenced | `nil_check` |
| 103 | conversion out of range | `conversion_check` |
| 104 | shift count out of range | `shift_check` |
| 105 | dangling pointer | `dangling_check` |
| 106 | invalid dispose | — |

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
- **Scope**: among the declarations of the program, a pragma applies to
  the whole file; among the declarations of a routine, to the whole
  routine; as a statement, from where it stands to the end of its
  statement list (the body of a loop, a branch of an `if`). An inner
  pragma prevails over an outer one.
- **Compiler option**: `limba --suppress=index_check,...` (or
  `all_checks`) turns checks off for the whole file, with a warning. It
  serves measurement and final builds and does not replace the pragma.

**Semantics, as in Ada**: turning a check off does not make the program
correct. If the condition that the check would have detected occurs, the
**behaviour is undefined** (in Ada the program is *erroneous*). The
compiler may still keep a suppressed check (Ada allows this too).

Compile-time checks cannot be turned off (definite assignment, missing
`return` and `out` values, constants out of range, types): they cost
nothing at run time.

## 11. Program structure

```pascal
program NBody;
const ...
type ...
var ...
function ...
begin
  ...
end.
```

- A Luxia 0 program is **a single file**. It starts with `program Name;`,
  continues with declarations (§ 5) and routines (§ 8) in any order, and
  ends with the body `begin ... end.`
- The name after the final `end` is optional and, if present, checked.
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

## 12. Grammar

Wirth's notation: `=` defines, `.` ends a rule, `|` separates
alternatives, `[ ]` is optional, `{ }` repeats zero or more times, `( )`
groups; terminals are quoted; token classes are lowercase (`ident`,
`integer`, `real`, `char`, `string`). The grammar is LL(1).

```ebnf
Program    = "program" ident ";" { Decl } "begin" Stmts "end" [ ident ] "." .
Decl       = ConstSec | TypeSec | VarSec | Routine | Pragma ";" .
Pragma     = "pragma" ident "(" IdentList ")" .
ConstSec   = "const" ConstDecl ";" { ConstDecl ";" } .
ConstDecl  = ident [ ":" Type ] "=" Expr .
TypeSec    = "type" TypeDecl ";" { TypeDecl ";" } .
TypeDecl   = ident "=" Type .
VarSec     = "var" VarDecl ";" { VarDecl ";" } .
VarDecl    = IdentList ( ":" Type [ ":=" Expr ] | ":=" Expr ) .
IdentList  = ident { "," ident } .
Routine    = ( "procedure" ident Params | "function" ident Params ":" Type )
             ";" { LocalDecl } "begin" Stmts "end" [ ident ] ";" .
LocalDecl  = ConstSec | TypeSec | VarSec | Pragma ";" .
Params     = "(" [ Param { ";" Param } ] ")" .
Param      = [ "var" | "out" ] IdentList ":" Type .
Type       = ident [ "range" ( Range | "<>" ) ]
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
           | Designator | "(" Expr ")" | "new" "(" Type ")" .
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
`write`/`writeln`, and a routine inside another.

A branch of `case` begins with `when`, and a conditional `exit` or
`continue` is written `exit when c`; since every statement ends with `;`,
the two uses of `when` do not conflict.

## Appendix A. Planned for later versions (informative)

The following are not part of Luxia 0; they are listed so that programs
and tools can anticipate them.

- Modules with export marks, one file per module (`module`, `import` and
  `export` are already reserved).
- Exceptions and handlers (`raise`, `try ... except`), grouping the
  run-time checks as Ada's `Constraint_Error` does.
- Named association of arguments (`F(x => 1)`), optional.
- Aggregates for records and arrays; the braces `{ }` are kept free for
  them and for sets.
- Assignment of whole open arrays, with a length check.
- Iteration over the Unicode characters of a string.
- Redefinition of the names of the predefined library.
- Types for hardware: bit layout of records, byte order, alignment,
  variables at fixed addresses, volatile access, sizes imposed on
  subtypes.
- Ownership of pointers, making `dispose` safe at compile time, and
  lifting the rule of § 3.10 on arguments reached through a pointer.
