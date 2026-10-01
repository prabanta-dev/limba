/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * test_luxia.c - the Luxia front end against expected results. Each case
 * is a source, what it must give (the tokens in the short form of
 * limba_lx_show, or the syntax tree on one line) and the diagnoses it
 * must give as code@line:column, notes as L0000.
 */
#include "front/diag.h"
#include "front/source.h"
#include "luxia/gen.h"
#include "luxia/lex.h"
#include "luxia/parse.h"
#include "eval/eval.h"
#include "limba/opt.h"
#include "opt/pass.h"
#include "luxia/lower.h"
#include "luxia/sema.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *src;
    const char *tokens;
    const char *errors;
} lex_case;

static const lex_case lex_cases[] = {
    {"", "eof", ""},
    {"program Hello; begin end.", "program id:hello ; begin end . eof", ""},
    /* names fold to lowercase; keywords are lowercase only */
    {"Conto conto CONTO", "id:conto id:conto id:conto eof", ""},
    {"BEGIN Begin", "begin begin eof", "L0006@1:1 L0006@1:7"},
    {"beginning ends", "id:beginning id:ends eof", ""},
    {"_x", "id:x eof", "L0002@1:1"},
    /* integers */
    {"0 123 1_000_000 0xFF 0xff 0o17 0b1010",
     "int:0 int:123 int:1000000 int:255 int:255 int:15 int:10 eof", ""},
    {"18446744073709551615 18446744073709551616",
     "int:18446744073709551615 int:big eof", ""},
    {"1__0 12abc 0XFF 1E5", "int:0 int:0 int:0 real:0 eof",
     "L0007@1:1 L0007@1:6 L0007@1:12 L0007@1:18"},
    {"_1 1_ 0x 1e", "int:1 int:0 int:0 real:0 eof",
     "L0002@1:1 L0007@1:4 L0007@1:7 L0007@1:10"},
    {"0b102", "int:0 eof", "L0007@1:1"},
    /* reals, and 1..10 is a range */
    {"1.0 2.5e3 2e10 0.5 6.25e-2 1e+2",
     "real:1 real:2500 real:20000000000 real:0.5 real:0.0625 real:100 eof", ""},
    {"1e400", "real:0 eof", "L0008@1:1"},
    {"1..10 a[1..n]", "int:1 .. int:10 id:a [ int:1 .. id:n ] eof", ""},
    /* operators and punctuation */
    {"+ - * / ** & = <> < <= > >= := : ; , . .. ( ) [ ] ^",
     "+ - * / ** & = <> < <= > >= := : ; , . .. ( ) [ ] ^ eof", ""},
    {"x:=1 a.b p^.x", "id:x := int:1 id:a . id:b id:p ^ . id:x eof", ""},
    /* comments */
    {"a // x\nb (* c (* d *) e *) f", "id:a id:b id:f eof", ""},
    {"a (* b (* c *)", "id:a eof", "L0004@1:3"},
    /* strings and characters, as in Ada */
    {"\"ciao\" \"dice \"\"ciao\"\"\" \"\"",
     "str:\"ciao\" str:\"dice \"\"ciao\"\"\" str:\"\" eof", ""},
    {"x := \"abc\ny", "id:x := str:\"abc\" id:y eof", "L0003@1:6"},
    {"\"a\tb\"", "str:\"ab\" eof", "L0009@1:3"},
    {"'a' ''' '\xc3\xa8'", "char:U+0061 char:U+0027 char:U+00E8 eof", ""},
    /* 'ab' is one error, up to the apostrophe that closes it */
    {"'' 'ab'", "char:U+0000 char:U+0061 eof", "L0005@1:1 L0005@1:4"},
    /* characters that start no token; columns count UTF-8 characters */
    {"a @ b {", "id:a id:b eof", "L0002@1:3 L0002@1:7"},
    {"\"\xc3\xa8\" @", "str:\"\xc3\xa8\" eof", "L0002@1:5"},
    {"x \xc3\xa8", "id:x eof", "L0002@1:3"},
    {"a\n  @", "id:a eof", "L0002@2:3"},
    /* encoding */
    {"a \xff b", "eof", "L0001@1:3"},
    {"a \xed\xa0\x80", "eof", "L0001@1:3"},
    {"\xef\xbb\xbf"
     "begin",
     "begin eof", ""},
};

/* the tokens and the diagnoses of src, in the forms of the cases; with
   printed, the printed report instead of the tokens */
static void lex_string(const char *src, char **tokens, char **errors,
                       bool printed)
{
    limba_source s;
    limba_source_init(&s);
    uint32_t f = limba_source_add(&s, "t.luxia", src, strlen(src));
    limba_report rep;
    limba_report_init(&rep, &s, 'L', 0);
    limba_lx lx;
    limba_lx_init(&lx);
    limba_lx_run(&lx, &s, f, &rep);

    size_t n;
    FILE *out = open_memstream(tokens, &n);
    for (uint32_t i = 0; i < lx.ntok; i++) {
        if (i)
            fputc(' ', out);
        limba_lx_show(out, &lx, &lx.tok[i]);
    }
    fclose(out);
    out = open_memstream(errors, &n);
    for (uint32_t i = 0; i < rep.count; i++) {
        limba_where w;
        if (!limba_source_where(&s, rep.item[i].loc, &w))
            w.line = w.col = 0;
        fprintf(out, "%sL%04u@%u:%u", i ? " " : "", rep.item[i].code, w.line,
                w.col);
    }
    fclose(out);
    if (printed) {
        free(*tokens);
        out = open_memstream(tokens, &n);
        limba_report_print(&rep, out);
        fclose(out);
    }
    limba_lx_free(&lx);
    limba_report_free(&rep);
    limba_source_free(&s);
}

static int test_lexer(void)
{
    int failures = 0;
    size_t count = sizeof(lex_cases) / sizeof(lex_cases[0]);
    for (size_t i = 0; i < count; i++) {
        const lex_case *c = &lex_cases[i];
        char *tokens, *errors;
        lex_string(c->src, &tokens, &errors, false);
        if (strcmp(tokens, c->tokens) || strcmp(errors, c->errors)) {
            fprintf(stderr,
                    "test_luxia: lexer case %zu\n  tokens   %s\n  expected "
                    "%s\n  errors   %s\n  expected %s\n",
                    i, tokens, c->tokens, errors, c->errors);
            failures++;
        }
        free(tokens);
        free(errors);
    }
    return failures;
}

/* the printed form of a diagnosis, with its line of source */
static int test_report(void)
{
    static const char want[] =
        "t.luxia:1:6: error[L0003]: string not closed on its line\n"
        " 1 | x := \"abc\n"
        "   |      ^~~~\n";
    char *text, *errors;
    lex_string("x := \"abc", &text, &errors, true);
    int failures = 0;
    if (strcmp(text, want)) {
        fprintf(stderr, "test_luxia: printed report\n%s\nexpected\n%s", text,
                want);
        failures++;
    }
    free(text);
    free(errors);
    return failures;
}

/* the error limit stops the report and says so */
static int test_limit(void)
{
    limba_source s;
    limba_source_init(&s);
    const char *src = "@ @ @ @ @ @ @ @ @ @";
    uint32_t f = limba_source_add(&s, "t.luxia", src, strlen(src));
    limba_report rep;
    limba_report_init(&rep, &s, 'L', 4);
    limba_lx lx;
    limba_lx_init(&lx);
    limba_lx_run(&lx, &s, f, &rep);
    int failures = 0;
    if (rep.errors != 4 || !rep.full) {
        fprintf(stderr, "test_luxia: limit 4, got %u errors, full %d\n",
                rep.errors, rep.full);
        failures++;
    }
    limba_lx_free(&lx);
    limba_report_free(&rep);
    limba_source_free(&s);
    return failures;
}

/* the parser: a tree on one line and the diagnoses */
typedef struct {
    const char *src;
    const char *tree;
    const char *errors;
} parse_case;

static const parse_case expr_cases[] = {
    /* the five levels */
    {"a + b * c", "(binary + a (binary * b c))", ""},
    {"a * b + c", "(binary + (binary * a b) c)", ""},
    {"a - b - c", "(binary - (binary - a b) c)", ""},
    {"a & b & c", "(binary & (binary & a b) c)", ""},
    {"a div b mod c", "(binary mod (binary div a b) c)", ""},
    {"a shl 2 + 1", "(binary + (binary shl a 2) 1)", ""},
    {"-2 ** 2", "(unary - (binary ** 2 2))", ""},
    {"-a * b", "(unary - (binary * a b))", ""},
    {"-a + b", "(binary + (unary - a) b)", ""},
    {"a = -b", "(binary = a (unary - b))", ""},
    {"not a = b", "(binary = (unary not a) b)", ""},
    {"-not a", "(unary - (unary not a))", ""},
    {"a = b and c < d", "(binary and (binary = a b) (binary < c d))", ""},
    {"a and b and c", "(binary and (binary and a b) c)", ""},
    {"(a or b) and c", "(binary and (binary or a b) c)", ""},
    {"x in 1..10", "(in x 1 10)", ""},
    {"x in T and y", "(binary and (in x t -) y)", ""},
    /* operands */
    {"f(x, y:3, z:0:9)", "(call f [x (fmt y 3 -) (fmt z 0 9)])", ""},
    {"a.b[i]^.c", "(sel (deref (index (sel a b) i)) c)", ""},
    {"p()", "(call p [])", ""},
    {"new(Node)", "(new (tname node - -))", ""},
    {"Float64(i) / 2.5", "(binary / (call float64 [i]) 2.5)", ""},
    {"'a' & \"b\" & nil = true",
     "(binary = (binary & (binary & 'a' \"b\") nil) true)", ""},
    /* the rules of rigour */
    {"a and b or c", "(binary or (binary and a b) c)", "L0011@1:9"},
    {"a < b < c", "(binary < (binary < a b) c)", "L0012@1:7"},
    {"a = b <> c", "(binary <> (binary = a b) c)", "L0012@1:7"},
    {"a ** b ** c", "(binary ** (binary ** a b) c)", "L0012@1:8"},
    {"a * -b", "(binary * a (unary - b))", "L0013@1:5"},
    {"a + -b", "(binary + a (unary - b))", "L0013@1:5"},
    {"2 ** -1", "(binary ** 2 (unary - 1))", "L0013@1:6"},
    {"not -a", "(unary not (unary - a))", "L0013@1:5"},
    {"not a ** b", "(binary ** (unary not a) b)", "L0014@1:7"},
    {"abs x ** 2", "(binary ** (unary abs x) 2)", "L0014@1:7"},
    {"a = = b", "(binary = (binary = a error) b)", "L0010@1:5"},
    {"(a", "a", "L0010@1:3"},
};

static const parse_case program_cases[] = {
    {"program p; begin end.", "(program p [] [])", ""},
    {"program p;\n"
     "const N = 10;\n"
     "var s: Int32 := 0;\n"
     "begin\n"
     "  for var i := 1 to N do\n"
     "    s := s + i;\n"
     "  end;\n"
     "end p.",
     "(program p [(const n - 10) (var [s] (tname int32 - -) 0)] [(for to i "
     "- (range 1 n) [(assign s (binary + s i))])])",
     ""},
    {"program p;\n"
     "function f(a, b: Int32; var c: Int32): Int32;\n"
     "begin\n"
     "  case a of\n"
     "    when 1, 2..3: return b;\n"
     "    else return c;\n"
     "  end;\n"
     "end f;\n"
     "begin\n"
     "  loop\n"
     "    exit when x > 0;\n"
     "  end;\n"
     "  while x < 1 do x := x + 1; end;\n"
     "  repeat x := 1; until x = 1;\n"
     "  for var i: Int32 := 10 downto 1 do continue; end;\n"
     "end.",
     "(program p [(routine function f [(param [a b] (tname int32 - -)) "
     "(param var [c] (tname int32 - -))] (tname int32 - -) (body [] [(case "
     "a [(when [(label 1 -) (label 2 3)] [(return b)])] [(return c)])]))] "
     "[(loop [(exit (binary > x 0))]) (while (binary < x 1) [(assign x "
     "(binary + x 1))]) (repeat [(assign x 1)] (binary = x 1)) (for downto "
     "i (tname int32 - -) (range 10 1) [(continue -)])])",
     ""},
    {"program p;\n"
     "type\n"
     "  Colore = (Rosso, Verde);\n"
     "  P = ^Nodo;\n"
     "  Nodo = record\n"
     "    x, y: Float64;\n"
     "    next: P;\n"
     "  end;\n"
     "  V = array[Int32 range 1..10] of Float64;\n"
     "  M = new Float64;\n"
     "procedure q(out r: array[Int32 range <>] of Byte);\n"
     "begin\n"
     "end;\n"
     "begin\n"
     "end.",
     "(program p [(type colore (tenum [rosso verde])) (type p (tptr (tname "
     "nodo - -))) (type nodo (trecord [(field [x y] (tname float64 - -)) "
     "(field [next] (tname p - -))])) (type v (tarray (tname int32 1 10) "
     "(tname float64 - -))) (type m (tnew (tname float64 - -))) (routine "
     "procedure q [(param out [r] (topen (tbox int32) (tname byte - -)))] - (body [] "
     "[]))] [])",
     ""},
    {"program p;\n"
     "begin\n"
     "  if a then\n"
     "    f();\n"
     "  elsif b then\n"
     "    var x := 1;\n"
     "  else\n"
     "    g(1);\n"
     "  end;\n"
     "end.",
     "(program p [] [(if [(arm a [(callst (call f []))]) (arm b [(var [x] - "
     "1)])] [(callst (call g [1]))])])",
     ""},
    /* errors, and the parser going on after them */
    {"program p;\nbegin\n  x := 1\n  y := 2;\nend.",
     "(program p [] [(assign x 1) (assign y 2)])", "L0010@3:9"},
    {"program p; begin x := 1;; end.", "(program p [] [(assign x 1)])",
     "L0018@1:25"},
    {"program p; begin x = 1; end.", "(program p [] [error])", "L0016@1:20"},
    {"program p; begin f; end.", "(program p [] [error])", "L0016@1:19"},
    {"program p; begin x := a and b or c; end.",
     "(program p [] [(assign x (binary or (binary and a b) c))])",
     "L0011@1:31"},
    {"program p; begin for i := 1 to 2 do end; end.",
     "(program p [] [(for to i - (range 1 2) [])])", "L0010@1:22"},
    {"program p; procedure a; begin end; begin end.",
     "(program p [(routine procedure a [] - (body [] []))] [])", "L0010@1:23"},
    {"program p; procedure a(); begin end b; begin end.",
     "(program p [(routine procedure a [] - (body [] []))] [])", "L0015@1:37"},
    {"program p;\n"
     "procedure a();\n"
     "  procedure b();\n"
     "  begin\n"
     "  end;\n"
     "begin\n"
     "end;\n"
     "begin\n"
     "end.",
     "(program p [(routine procedure a [] - (body [(routine procedure b [] "
     "- (body [] []))] []))] [])",
     "L0017@3:3"},
    {"program p;\nbegin\n  if a then\n    x := 1;\n   end;\nend.",
     "(program p [] [(if [(arm a [(assign x 1)])] -)])", "L0020@5:4"},
    {"program p; begin end. x", "(program p [] [])", "L0019@1:23"},
    {"program p; begin if a then begin x := 1; end; end.",
     "(program p [] [(if [(arm a [(assign x 1)])] -)])", "L0016@1:28"},
};

static void parse_string(const char *src, bool whole, char **tree,
                         char **errors)
{
    limba_source s;
    limba_source_init(&s);
    uint32_t f = limba_source_add(&s, "t.luxia", src, strlen(src));
    limba_report rep;
    limba_report_init(&rep, &s, 'L', 0);
    limba_lx lx;
    limba_lx_init(&lx);
    limba_lx_run(&lx, &s, f, &rep);
    limba_lx_ast t;
    limba_lx_ast_init(&t);
    if (whole)
        limba_lx_parse(&t, &lx, &s, &rep);
    else
        limba_lx_parse_expr(&t, &lx, &s, &rep);
    size_t n;
    FILE *out = open_memstream(tree, &n);
    limba_lx_ast_show(out, &t, &lx, t.root, -1);
    fclose(out);
    out = open_memstream(errors, &n);
    for (uint32_t i = 0; i < rep.count; i++) {
        limba_where w;
        if (!limba_source_where(&s, rep.item[i].loc, &w))
            w.line = w.col = 0;
        fprintf(out, "%sL%04u@%u:%u", i ? " " : "", rep.item[i].code, w.line,
                w.col);
    }
    fclose(out);
    limba_lx_ast_free(&t);
    limba_lx_free(&lx);
    limba_report_free(&rep);
    limba_source_free(&s);
}

static int test_parser(const parse_case *cases, size_t count, bool whole,
                       const char *what)
{
    int failures = 0;
    for (size_t i = 0; i < count; i++) {
        char *tree, *errors;
        parse_string(cases[i].src, whole, &tree, &errors);
        if (strcmp(tree, cases[i].tree) || strcmp(errors, cases[i].errors)) {
            fprintf(stderr,
                    "test_luxia: %s case %zu\n  tree     %s\n  expected "
                    "%s\n  errors   %s\n  expected %s\n",
                    what, i, tree, cases[i].tree, errors, cases[i].errors);
            failures++;
        }
        free(tree);
        free(errors);
    }
    return failures;
}

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* the semantic phase: a program and the diagnoses it must give */
typedef struct {
    const char *src;
    const char *errors;
} sema_case;

static const sema_case sema_cases[] = {
    /* a program that uses most of Luxia 0 */
    {"program P;\n"
     "const\n"
     "  N = 10;\n"
     "  Half = N div 2;\n"
     "  Pi: Float64 = 3.141592653589793;\n"
     "  Exact: Boolean = 0.1 + 0.2 = 0.3;\n"
     "type\n"
     "  Colore = (Rosso, Verde, Blu);\n"
     "  Indice = Int32 range 1..N;\n"
     "  Vettore = array[Indice] of Float64;\n"
     "  PNodo = ^Nodo;\n"
     "  Nodo = record\n"
     "    valore: Int64;\n"
     "    next: PNodo;\n"
     "  end;\n"
     "  Metri = new Float64;\n"
     "var\n"
     "  v: Vettore;\n"
     "  c: Colore := Verde;\n"
     "  m: Metri;\n"
     "  p: PNodo := nil;\n"
     "  s: String := \"ciao\" & ' ' & \"mondo\";\n"
     "  b: Bits32 := 0xFF;\n"
     "function Somma(a: array[Int32 range <>] of Float64): Float64;\n"
     "var t: Float64 := 0.0;\n"
     "begin\n"
     "  for var i := low(a) to high(a) do\n"
     "    t := t + a[i];\n"
     "  end;\n"
     "  return t;\n"
     "end Somma;\n"
     "procedure Riempi(var a: Vettore; out n: Int32);\n"
     "begin\n"
     "  for var i: Int32 := 1 to 10 do\n"
     "    a[i] := Float64(i) * 0.5;\n"
     "  end;\n"
     "  n := 10;\n"
     "end;\n"
     "begin\n"
     "  var n: Int32;\n"
     "  Riempi(v, n);\n"
     "  writeln(\"somma = \", Somma(v):0:3, \" n = \", n, Half);\n"
     "  m := Metri(2.5);\n"
     "  p := new(Nodo);\n"
     "  p.valore := 42;\n"
     "  p.next := nil;\n"
     "  case c of\n"
     "    when Rosso: writeln(\"r\");\n"
     "    when Verde, Blu: writeln(\"vb\");\n"
     "  end;\n"
     "  b := (b shl 4) xor Bits32(0x0F);\n"
     "  if s[1] = 99 and n in 1..10 and c in Colore then\n"
     "    dispose(p);\n"
     "  end;\n"
     "  while n > 0 do\n"
     "    n := n - 1;\n"
     "    exit when n = 5;\n"
     "  end;\n"
     "  var k: Int32 := 3;\n"
     "  var a: array[Int32 range 1..k] of Byte;\n"
     "  a[1] := 2;\n"
     "  const Z = Pi * 2.0;\n"
     "  writeln(sqrt(m / Metri(2.0)) > Metri(Z));\n"
     "end P.",
     ""},
    /* names */
    {"program p; begin x := 1; end.", "L0021@1:18"},
    {"program p; var Conto: Int32 := 0; begin conto := 1; end.", "L0022@1:41"},
    {"program p; var a: Int32; a: Int64; begin end.", "L0023@1:26 L0000@1:16"},
    {"program p; var writeln: Int32; begin end.", "L0023@1:16"},
    {"program p; const A = B; const B = 1; begin end.",
     "L0024@1:22 L0000@1:31"},
    {"program p; type T = S; S = T; begin end.", "L0047@1:17"},
    {"program p; type R = record x: R; end; begin end.", "L0047@1:31"},
    /* types of operands */
    {"program p; var a: Int32; b: Int64; begin a := b; end.", "L0027@1:47"},
    {"program p; var a: Int32; b: Int64; begin a := a + b; end.", "L0027@1:49"},
    {"program p; var a: Int8 := 300; begin end.", "L0029@1:27"},
    {"program p; const A: Int8 = 100 + 100; begin end.", "L0029@1:32"},
    {"program p; begin var x := 0; end.", "L0033@1:27"},
    {"program p; var u: UInt32 := 1; begin u := -u; end.", "L0028@1:43"},
    {"program p; var a: Int32 := 7 / 2; begin end.", "L0028@1:30"},
    {"program p; var a: UInt32 := 1; begin a := a shl 2; end.", "L0028@1:45"},
    {"program p; var f: Float32 := 16777217; begin end.", "L0029@1:30"},
    {"program p; const Z: Float32 = 1e39; begin end.", "L0029@1:31"},
    {"program p; const D = 1 div 0; begin end.", "L0032@1:24"},
    {"program p; begin if 1 then end; end.", "L0027@1:21"},
    {"program p; var s: String := String(1); begin end.", "L0039@1:29"},
    /* variables, constants, calls */
    {"program p; const C = 1; begin C := 2; end.", "L0034@1:31"},
    {"program p; begin for var i: Int32 := 1 to 2 do i := 3; end; end.",
     "L0034@1:48"},
    {"program p; procedure q(a: Int32); begin a := 1; end; begin end.",
     "L0034@1:41"},
    {"program p; var s: String := \"a\"; begin s[1] := 65; end.", "L0034@1:41"},
    {"program p; var a: Int32; const B = a; begin end.", "L0030@1:36"},
    {"program p; procedure q(a: Int32); begin end; begin q(); end.",
     "L0035@1:52"},
    {"program p; function f(): Int32; begin return 1; end; begin f(); end.",
     "L0037@1:60"},
    {"program p; procedure q(); begin end; var x: Int32 := q(); begin end.",
     "L0038@1:54"},
    {"program p; var s: String := str(1:2); begin end.", "L0048@1:33"},
    {"program p; var a: array[Int32 range <>] of Int32; begin end.",
     "L0049@1:19"},
    {"program p; var n: Int32 := 3; var a: array[Int32 range 1..n] of Int32;"
     " begin end.",
     "L0051@1:44"},
    /* the pragmas and the names of the checks (§ 9) */
    {"program p; pragma sopress(range_check); begin end.", "L0057@1:19"},
    {"program p; pragma suppress(range_chek, index_check); begin end.",
     "L0058@1:28"},
    /* an object reached through a pointer is no var argument, nor an
       out record or array (§ 3.10) */
    {"program p; type R = record a: Int32; s: String; end; var q: ^R; "
     "procedure f(var x: Int32); begin x := 1; end f; begin q := new(R); "
     "f(q.a); end.",
     "L0059@1:135"},
    {"program p; type R = record a: Int32; end; var q: ^R; procedure g(var "
     "v: R); begin v.a := 1; end g; begin q := new(R); g(q^); end.",
     "L0059@1:122"},
    {"program p; type R = record a: Int32; end; var q: ^R; procedure h(out "
     "v: R); begin v.a := 1; end h; begin q := new(R); h(q^); end.",
     "L0059@1:122"},
    /* arrays made by new (§ 3.10, § 9.7) and move (§ 9.5) */
    {"program p; type R = record x: Int32; end; procedure s(r: R); begin "
     "end s; begin var q := new(R); s(q^); end.",
     "L0059@1:101"},
    {"program p; type A = array[Int32 range <>] of Int32; begin var p := "
     "new(A range 1..3); var q := new(A range 1..3); p^ := q^; end.",
     "L0049@1:116 L0049@1:122"},
    {"program p; type A = array[Int32 range <>] of Int32; begin var p := "
     "new(A); end.",
     "L0049@1:68"},
    {"program p; type A = array[Int32 range <>] of Int32; B = "
     "array[Int32 range <>] of Int64; begin var p := new(A range 1..3); "
     "var q := new(B range 1..3); move(p^, 1, q^, 1, 3); end.",
     "L0027@1:164"},
    {"program p; type A = array[Int32 range <>] of Int32; procedure f(a: "
     "A); begin move(a, low(a), a, low(a), 0); end f; begin end.",
     "L0034@1:94"},
    /* translate, reverse, occurrences (§ 9.5): a String is not written,
       the table is indexed by Byte and holds Bytes, the elements are
       Bytes, reverse takes an array, written, the pattern holds Bytes,
       the names are the language's */
    {"program p; type T = array[Byte] of Byte; var tb: T; s: String; begin translate(s, 1, 1, tb); end.",
     "L0027@1:80"},
    {"program p; type T = array[Byte] of Byte; var tb: T; u: array[Int32 range 0..255] of Byte; a: array[Int32 range 1..3] of Byte; begin translate(a, 1, 3, u); end.",
     "L0027@1:152"},
    {"program p; type T = array[Byte] of Byte; var tb: T; type S = Byte range 0..9; var a: array[Int32 range 1..3] of S; begin translate(a, 1, 3, tb); end.",
     "L0027@1:132"},
    {"program p; type T = array[Byte] of Byte; var tb: T; n: Int32; begin reverse(n, 1, 1); end.",
     "L0027@1:77"},
    {"program p; type A = array[Int32 range <>] of Int32; procedure f(a: "
     "A); begin reverse(a, low(a), 0); end f; begin end.",
     "L0034@1:86"},
    {"program p; var a: array[Int32 range 1..3] of Byte; q: array[Int32 "
     "range 1..3] of Int32; n: Int32; begin n := occurrences(a, 1, 3, q); "
     "end.",
     "L0027@1:131"},
    {"program p; var reverse: Int32; begin end.", "L0023@1:16"},
    {"program p; type T = array[Byte] of Byte; var q: ^T; n: Int64; a: "
     "array[Int8 range -2..2] of Byte; m: Int8; begin q := new(T); "
     "translate(a, -2, 5, q^); n := occurrences(\"abab\", 1, 4, \"ab\"); m "
     ":= occurrences(a, -2, 5, a); end.",
     ""},
    /* readbytes and writebytes (§ 9.1, § 9.2): an array of Bytes read
       into, written; a String only written; the result used; 141 is the
       status of a closed output */
    {"program p; var a: array[Int32 range 1..3] of Byte; n: Int32; s: String; begin n := readbytes(s, 1, 1); end.",
     "L0027@1:94"},
    {"program p; var a: array[Int32 range 1..3] of Int32; begin writebytes(a, 1, 3); end.",
     "L0027@1:70"},
    {"program p; type S = Byte range 0..9; var a: array[Int32 range 1..3] of S; n: Int32; begin n := readbytes(a, 1, 3); end.",
     "L0027@1:106"},
    {"program p; type A = array[Int32 range <>] of Byte; procedure f(a: A); var n: Int32; begin n := readbytes(a, low(a), 0); end f; begin end.",
     "L0034@1:106"},
    {"program p; var a: array[Int32 range 1..3] of Byte; n: Int32; s: String; begin readbytes(a, 1, 3); end.",
     "L0037@1:79"},
    {"program p; var writebytes: Int32; begin end.",
     "L0023@1:16"},
    {"program p; type Bs = array[Int32 range <>] of Byte; var q: ^Bs; n: Int64; m: Int32; begin q := new(Bs range 1..4); m := readbytes(q^, 1, 4); writebytes(q^, 1, m); writebytes(\"abc\", 2, 2); n := 1; end.",
     ""},
    {"program p; begin halt(141); end.",
     "L0055@1:23"},
    {"program p; type R = record s: String; end; var q: ^R; b: Boolean; "
     "begin q := new(R); b := readline(q.s); end.",
     "L0059@1:101"},
    {"program p; type R = record a: Int32; end; var q: ^R; b: Boolean; "
     "begin q := new(R); b := val(\"1\", q.a); end.",
     "L0059@1:100"},
    /* statements */
    {"program p; type Col = (A, B, D); var c: Col; begin case c of when A: "
     "c := B; end; end.",
     "L0044@1:52"},
    {"program p; var c: Int32; begin case c of when 1..5: c := 1; when 3: "
     "c := 2; else c := 3; end; end.",
     "L0043@1:66"},
    {"program p; begin exit; end.", "L0045@1:18"},
    {"program p; procedure q(); begin return 1; end; begin end.", "L0046@1:33"},
    {"program p; function f(): Int32; begin return; end; begin end.",
     "L0046@1:39"},
    /* 1 is the exit status of the errors at run time */
    {"program p; begin halt(1); end.", "L0055@1:23"},
    {"program p; begin halt(256); end.", "L0055@1:23"},
    /* a width from 0, decimals from 0 to 100, arguments from 1 */
    {"program p; var x: Int32 := 1; begin writeln(x:-1); end.", "L0029@1:47"},
    {"program p; var r: Float64 := 1.0; begin writeln(r:5:101); end.",
     "L0029@1:53"},
    {"program p; begin writeln(arg(0)); end.", "L0029@1:30"},
    /* the name after end is written as declared (§ 2.2) */
    {"program p; procedure Show(); begin end show; begin end p.", "L0022@1:40"},
    {"program p; begin end P.", "L0022@1:22"},
    /* a file that is not UTF-8: one error, no echo from the parser */
    {"program p; \xff begin end.", "L0001@1:12"},
    /* ord of a character, a Boolean or an enumeration only; low and high
       of a discrete type */
    {"program p; var x: Int32 := 1; begin writeln(ord(x)); end.", "L0027@1:49"},
    {"program p; begin writeln(low(Float64)); end.", "L0027@1:30"},
    /* open arrays: index of the same base, same elements, bounds inside a
       range, only for parameters */
    {"program p; var b: array[Int16 range 1..2] of Int8; procedure Q(v: "
     "array[Int32 range <>] of Int8); begin end Q; begin Q(b); end.",
     "L0027@1:120"},
    {"program p; type S = Int32 range 1..10; var a: array[Int32 range 0..20] "
     "of Int8; procedure P(v: array[S range <>] of Int8); begin end P; begin "
     "P(a); end.",
     "L0029@1:145"},
    {"program p; var y: Int32 range <>; begin end.", "L0049@1:19"},
    {"program p; type V = array[Int8 range <>] of Int8; function F(): V; "
     "begin end F; begin end.",
     "L0049@1:65"},
    /* the boundary with C (§ 3.13, § 8.5, § 10.4): what does not cross,
       the pragmas written wrong, the restriction, the opaque pointer */
    {"program p; procedure P(s: String); external \"c\"; begin end.",
     "L0060@1:27"},
    {"program p; function F(): Boolean; external \"c\"; begin end.",
     "L0060@1:26"},
    {"program p; type R = record a: CInt; end; procedure P(r: R); external \"c\"; begin end.",
     "L0060@1:57"},
    {"program p; type R = record a: CInt; end; pragma convention(c, R); type Q = ^R; procedure P(q: Q); external \"c\"; begin end.",
     "L0060@1:95"},
    {"program p; type S = Int32 range 1..9; procedure P(s: S); external \"c\"; begin end.",
     "L0060@1:54"},
    {"program p; type R = record a: String; end; pragma convention(c, R); begin end.",
     "L0060@1:65"},
    {"program p; type R = record a: CInt; end; pragma convention(ada, R); begin end.",
     "L0062@1:42"},
    {"program p; type R = record a: CInt; end; pragma convention(c, Int32); begin end.",
     "L0062@1:63"},
    {"program p; pragma restrictions(no_heap); begin end.", "L0062@1:12"},
    {"program p; pragma restrictions(no_external); procedure P(); external \"c\"; begin end.",
     "L0061@1:56"},
    {"program p; pragma restrictions(no_external); var s: CString; begin end.",
     "L0061@1:53"},
    {"program p; type R = record a: CInt; end; begin pragma convention(c, R); end.",
     "L0062@1:55"},
    {"program p; var q: CPointer; begin q := q + 1; end.", "L0027@1:44"},
    {"program p; var q: CPointer; i: Int64; begin i := Int64(q); end.",
     "L0039@1:50"},
    {"program p; type TA = new CPointer; TB = new CPointer; var x: TA; y: TB; begin x := y; end.",
     "L0027@1:84"},
    {"program p; var c: CInt; i: Int32 := 1; begin c := i; end.", "L0027@1:51"},
    {"program p; var s: String; begin writeln(cvalue(s)); end.", "L0027@1:48"},
    {"program p; procedure P(); external name \"x\"; begin end.", "L0010@1:36"},
    {"program p; var q: CPointer; begin writeln(q^); end.", "L0042@1:44"},
    /* BigInt: not discrete, not bits, no range, no mixing, no
    real constant, the limit of the constants */
    {"program p; type R = BigInt range 1..10; begin end.", "L0050@1:21"},
    {"program p; var a: array[BigInt] of Int32; begin end.", "L0027@1:25"},
    {"program p; var b: BigInt := 2; i: Int32 := 2; begin i := i ** b; end.",
     "L0028@1:63"},
    {"program p; var b: BigInt := 2; begin b := b shl 1; end.", "L0028@1:45"},
    {"program p; var b: BigInt := 65; begin writeln(chr(b)); end.",
     "L0027@1:51"},
    {"program p; var b: BigInt := 2; i: Int64 := 2; begin b := b + i; end.",
     "L0027@1:60"},
    {"program p; var b: BigInt := 1.5; begin end.", "L0027@1:29"},
    {"program p; const C: BigInt = 2 ** 20000; begin end.", "L0031@1:32"},
    {"program p; var b: BigInt := 2; begin case b of when 1: writeln(1); else writeln(2); end; end.",
     "L0027@1:43"},
    {"program p; var b: BigInt := 2; begin b := succ(b); end.", "L0027@1:48"},
    {"program p; var b: BigInt := 2; begin for var i: BigInt := 1 to b do end; end.",
     "L0027@1:59"},
    {"program p; var b: BigInt := 2; begin b := b and b; end.", "L0028@1:45"},
    {"program p; var b: BigInt := 2; begin writeln(b / b); end.", "L0028@1:48"},
    {"program p; var b: BigInt := 2; begin writeln(low(BigInt)); end.",
     "L0027@1:50"},
};

static void check_string(const char *src, char **errors)
{
    limba_source s;
    limba_source_init(&s);
    uint32_t f = limba_source_add(&s, "t.luxia", src, strlen(src));
    limba_report rep;
    limba_report_init(&rep, &s, 'L', 0);
    limba_lx lx;
    limba_lx_init(&lx);
    limba_lx_run(&lx, &s, f, &rep);
    limba_lx_ast t;
    limba_lx_ast_init(&t);
    limba_lx_parse(&t, &lx, &s, &rep);
    if (rep.errors == 0) {
        limba_lxs sema;
        limba_lxs_init(&sema, &t, &lx, &s, &rep);
        limba_lxs_check(&sema);
        limba_lxs_free(&sema);
    }
    size_t n;
    FILE *out = open_memstream(errors, &n);
    for (uint32_t i = 0; i < rep.count; i++) {
        limba_where w;
        if (!limba_source_where(&s, rep.item[i].loc, &w))
            w.line = w.col = 0;
        fprintf(out, "%sL%04u@%u:%u", i ? " " : "", rep.item[i].code, w.line,
                w.col);
    }
    fclose(out);
    if (getenv("LIMBA_TEST_VERBOSE"))
        limba_report_print(&rep, stderr);
    limba_lx_ast_free(&t);
    limba_lx_free(&lx);
    limba_report_free(&rep);
    limba_source_free(&s);
}

/* programs compiled to the IR and run by the reference interpreter:
   what they print, and how they end: "ok", "trap N", "halt N", or the
   compile errors as "errors L0053@1:54" */
typedef struct {
    const char *src;
    const char *out;
    const char *end;
} run_case;

static const run_case run_cases[] =
    {
        {"program p; begin writeln(\"ciao, \", 42, ' ', true); end.",
         "ciao, 42 true\n", "ok"},
        /* constant strings joined, two empty ones too */
        {"program p; const e = \"\" & \"\"; f = e & 'a' & \"\"; "
         "begin writeln(length(e), f); end.",
         "0a\n", "ok"},
        /* integers: div truncates, mod has the sign of the divisor, rem of
           the dividend */
        {"program p;\nvar a, b, c: Int32;\nbegin\n  a := -7; b := 2; c := -2;\n"
         "  writeln(a div b, \" \", a mod b, \" \", a rem b);\n  a := 7;\n"
         "  writeln(a div c, \" \", a mod c, \" \", a rem c);\nend.",
         "-3 1 -1\n-3 -1 1\n", "ok"},
        {"program p; var a: Int8 := 100; begin a := a + a; writeln(a); end.",
         "", "trap 6"},
        {"program p; var u: UInt32 := 0; begin u := u - 1; end.", "", "trap 6"},
        {"program p; var u: UInt8 := 16; begin u := u * u; end.", "", "trap 6"},
        {"program p; var b: Bits8 := 250;\nbegin\n  b := b + 10; writeln(b);\n"
         "  b := not b; writeln(b);\n  b := b shl 1; writeln(b);\nend.",
         "4\n251\n246\n", "ok"},
        {"program p; var b: Bits8 := 1; n: Int32 := 8; begin b := b shl n; "
         "end.",
         "", "trap 104"},
        {"program p; type Vec = array[Int32 range 1..3] of Int32; var v: Vec; i: "
         "Int32 := 4; begin v[i] := 1; end.",
         "", "trap 100 at 1:91"},
        {"program p; type P = Int32 range 0..100; var x: P; y: Int32 := 101; "
         "begin x := y; end.",
         "", "trap 101"},
        {"program p; type N = record a: Int32; end; var p: ^N; begin p.a := 1; "
         "end.",
         "", "trap 102"},
        {"program p; var a: Int32 := 1; z: Int32 := 0; begin writeln(a div z); "
         "end.",
         "", "trap 11 at 1:62"},
        {"program p; function f(x: Int32): Int32; begin return 10 div x; end; "
         "begin writeln(f(0)); end.",
         "", "trap 11 at 1:57"},
        {"program p; var a: Int64 := -9223372036854775807 - 1; m: Int64 := -1; "
         "begin writeln(a div m); end.",
         "", "trap 6"},
        {"program p; var f: Float64 := 2.5;\nbegin\n  writeln(Int32(f), \" \", "
         "Int32(-f), \" \", Int32(2.5));\n  f := 3.0e10; writeln(Int32(f));\n"
         "end.",
         "3 -3 3\n", "trap 103"},
        /* loops */
        {"program p;\nvar s: Int64 := 0;\nbegin\n"
         "  for var i: Int8 := 120 to 127 do s := s + Int64(i); end;\n"
         "  writeln(s);\n"
         "  for var i: Int32 := 5 downto 1 do write(i); end;\n  writeln();\n"
         "  for var i: Int32 := 3 to 1 do writeln(\"never\"); end;\nend.",
         "988\n54321\n", "ok"},
        {"program p;\nvar i: Int32 := 0; n: Int32 := 0;\nbegin\n"
         "  while i < 10 do\n    i := i + 1;\n    continue when i mod 2 = 0;\n"
         "    n := n + i;\n  end;\n  writeln(n);\n"
         "  repeat i := i - 3; until i < 0;\n  writeln(i);\n"
         "  loop i := i + 1; exit when i = 5; end;\n  writeln(i);\nend.",
         "25\n-2\n5\n", "ok"},
        {"program p;\ntype Col = (A, B, D);\nvar c: Col := B; k: Int32 := 7;\nbegin\n"
         "  case c of when A: writeln(\"a\"); when B, D: writeln(\"bd\"); end;\n"
         "  case k of when 1..5: writeln(\"low\"); when 6..9: writeln(\"mid\"); "
         "else writeln(\"high\"); end;\n"
         "  k := 42;\n"
         "  case k of when 1..5: writeln(\"low\"); else writeln(\"high\"); end;\n"
         "end.",
         "bd\nmid\nhigh\n", "ok"},
        /* strings and output */
        {"program p;\nvar s: String := \"ciao\";\nbegin\n"
         "  s := s & ' ' & \"mondo\";\n"
         "  writeln(s, \" \", length(s), \" \", s[1], \" \", copy(s, 6, 5));\n"
         "  writeln(s = \"ciao mondo\", \" \", \"a\" < \"b\");\n"
         "  writeln(chr(65), ord('a'), str(12) & \"!\");\nend.",
         "ciao mondo 10 99 mondo\ntrue true\nA9712!\n", "ok"},
        {"program p; begin writeln(3.14159:0:2, \"|\", 42:5, \"|\", \"ab\":4, "
         "\"|\", 1.5:8:3); end.",
         "3.14|   42|  ab|   1.500\n", "ok"},
        {"program p; var f: Float32 := 0.1; begin writeln(Float64(f):0:10); "
         "end.",
         "0.1000000015\n", "ok"},
        /* and then: no nil reached */
        {"program p; type N = record a: Int32; end; var p: ^N := nil; begin if "
         "p <> nil and p.a = 1 then writeln(\"no\"); else writeln(\"safe\"); "
         "end; end.",
         "safe\n", "ok"},
        /* records, arrays, parameters */
        {"program p;\ntype\n  Pt = record x, y: Int32; end;\n"
         "  Vec = array[Int32 range 0..4] of Int32;\nvar v: Vec; q: Pt;\n"
         "procedure Fill(var a: array[Int32 range <>] of Int32);\nbegin\n"
         "  for var i := low(a) to high(a) do a[i] := Int32(i) * 10; end;\n"
         "end;\n"
         "function Sum(a: array[Int32 range <>] of Int32): Int64;\nvar s: Int64 := "
         "0;\nbegin\n"
         "  for var i := 0 to high(a) do s := s + Int64(a[i]); end;\n"
         "  return s;\nend;\n"
         "procedure Shift(var p: Pt; dx: Int32);\nbegin p.x := p.x + dx; end;\n"
         "begin\n  Fill(v);\n  writeln(Sum(v), \" \", v[4]);\n"
         "  q.x := 1; q.y := 2;\n  Shift(q, 5);\n  var r: Pt := q;\n  r.y := 9;\n"
         "  writeln(q.x, \" \", q.y, \" \", r.x, \" \", r.y);\nend.",
         "100 40\n6 2 6 9\n", "ok"},
        {"program p;\ntype PN = ^Node; Node = record v: Int32; next: PN; end;\n"
         "var head: PN := nil; n: Int32 := 0;\nbegin\n"
         "  for var i: Int32 := 1 to 5 do\n    var c := new(Node);\n"
         "    c.v := i; c.next := head; head := c;\n  end;\n"
         "  var p := head;\n  while p <> nil do\n    n := n + p.v;\n"
         "    var q := p.next;\n    dispose(p);\n    p := q;\n  end;\n"
         "  writeln(n);\nend.",
         "15\n", "ok"},
        {"program p;\nbegin\n  var n: Int32 := 4;\n"
         "  var a: array[Int32 range 1..n] of Int64;\n"
         "  for var i := 1 to n do a[i] := Int64(i) * Int64(i); end;\n"
         "  writeln(a[4], \" \", length(a), \" \", low(a), \" \", high(a));\n"
         "end.",
         "16 4 1 4\n", "ok"},
        {"program p;\nvar n: Int32 := 5;\nprocedure Get(out r: Int32);\nbegin r := "
         "7; end;\nbegin\n  if not val(\"123\", n) then writeln(\"bad\"); end;\n"
         "  writeln(n);\n  if val(\"x1\", n) then writeln(\"?\"); end;\n"
         "  writeln(n);\n  var z: Int32;\n  Get(z);\n  writeln(z);\nend.",
         "123\n123\n7\n", "ok"},
        {"program p;\nfunction Fib(n: Int32): Int32;\nbegin\n  if n < 2 then "
         "return n; end;\n  return Fib(n - 1) + Fib(n - 2);\nend;\nbegin "
         "writeln(Fib(20)); end.",
         "6765\n", "ok"},
        {"program p; var b: Int64 := 3; begin writeln(b ** 4, \" \", 2.0 ** 10); "
         "end.",
         "81 1024.0\n", "ok"},
        {"program p; begin writeln(\"a\"); halt(3); writeln(\"b\"); end.",
         "a\n", "halt 3"},
        /* a range is checked at the assignment, the argument, the return,
           whatever the value computed last */
        {"program p;\ntype R = Int16 range 0..10;\nvar x: R := 3; y: Int16 := "
         "10;\nbegin\n  x := (y + 1);\nend p.",
         "", "trap 101 at 5:3"},
        {"program p;\ntype R = Int16 range 0..10;\nvar y: Int16 := 10;\nprocedure "
         "Q(a: R);\nbegin\nend Q;\nbegin\n  Q((y + 1));\nend p.",
         "", "trap 101 at 8:3"},
        {"program p;\ntype R = Int16 range 0..10;\nvar x: R := 10;\nfunction "
         "F(a: R): R;\nbegin\n  return (a + 1);\nend F;\nbegin\n  "
         "writeln(F(x));\nend p.",
         "", "trap 101 at 6:3"},
        /* inside an operation a constant takes the base type of a range */
        {"program p; type R = Int16 range -5..20; var x: R := 3; y: Int16 := 0; "
         "begin y := x + 100; writeln(y, \" \", x < 100); end p.",
         "103 true\n", "ok"},
        /* in on constants is a constant (it made an iconst without a type)
         */
        {"program p; type R = Int8 range 1..5; var b: Boolean := false; begin b "
         ":= 0 in 0..10; writeln(b, \" \", 5 in 1..3, \" \", 3 in R, \" \", 9 in "
         "R); end p.",
         "true false true false\n", "ok"},
        /* a value without a sign never fits a range below zero */
        {"program p; type R = Int32 range -1..-1; var b: Bits16 := 5; begin "
         "writeln(R(b)); end p.",
         "", "trap 103"},
        /* empty ranges, as in Ada: no element, every index outside */
        {"program p; type E = Int32 range 1..0; A = array[E] of Int32; var f: A; "
         "begin writeln(length(f), \" \", low(f), \" \", high(f)); end p.",
         "0 1 0\n", "ok"},
        {"program p; var n: Int32 := 0; begin var d: array[Int32 range 1..n] of "
         "Int32; writeln(length(d)); for var i := low(d) to high(d) do "
         "writeln(i); end; writeln(d[1]); end p.",
         "0\n", "trap 100"},
        {"program p; type Z = Int8 range 5..1; var y: Int8 := 3; begin var z: Z "
         ":= y; end p.",
         "", "trap 101"},
        /* halt with a computed 1 is an error at run time, status 1 */
        {"program p; var n: Int32 := 1; begin halt(n); end p.", "", "trap 101"},
        {"program p; var n: Int32 := 0; begin halt(n); end p.", "", "halt 0"},
        /* val reads a literal of Luxia of the type of its variable, spaces
           around; the variable changes only when it is true */
        {"program p; var i: Int8 := 0; u: UInt64 := 0; b: Bits8 := 0; f: "
         "Float32 := 0.0; d: Float64 := 0.0; begin\n"
         "writeln(val(\" -128 \", i), \" \", i, \" \", val(\"-129\", i), \" \", i, "
         "\" \", val(\"0x7f\", i), \" \", i);\n"
         "writeln(val(\"18446744073709551615\", u), \" \", u, \" \", val(\"-0\", "
         "u), \" \", u, \" \", val(\"-1\", u), \" \", u);\n"
         "writeln(val(\"255\", b), \" \", b, \" \", val(\"256\", b), \" \", b, \" "
         "\", val(\"1_0\", b), \" \", b, \" \", val(\"1__0\", b), \" \", "
         "val(\"_1\", b), \" \", val(\"1.\", d), \" \", val(\".5\", d));\n"
         "writeln(val(\"1.0000000596046447753906250001\", f), \" \", f, \" \", "
         "val(\"3.5e38\", f), \" \", val(\"1e3\", i), \" \", val(\"1e3\", d), "
         "\" \", d);\n"
         "writeln(val(\"inf\", d), \" \", d, \" \", val(\"-inf\", d), \" \", d, "
         "\" \", val(\"nan\", d), \" \", d, \" \", val(\"-nan\", d), \" \", "
         "val(\"1e400\", d), \" \", val(\"0b101\", d), \" \", d);\nend p.",
         "true -128 false -128 true 127\n"
         "true 18446744073709551615 true 0 false 0\n"
         "true 255 false 255 true 10 false false false false\n"
         "true 1.0000001 false false true 1000.0\n"
         "true inf true -inf true nan false false true 5.0\n",
         "ok"},
        /* low(T), high(T): the first and last value of a discrete type, a
           constant of it */
        {"program p; type C = (r, g, b); S = Int8 range -3..5; var m: Int64 := "
         "low(Int64); begin writeln(low(Int8), \" \", high(UInt64), \" \", "
         "low(S), \" \", high(S), \" \", ord(high(C)), \" \", high(Boolean), "
         "\" \", ord(high(Char)));\nwriteln(m div (-1)); end p.",
         "-128 18446744073709551615 -3 5 2 true 1114111\n", "trap 6 at 2:11"},
        /* arg from 1 to argcount(); a width from 0, decimals from 0 to 100,
           checked where they are written */
        {"program p; begin writeln(argcount());\nwriteln(arg(1)); end p.",
         "0\n", "trap 101 at 2:9"},
        {"program p; var n: Int32 := -1; r: Float64 := 1.5; begin "
         "writeln(r:4:1, r:0);\nwriteln(r:n); end p.",
         " 1.51.5\n", "trap 101 at 2:11"},
        {"program p; var k: Int32 := 101; r: Float64 := 1.5; begin "
         "writeln(r:1:0);\nwriteln(r:3:k); end p.",
         "2\n", "trap 101 at 2:13"},
        /* an open array takes the bounds of its argument (Ada) */
        {"program p; type V = array[Int32 range <>] of Int64; var a: "
         "array[Int32 range 5..9] of Int64; n: Int32 := 3; procedure F(var v: "
         "V); begin writeln(low(v), \" \", high(v), \" \", length(v)); for var "
         "i := low(v) to high(v) do v[i] := Int64(i); end; end F; function "
         "S(v: V): Int64; var s: Int64 := 0; begin for var i := low(v) to "
         "high(v) do s := s + v[i]; end; return s; end S; begin F(a); "
         "writeln(S(a)); var d: array[Int32 range -2..n] of Int64; F(d); "
         "writeln(S(d)); var e: array[Int32 range 4..n] of Int64; F(e); "
         "writeln(S(e), \" \", a[5]); end p.",
         "5 9 5\n35\n-2 3 6\n3\n4 3 0\n0 5\n", "ok"},
        {"program p; type Small = Int32 range 1..10; var n: Int32 := 3; "
         "procedure Only(v: array[Small range <>] of Int8); begin "
         "writeln(low(v)); end Only; begin var e: array[Int32 range 4..n] of "
         "Int8; Only(e); var d: array[Int32 range 0..n] of Int8; Only(d); end "
         "p.",
         "4\n", "trap 101"},
        {"program p; var a: array[Int32 range 0..20] of Int8; procedure Q(v: "
         "array[Int32 range <>] of Int8); begin writeln(v[25]); end Q; begin "
         "Q(a); end p.",
         "", "trap 100"},
        /* no value with a sign fits a range above INT64_MAX */
        {"program p; type R = UInt64 range "
         "18446744073709551610..18446744073709551611; var x: Int8 := 1; begin "
         "writeln(R(x)); end p.",
         "", "trap 103"},
        /* constants are computed exactly, past 64 bits, and checked when
           they take a type */
        {"program p; const k1 = 2 ** 70 div 2 ** 60; k2 = (-7) mod 3; k3 = "
         "(-7) rem 3; k4 = abs (-(2 ** 90)) div (2 ** 89); k5: Int16 = k1 * 3; "
         "k6 = 18446744073709551616 * 4; var x: Int8 := 1; begin const k7 = k6 "
         "mod 97; writeln(k1, \" \", k2, \" \", k3, \" \", k4, \" \", k5, \" \", x "
         "+ k7, \" \", (k6 * k6) mod 100 + x); end p.",
         "1024 2 -1 2 3072 51 97\n", "ok"},
        /* nil is reported where it is gone through: the . or the ^ */
        {"program t; type R = record a: Int32; end; Ptr = ^R; var q: Ptr := "
         "nil; begin\n  writeln(q.a);\nend t.",
         "", "trap 102 at 2:12"},
        {"program t; type R = record a: Int32; end; Ptr = ^R; var q: Ptr := "
         "nil; begin\n  writeln(q^.a);\nend t.",
         "", "trap 102 at 2:12"},
        /* a dangling pointer is checked at the access itself, after all the
           statement evaluates; a second dispose is an error (§ 3.10, § 9.7)
         */
        {"program t; type R = record a: Int32; end; var q: ^R; begin q := "
         "new(R); dispose(q);\n  writeln(q.a);\nend t.",
         "", "trap 105 at 2:12"},
        {"program t; type R = record a: Int32; end; var q: ^R; function g(): "
         "Int32; begin dispose(q); return 7; end g; begin q := new(R);\n  q.a "
         ":= g();\nend t.",
         "", "trap 105 at 2:3"},
        {"program t; type R = record a: Int32; end; var q: ^R; w: R; function "
         "h(): R; begin dispose(q); return w; end h; begin q := new(R);\n  q^ "
         ":= h();\nend t.",
         "", "trap 105 at 2:3"},
        {"program t; type S = record s: String; end; var q: ^S; begin q := "
         "new(S); q.s := \"x\"; dispose(q);\n  q.s := \"y\";\nend t.",
         "", "trap 105 at 2:3"},
        {"program t; type R = record a: Int32; end; var q: ^R; begin q := "
         "new(R); dispose(q);\n  dispose(q);\nend t.",
         "", "trap 106 at 2:3"},
        {"program t; type R = record a: Int32; end; var q, w: ^R; begin q := "
         "nil; dispose(q); q := new(R); w := q; dispose(q); q := new(R); "
         "writeln(w = q, \" \", w = nil); dispose(q); end t.",
         "false false\n", "ok"},
        /* a record reached through a pointer is no argument (§ 3.10): pass
           a copy made in a variable */
        {"program t; type R = record a: Int32; end; var q: ^R; procedure "
         "show(x: R); begin dispose(q); writeln(x.a); end show; begin q := "
         "new(R); q.a := 5; var c := q^; show(c); end t.",
         "5\n", "ok"},
        {"program t; type R = record a: Int32; end; var q: ^R; procedure k(out "
         "x: Int32); begin x := 2; end k; begin q := new(R); k(q.a); "
         "writeln(q.a); dispose(q); end t.",
         "2\n", "ok"},
        {"program t; type R = record a: Int32; end; var q: ^R; procedure k(out "
         "x: Int32); begin dispose(q); x := 2; end k; begin q := new(R);\n  "
         "k(q.a);\nend t.",
         "", "trap 105 at 2:3"},
        {"program t; pragma suppress(dangling_check); type R = record a: Int32; "
         "end; var q: ^R; begin q := new(R); q.a := 3; dispose(q); writeln(q.a); "
         "end t.",
         "3\n", "ok"},
        /* arrays made by new: a vector that grows with move, Strings,
           overlapping moves, bounds, an empty one (§ 3.10, § 9.5, § 9.7) */
        {"program t;\ntype Items = array[Int64 range <>] of Int64; ItemsRef = "
         "^Items; Words = array[Int32 range <>] of String;\nprocedure Push(var "
         "v: ItemsRef; var n: Int64; x: Int64);\nbegin\n  if n = length(v^) "
         "then\n    var bigger := new(Items range 0..2 * length(v^) - 1);\n "
         "   move(v^, 0, bigger^, 0, n);\n    dispose(v);\n    v := "
         "bigger;\n  end;\n  v[n] := x;\n  n := n + 1;\nend Push;\nbegin\n  "
         "var v := new(Items range 0..1);\n  var n: Int64 := 0;\n  for var "
         "i: Int64 := 1 to 20 do Push(v, n, i * i); end;\n  writeln(n, \" \", "
         "low(v^), \" \", high(v^), \" \", length(v^), \" \", v[19]);\n  "
         "move(v^, 0, v^, 1, 5);\n  writeln(v[0], \" \", v[1], \" \", v[2], "
         "\" \", v[5]);\n  move(v^, 3, v^, 0, 4);\n  writeln(v[0], \" \", "
         "v[3]);\n  dispose(v);\n  var w := new(Words range 5..7);\n  w[5] := "
         "\"a\" & str(1);\n  w[6] := w[5] & \"b\";\n  var u := new(Words "
         "range 1..3);\n  move(w^, 5, u^, 1, 3);\n  writeln(u[1], \" \", "
         "u[2], \" [\", u[3], \"]\");\n  dispose(w);\n  writeln(u[2]);\n  "
         "dispose(u);\n  var e := new(Items range 1..0);\n  "
         "writeln(length(e^));\n  dispose(e);\nend t.",
         "20 0 31 32 400\n1 1 4 25\n9 49\na1 a1b []\na1b\n0\n", "ok"},
        {"program t; type A = array[Int32 range <>] of Int32; begin var p := "
         "new(A range 1..3);\n  p[4] := 1;\nend t.",
         "", "trap 100 at 2:4"},
        {"program t; type A = array[Int32 range <>] of Int32; begin var p := "
         "new(A range 1..3); dispose(p);\n  writeln(p[1]);\nend t.",
         "", "trap 105 at 2:12"},
        {"program t; type A = array[Int32 range <>] of Int32; begin var p := "
         "new(A range 1..3); dispose(p);\n  dispose(p);\nend t.",
         "", "trap 106 at 2:3"},
        {"program t; type A = array[Int32 range <>] of Int32; S = Int32 range "
         "1..10; B = array[S range <>] of Int32; begin var n: Int32 := 11;\n  "
         "var p := new(B range 1..n);\nend t.",
         "", "trap 101 at 2:12"},
        {"program t; type A = array[Int32 range <>] of Int32; begin var n: "
         "Int32 := 2147483647;\n  var p := new(A range -n..n);\nend t.",
         "", "trap 101 at 2:12"},
        {"program t; type A = array[Int64 range <>] of Int64; begin var n: "
         "Int64 := 4611686018427387904;\n  var p := new(A range 0..n);\nend "
         "t.",
         "", "trap 7 at 2:12"},
        {"program t; type A = array[Int32 range <>] of Int32; begin var p := "
         "new(A range 1..3); var c: Int32 := -1;\n  move(p^, 1, p^, 1, "
         "c);\nend t.",
         "", "trap 101 at 2:3"},
        {"program t; type A = array[Int32 range <>] of Int32; begin var p := "
         "new(A range 1..3);\n  move(p^, 2, p^, 1, 3);\nend t.",
         "", "trap 100 at 2:3"},
        {"program t; type A = array[Int32 range <>] of Int32; begin var p := "
         "new(A range 1..3); var q := p; dispose(p);\n  move(q^, 1, q^, 2, "
         "0);\nend t.",
         "", "trap 105 at 2:3"},
        /* translate, reverse, occurrences (§ 9.5): a complement and its
           reverse, counts without overlaps, empty tracts that check no
           bound, a table translated through itself (read whole first),
           records with Strings reversed (counted as they were) */
        {"program t;\ntype Rec = record k: Int32; w: String; end;\nvar "
         "tbl: array[Byte] of Byte;\n  a: array[Int32 range 1..8] of "
         "Byte;\n  rs: array[Int32 range 1..3] of Rec;\nbegin\n  var s: "
         "String := \"GATTACAA\";\n  for var i := low(tbl) to high(tbl) do "
         "tbl[i] := i; end;\n  tbl[65] := 84; tbl[84] := 65; tbl[67] := 71; "
         "tbl[71] := 67;\n  for var i: Int32 := 1 to 8 do a[i] := "
         "s[Int64(i)]; end;\n  translate(a, 1, 8, tbl);\n  for var i: Int32 "
         ":= 1 to 8 do writebyte(a[i]); end;\n  writeln(\"\");\n  reverse(a, "
         "1, 8);\n  for var i: Int32 := 1 to 8 do writebyte(a[i]); end;\n  "
         "writeln(\"\");\n  writeln(occurrences(a, 1, 8, \"TT\"), \" \", "
         "occurrences(s, 1, 8, \"A\"), \" \", occurrences(\"aaaa\", 1, 4, "
         "\"aa\"), \" \", occurrences(a, 9, 0, \"x\"));\n  reverse(a, 30, "
         "0);\n  translate(a, -4, 0, tbl);\n  tbl[0] := 1; tbl[1] := 0;\n  "
         "translate(tbl, 0, 2, tbl);\n  writeln(tbl[0], \" \", tbl[1]);\n  "
         "for var i: Int32 := 1 to 3 do rs[i].k := i; rs[i].w := \"w\" & "
         "str(i); end;\n  reverse(rs, 1, 3);\n  writeln(rs[1].k, rs[1].w, "
         "\" \", rs[3].k, rs[3].w);\nend t.",
         "CTAATGTT\nTTGTAATC\n1 4 2 0\n0 1\n3w3 1w1\n", "ok"},
        {"program t; var a: array[Int32 range 1..8] of Byte; begin var c: "
         "Int32 := -1;\n  reverse(a, 1, c);\nend t.",
         "", "trap 101 at 2:3"},
        {"program t; var a: array[Int32 range 1..8] of Byte; tbl: "
         "array[Byte] of Byte; begin\n  translate(a, 7, 3, tbl);\nend t.",
         "", "trap 100 at 2:3"},
        {"program t; var a: array[Int32 range 1..8] of Byte; begin\n  "
         "writeln(occurrences(a, 1, 8, \"\"));\nend t.",
         "", "trap 101 at 2:11"},
        {"program t; var a: array[Int32 range 1..8] of Byte; begin\n  "
         "writeln(occurrences(a, 1, 0, \"\"));\nend t.",
         "", "trap 101 at 2:11"},
        {"program t; type Bs = array[Int32 range <>] of Byte; begin var p := "
         "new(Bs range 1..3); dispose(p);\n  reverse(p^, 1, 1);\nend t.",
         "", "trap 105 at 2:3"},
        {"program t; type Bs = array[Int32 range <>] of Byte; begin var p := "
         "new(Bs range 1..3); var q := new(Bs range 1..1); dispose(q);\n  "
         "writeln(occurrences(p^, 1, 3, q^));\nend t.",
         "", "trap 105 at 2:11"},
        /* the destination out of its bounds; a dispose between the bounds
           and the copy; the bounds of a freed array are not read */
        {"program t; type A = array[Int32 range <>] of Int32; begin var p := "
         "new(A range 1..3);\n  move(p^, 1, p^, 2, 3);\nend t.",
         "", "trap 100 at 2:3"},
        {"program t; type A = array[Int32 range <>] of Int32; var q: ^A; "
         "function f(): Int32; begin dispose(q); return 1; end f; begin q := "
         "new(A range 1..3);\n  move(q^, 1, q^, 1, f());\nend t.",
         "", "trap 105 at 2:3"},
        {"program t; type A = array[Int32 range <>] of Int32; begin var p := "
         "new(A range 1..3); dispose(p);\n  writeln(p[99]);\nend t.",
         "", "trap 105 at 2:12"},
        /* licm keeps what the program sees: a dispose in the loop is seen
           at the next access; a write before a dangling access comes first
         */
        {"program t; type A = array[Int32 range <>] of Int32; begin var p := "
         "new(A range 1..3); var q := new(A range 1..3); p[1] := 7;\n  for var "
         "i: Int32 := 1 to 3 do if i = 2 then dispose(q); dispose(p); end; "
         "writeln(p[1]); end;\nend t.",
         "7\n", "trap 105 at 2:85"},
        {"program t; type A = array[Int32 range <>] of Int32; begin var p := "
         "new(A range 1..3); dispose(p); var i: Int32 := 0;\n  while i < 2 do "
         "writeln(i); i := p[1]; end;\nend t.",
         "0\n", "trap 105 at 2:36"},
        {"program t; type A = array[Int32 range <>] of Int32; begin var p := "
         "new(A range 1..3); dispose(p); var s: Int32 := 0; var n: Int32 := 0; "
         "for var i: Int32 := 1 to n do s := s + p[i]; end; writeln(s);\nend "
         "t.",
         "0\n", "ok"},
        /* calls without end: stack overflow, never a crash (§ 10.1) */
        {"program t; function f(n: Int64): Int64; begin return f(n + 1) + 1; "
         "end f; begin\n  writeln(f(0));\nend t.",
         "", "trap 28 at 1:54"},
        /* Strings in records, arrays and new: copied, returned, passed,
           disposed, in a loop and in a computed array, counted right */
        {"program t; type S = record n: Int32; s: String; end; var a: "
         "array[Int32 range 1..3] of S; g: S; q: ^S; function mk(k: Int32): S; "
         "var r: S; begin r.n := k; r.s := \"k\" & str(k); return r; end mk; "
         "procedure show(x: S); begin writeln(x.n, \" \", x.s); end show; begin "
         "for var i: Int32 := 1 to 3 do a[i] := mk(i); var l: S; l.s := l.s & "
         "\"+\"; write(l.s); end; writeln(); g := a[2]; a[2] := a[2]; show(g); "
         "q := new(S); q^ := a[3]; var c := q^; show(c); dispose(q); var n: Int32 := 2; var "
         "d: array[Int32 range 1..n] of S; d[1] := a[1]; show(d[1]); end t.",
         "+++\n2 k2\n3 k3\n1 k1\n", "ok"},
        /* a record assigned is copied, from the right to the left */
        {"program t; type Pair = record a, b: Int32; end; var u, w: Pair; begin "
         "u.a := 1; u.b := 2; w := u; w.a := 5; writeln(u.a, \" \", w.a, \" \", "
         "w.b); end t.",
         "1 5 2\n", "ok"},
        /* new gives values outside a narrow range, caught when read through
           a pointer; a copy carries them, and its reads are checked too
           (§ 4.5) */
        {"program n; type Small = Int32 range 4..9; Tiny = UInt8 range 0..3; "
         "Rec = record a: Small; b: Int32; c: Tiny; v: array[Int32 range 1..5] "
         "of Small; end; Ptr = ^Rec; var p: Ptr := nil; r: Rec; begin p := "
         "new(Rec); writeln(p.b); p.a := 5; writeln(p.a); r := p^; "
         "writeln(r.c, \" \", r.v[3]);\n  writeln(p.c); end n.",
         "0\n5\n", "trap 101 at 1:268"},
        {"program n; type Small = Int32 range 4..9; Rec = record v: "
         "array[Int32 range 1..5] of Small; end; Ptr = ^Rec; var p: Ptr := nil; "
         "begin p := new(Rec); p.v[2] := 7; writeln(p.v[2]);\n  "
         "writeln(p.v[5]); end n.",
         "7\n", "trap 101 at 2:14"},
        {"program n; type Small = Int32 range 4..9; SP = ^Small; var q: SP := "
         "nil; begin q := new(Small);\n  writeln(q^); end n.",
         "", "trap 101 at 2:12"},
        /* ** at run time: checked on numbers, modular on Bits, any unsigned
           exponent */
        {"program w; var a: UInt8 := 3; b: Bits8 := 3; c: Int64 := 1; d: Bits8 "
         ":= 2; n: UInt64 := 18446744073709551615; begin writeln(a ** 4, \" \", b "
         "** 7, \" \", c ** n, \" \", d ** n); end w.",
         "81 139 1 0\n", "ok"},
        {"program w; var a: Int8 := 3; n: Int32 := 5; begin writeln(a ** 4); "
         "writeln(a ** n); end w.",
         "81\n", "trap 6"},
        {"program w; var a: UInt32 := 2; n: Int32 := 32; begin writeln(a ** n); "
         "end w.",
         "", "trap 6"},
        /* reals: abs clears the sign (IEEE 754), a constant -0.0 is the
           rational 0, a conversion fits by the exact bounds */
        {"program f; var z: Float64 := 0.0; begin writeln(abs (-(z)), \" \", "
         "-(z), \" \", -0.0); end f.",
         "0.0 -0.0 0.0\n", "ok"},
        {"program f; type R = Int64 range 0..9007199254740993; var a: Float64 := "
         "9007199254740992.0; b: Float64 := 9007199254740994.0; begin "
         "writeln(R(a)); writeln(R(b)); end f.",
         "9007199254740992\n", "trap 103"},
        /* a negative exponent is a range error, the exponent a Natural */
        {"program w; var a: Int64 := 1; e: Int32 := -1; begin writeln(a ** 0); "
         "writeln(a ** e); end w.",
         "1\n", "trap 101"},
        /* succ and pred are checked, before the step (pred of the first
           gave 255) */
        {"program e; type Color = (Red, Green, Blue); var c: Color := Red; "
         "begin writeln(ord(succ(c))); writeln(ord(pred(c))); end e.",
         "1\n", "trap 101"},
        {"program e; var a: UInt64 := 18446744073709551615; b: Int8 := -128; "
         "begin writeln(pred(a), \" \", succ(b)); writeln(succ(a)); end e.",
         "18446744073709551614 -127\n", "trap 101"},
        /* out: copied back at the return, given a value on every path */
        {"program o; var g: Int32 := 0; procedure Set(var x: Int32); begin x "
         ":= 7; end Set; procedure P(out r: Int32); begin r := 1; writeln(g); "
         "Set(r); writeln(g, \" \", r); end P; begin P(g); writeln(g); end o.",
         "0\n0 7\n7\n", "ok"},
        {"program o; procedure Get(out r: Int32; x: Int32); begin if x > 0 then "
         "r := x; end; end Get; var v: Int32 := 0; begin Get(v, 1); end o.",
         "", "errors L0056@1:22"},
        {"program o; procedure Get(out r: Int32); begin writeln(r); r := 1; end "
         "Get; var v: Int32 := 0; begin Get(v); end o.",
         "", "errors L0053@1:55"},
        /* the target of an assignment before its value */
        {"program p;\nvar a: array[Int32 range 1..3] of Int32; z: Int32 := "
         "0;\nbegin\n  a[1 div z] := 7 div z;\nend p.",
         "", "trap 11 at 4:7"},
        {"program p; var a: array[Int32 range 1..3] of Int32; function F(): "
         "Int32; begin writeln(\"value\"); return 1; end F; function G(): "
         "Int32; begin writeln(\"index\"); return 1; end G; begin a[G()] := "
         "F(); end p.",
         "index\nvalue\n", "ok"},
        /* abs of a number without a sign is the number (found by the random
           programs: it was taken as signed) */
        {"program p; var u: UInt16 := 65534; b: UInt8 := 200; begin "
         "writeln(abs (u), \" \", abs (b)); end.",
         "65534 200\n", "ok"},
        /* checked on the SSA form */
        {"program p; var x: Int32; begin var y: Int32; writeln(y); end.", "",
         "errors L0053@1:54"},
        {"program p; var b: Boolean := true; begin var y: Int32; if b then y := "
         "1; end; writeln(y); end.",
         "", "errors L0053@1:87"},
        {"program p; var b: Boolean := true; begin var y: Int32; if b then y := "
         "1; else y := 2; end; writeln(y); end.",
         "1\n", "ok"},
        {"program p; function f(x: Int32): Int32; begin if x > 0 then return 1; "
         "end; end; begin writeln(f(1)); end.",
         "", "errors L0052@1:21"},
        /* records and arrays as results: into a slot of the caller */
        {"program p;\ntype V = record x, y: Int32; end;\n"
         "type A = array[Int32 range 1..4] of Int64;\n"
         "function mk(a, b: Int32): V; var r: V; begin r.x := a; r.y := b; "
         "return r; end mk;\n"
         "function sq(n: Int64): A; var r: A;\nbegin\n"
         "  for var i: Int32 := 1 to 4 do r[i] := n * Int64(i); end;\n"
         "  if n > 100 then return sq(n div 2); end;\n  return r;\nend sq;\n"
         "function sw(v: V): V; begin return mk(v.y, v.x); end sw;\n"
         "var g: V; h: A;\nbegin\n  g := sw(mk(3, 4));\n"
         "  writeln(g.x, \" \", g.y, \" \", mk(7, 8).y, \" \", sq(5)[3], \" \", "
         "sq(300)[4]);\n"
         "  h := sq(2); var w := sw(sw(mk(9, 10)));\n"
         "  writeln(h[1] + h[4], \" \", w.x);\nend.",
         "4 3 8 15 300\n10 9\n", "ok"},
        {"program p; type V = record x: Int32; end; function f(b: Boolean): V; "
         "var r: V; begin if b then return r; end; end; begin end.",
         "", "errors L0052@1:52"},
        /* computed arrays: freed at the end of their list, and on exit,
           continue and return */
        {"program p;\nfunction f(k: Int32): Int64;\n"
         "var a: array[Int32 range 1..k] of Int64;\nbegin\n  a[k] := 7;\n"
         "  for var i := 1 to k do\n"
         "    var b: array[Int32 range 0..i] of Int64;\n"
         "    if i = 2 then continue; end;\n"
         "    var c: array[Int32 range 0..i] of Int64;\n"
         "    exit when i = 4;\n"
         "    loop var d: array[Int32 range 0..i] of Int8; exit; end;\n"
         "    continue when i = 1;\n"
         "    if i = 5 then return a[k]; end;\n  end;\n"
         "  return a[k] + 1;\nend f;\n"
         "begin\n  writeln(f(3), \" \", f(6));\n"
         "  for var j: Int32 := 1 to 3 do var e: array[Int32 range 1..j] of "
         "Int32; e[j] := j; write(e[j]); end;\n  writeln();\nend.",
         "8 8\n123\n", "ok"},
        {"program p; type R = record a: Int32; end; var q: ^R; begin q := "
         "new(R); q.a := 1; end.",
         "", "ok, 1 live"},
        /* an integer to Float32 rounded once: through a double, 2^60 + 2^36
           + 1 would lose its last bit and tie down to 2^60 */
        {"program p; var i: Int64 := 1152921573326323713; u: UInt64 := "
         "9223372586610589697; begin writeln(Float32(i), \" \", Float32(u)); "
         "end.",
         "1.1529216e+18 9.223373e+18\n", "ok"},
        /* chr is checked at its name, s[i] at the [ */
        {"program p; var n: Int32 := 1114112; begin writeln(chr(n)); end.", "",
         "trap 103 at 1:51"},
        {"program p; var s: String := \"ab\"; i: Int64 := 3; begin "
         "writeln(s[i]); end.",
         "", "trap 100 at 1:65"},
        /* copy: from 1 on, a count from 0, cut short past the end; its
           arguments from left to right */
        {"program p; var s: String := \"abc\"; begin writeln(copy(s, 3, 5), "
         "\"|\", copy(s, 5, 1), \"|\", copy(s, 1, 0), \"|\", copy(s, 2, 2)); "
         "end.",
         "c|||bc\n", "ok"},
        {"program p; var s: String := \"abc\"; k: Int64 := 0; begin "
         "writeln(copy(s, k, 1)); end.",
         "", "trap 101 at 1:65"},
        {"program p; var s: String := \"abc\"; k: Int64 := -1; begin "
         "writeln(copy(s, 1, k)); end.",
         "", "trap 101 at 1:66"},
        {"program p; var z: Int64 := 0; m: Int64 := 9223372036854775807; begin "
         "writeln(copy(\"ab\", 1 div z, m + 1)); end.",
         "", "trap 11"},
        /* a record or an array variable without a value (§ 4.5): its narrow
           scalars are not valid, even where 0 would be, and a read of one
           is caught at the . or the [ */
        {"program p; type S = Int32 range -5..5; R = record a: Int32; b: S; "
         "end; var g: R; begin writeln(g.a); writeln(g.b); end.",
         "0\n", "trap 101 at 1:111"},
        {"program p; type S = Int32 range -5..5; procedure q(); var a: "
         "array[Int32 range 1..3] of S; begin a[2] := 0; writeln(a[2]); "
         "writeln(a[3]); end q; begin q(); end.",
         "0\n", "trap 101 at 1:133"},
        {"program p; type S = Int32 range -5..5; procedure q(n: Int32); var a: "
         "array[Int32 range 1..n] of S; begin a[1] := 1; writeln(a[1]); "
         "writeln(a[n]); end q; begin q(1); q(9); end.",
         "1\n1\n1\n", "trap 101 at 1:141"},
        /* a for over the bounds of x leaves out the checks of x[i] only
           when x keeps its bounds: a string assigned in the loop, a global
           one a routine may assign, one passed as var, another array, i - 1
           as the low bound keep them */
        {"program p; procedure q(); var s: String := \"abc\"; begin for var i: Int64 := 1 to length(s) do s := copy(s, 1, 1); writeln(s[i]); end; end q; begin q(); end.",
         "97\n", "trap 100 at 1:124"},
        {"program p; var g: String := \"abc\"; procedure cut(); begin g := \"a\"; end cut; begin for var i: Int64 := 1 to length(g) do cut(); writeln(g[i]); end; end.",
         "97\n", "trap 100 at 1:138"},
        {"program p; procedure cut(var t: String); begin t := \"a\"; end cut; procedure q(); var s: String := \"abc\"; begin for var i: Int64 := 1 to length(s) do cut(s); writeln(s[i]); end; end q; begin q(); end.",
         "97\n", "trap 100 at 1:167"},
        {"program p; var a: array[Int32 range 1..3] of Int32; b: array[Int32 range 1..2] of Int32; begin for var i := low(a) to high(a) do writeln(b[i]); end; end.",
         "0\n0\n", "trap 100 at 1:139"},
        {"program p; var a: array[Int32 range 1..3] of Int32; begin for var i := low(a) to high(a) do for var j := i - 1 to high(a) do writeln(a[j]); end; end; end.",
         "", "trap 100 at 1:135"},
        /* checks turned off (§ 9): in the whole file, in a routine, to the
           end of a list of statements; unsuppress turns them back on. Where
           one would fail the behaviour is undefined: here the value goes on
         */
        {"program p; pragma suppress(range_check); type P = Int32 range 0..100; var x: P; y: Int32 := 200; begin x := y; writeln(x); end.",
         "200\n", "ok"},
        {"program p; type P = Int32 range 0..100; var x: P; y: Int32 := 200; procedure q(); pragma suppress(range_check); var z: P; begin z := y; writeln(z); end q; begin q(); x := y; end.",
         "200\n", "trap 101 at 1:167"},
        {"program p; type P = Int32 range 0..100; var x: P; y: Int32 := 200; begin for var i: Int32 := 1 to 2 do pragma suppress(range_check); x := y; writeln(x); end; x := y; end.",
         "200\n200\n", "trap 101 at 1:159"},
        {"program p; pragma suppress(all_checks); type P = Int32 range 0..100; var x: P; y: Int32 := 200; a: Int8 := 100; begin a := a + a; writeln(a); if y > 0 then pragma unsuppress(range_check); x := y; end; end.",
         "-56\n", "trap 101 at 1:189"},
        /* a NaN is nan with decimals too, whatever its sign bit (0 / 0 sets
           it on x86) */
        {"program p; var z: Float64 := 0.0; begin writeln((z / z):0:2, \" \", "
         "(-(z / z)):5:1, \"|\"); end.",
         "nan   nan|\n", "ok"},
        /* BigInt: rounding to reals once and straight to the type, from
           reals half away from zero, conversions to integers at their
           bounds, comparison by value; in records, arrays, new, parameters;
           val and str; powers of 0, 1, -1; the signs of div, rem, mod; the
           traps */

        {"program p;\nvar b: BigInt := 9007199254740993; c: BigInt := 1152921573326323713;\n  d: BigInt := 340282356779733661637539395458142568448; z: Float64 := 0.0;\n  two: BigInt := 2;\nbegin\n  writeln(Float64(b), \" \", Float64(b + 2), \" \", Float32(BigInt(16777219) + b - b));\n  writeln(Float32(c), \" \", Float32(d), \" \", Float32(d - 1), \" \", Float64(-d * d * d * d * d * d * d * d * d));\n  writeln(BigInt(z + 0.5), \" \", BigInt(z - 1.5), \" \", BigInt(z + 2.5), \" \", BigInt(z + 1e20));\n  writeln(Int64(-two ** 63), \" \", UInt64(two ** 64 - 1), \" \", Bits8(-b), \" \", Bits64(-b), \" \", Int8(-two ** 7), \" \", Int16(-b div 1000000000000));\n  writeln(two ** 64 = BigInt(18446744073709551615) + 1, \" \", b <> b + 0, \" \", -b < b, \" \", -b < -two, \" \", two < -b);\nend.",
         "9007199254740992.0 9007199254740996.0 16777220.0\n1.1529216e+18 inf 3.4028235e+38 -inf\n1 -2 3 100000000000000000000\n-9223372036854775808 18446744073709551615 255 18437736874454810623 -128 -9007\ntrue false true true false\n",
         "ok"},
        {"program p;\ntype Rec = record n: BigInt; s: String; end;\n  Arr = array[Int32 range 1..2] of BigInt;\n  Open = array[Int32 range <>] of BigInt;\nvar r: Rec; a: Arr; p: ^Rec; q: ^Open;\nprocedure Twice(var x: BigInt; y: BigInt; out z: BigInt);\nbegin\n  x := x * 2; z := y + x;\nend Twice;\nbegin\n  r.n := 10 ** 25; a[2] := r.n; r.n := r.n + 1;\n  p := new(Rec); p.n := a[2] * a[2]; p.s := str(p.n);\n  writeln(r.n, \" \", a[1], \" \", a[2], \" \", p.s);\n  dispose(p);\n  q := new(Open range 1..3);\n  q[3] := -a[2]; writeln(q[1], \" \", q[3]);\n  dispose(q);\n  var x: BigInt := 3; var z: BigInt;\n  Twice(x, BigInt(4), z);\n  writeln(x, \" \", z, \"|\", x:5, \"|\", str(z * (-1)));\n  var v: BigInt := 7;\n  writeln(val(\"  +1_000_000_000_000_000_000_000 \", v), \" \", v, \" \", val(\"12.0\", v), \" \", v);\nend.",
         "10000000000000000000000001 0 10000000000000000000000000 100000000000000000000000000000000000000000000000000\n0 -10000000000000000000000000\n6 10|    6|-10\ntrue 1000000000000000000000 false 1000000000000000000000\n",
         "ok"},
        {"program p;\nvar n: Int32 := -1; e: UInt64 := 18446744073709551615; b: BigInt := 5;\nbegin\n  writeln(BigInt(-1) ** e, \" \", BigInt(0) ** e, \" \", BigInt(1) ** e, \" \", BigInt(0) ** 0, \" \", b ** 3);\n  writeln(b ** n);\nend.",
         "-1 0 1 1 125\n", "trap 101 at 5:13"},
        {"program p;\nvar b: BigInt := 7; z: BigInt := 0;\nbegin\n  writeln(b div 2, \" \", b rem (-2), \" \", b mod (-2), \" \", (-b) mod 2);\n  writeln(b div z);\nend.",
         "3 1 -1 1\n", "trap 11 at 5:13"},
        {"program p; var z: Float64 := 0.0; begin writeln(BigInt(z / z)); end.",
         "", "trap 103 at 1:49"},
        {"program p; var b: BigInt := 2; begin writeln(Int64(b ** 63 - 1)); writeln(Int64(b ** 63)); end.",
         "9223372036854775807\n", "trap 103 at 1:75"},
        /* C strings (§ 9.9): made, read, read n bytes, read from a buffer,
       freed; CBool and the C types by name; a 0 inside, nil, a negative
       count, a count past the buffer */
        {"program p;\nvar s: CString; t: CString; buf: array[Int32 range 1..4] of Byte; q: CPointer; b: CBool;\nbegin\n  writeln(s = nil, \" \", q = nil);\n  s := newcstring(\"ciao, mondo\");\n  writeln(cvalue(s), \"|\", cvalue(s, 4), \"|\", length(cvalue(s, 0)));\n  t := s;\n  freecstring(s);\n  writeln(s = nil, \" \", t = nil);\n  buf[1] := 104; buf[2] := 105; buf[3] := 0; buf[4] := 33;\n  writeln(cvalue(buf, 2), \"|\", length(cvalue(buf, 4)));\n  b := CBool(true);\n  writeln(Boolean(b), \" \", not b, \" \", CInt(7) * CInt(6), \" \", CLong(3) + 4);\nend.",
         "true true\nciao, mondo|ciao|0\ntrue false\nhi|4\ntrue false 42 7\n",
         "ok"},
        {"program p; var s: CString; begin s := newcstring(\"a\" & chr(0) & \"b\"); end.",
         "", "trap 101 at 1:39"},
        {"program p; var s: CString; begin writeln(cvalue(s)); end.", "",
         "trap 102 at 1:42"},
        {"program p; var s: CString; n: Int64 := -1; begin s := newcstring(\"x\"); writeln(cvalue(s, n)); end.",
         "", "trap 101 at 1:80"},
        {"program p; var buf: array[Int32 range 1..4] of Byte; n: Int64 := 5; begin writeln(cvalue(buf, n)); end.",
         "", "trap 100 at 1:83"},
        /* a width counts characters, not bytes */
        {"program p; var s: String := \"\xc3\xa8\xe2\x82\xac\"; begin writeln(s:5, "
         "\"|\", 'x':3, \"|\"); end.",
         "   \xc3\xa8\xe2\x82\xac|  x|\n", "ok"},
        /* short routines put in line at -O1 (OPTDIFF): a local array
           starts from zero at each call, a trap is where it is in the
           routine, halt stops there, strings go in and out */
        {"program p; function f(k: Int32): Int32; var a: array[Int32 range "
         "1..2] of Int32; begin a[1] := a[1] + k; return a[1]; end; begin for "
         "var i: Int32 := 1 to 3 do write(f(i)); end; writeln(); end.",
         "123\n", "ok"},
        {"program p; function g(x: Int32): Int32; begin return 12 div x; end; "
         "begin for var i: Int32 := 2 downto 0 do write(g(i)); end; end.",
         "612", "trap 11 at 1:57"},
        {"program p; procedure h(x: Int32); begin if x = 2 then halt(3); end; "
         "write(x); end; begin for var i: Int32 := 0 to 5 do h(i); end; end.",
         "01", "halt 3"},
        {"program p; var t: String := \"\"; function tag(s: String; n: Int32): "
         "String; begin return s & str(n); end; begin for var i: Int32 := 1 to "
         "3 do t := t & tag(\"a\", i); end; writeln(t); end.",
         "a1a2a3\n", "ok"},
};

/* an input that cannot be read: a directory (EISDIR) */
static const char unreadable[] = "";

/* run main on the input in (NULL: none; unreadable: an error in reading);
   what it printed (malloc'd, *len bytes) and how it ended */
static char *run_module(limba_module *m, const char *in, size_t inlen, int argc,
                        char **argv, char *end, size_t size, size_t *len)
{
    limba_eval_limits lim = {0, 0, argc, argv, NULL, true, 0};
    if (in == unreadable)
        lim.in = fopen("tests", "r");
    else if (in && inlen)
        lim.in = fmemopen((void *)in, inlen, "r");
    limba_eval_result r;
    limba_eval(m, "main", &lim, &r);
    if (lim.in)
        fclose(lim.in);
    snprintf(end, size, "ok");
    if (r.status == LIMBA_EVAL_TRAP)
        snprintf(end, size, "trap %lld", (long long)r.code);
    else if (r.status == LIMBA_EVAL_HALT)
        snprintf(end, size, "halt %lld", (long long)r.code);
    else if (r.status != LIMBA_EVAL_OK)
        snprintf(end, size, "status %d", r.status);
    else if (r.live)
        snprintf(end, size, "ok, %zu live", r.live);
    if (r.pos && r.pos <= m->npos) {
        size_t n = strlen(end);
        snprintf(end + n, size - n, " at %u:%u", m->pos[r.pos - 1].line,
                 m->pos[r.pos - 1].col);
    }
    char *out = r.out;
    *len = out ? r.outlen : 0;
    r.out = NULL;
    limba_eval_result_free(&r);
    return out;
}

/* each function optimised as the front end completes it, as limba -O1
   does: the last edit of its SSA and those of the passes applied once */
typedef struct {
    limba_optimizer *z;
    bool bad;
    limba_diag d;
} optctx;

static bool opt_func(void *ctx, limba_module *m, limba_id fid, limba_edit *e)
{
    optctx *c = ctx;
    if (limba_optimizer_func_edit(c->z, m, fid, e, &c->d) != 0)
        c->bad = true;
    return !c->bad;
}

/* compile a Luxia source to a verified module and run it, then compile it
   again optimising each function as it is made (as limba -O1) and run
   that: both runs must agree (the OPTDIFF net); the output
   may hold any byte, so its length goes to *len when len is not NULL;
   both runs read the input in (NULL: none), with the command line
   argv */
static char *compile_run(const char *src, const char *in, size_t inlen,
                         int argc, char **argv, char *end, size_t size,
                         limba_report *keep, size_t *len)
{
    size_t n1 = 0, n2 = 0;
    limba_source s;
    limba_source_init(&s);
    uint32_t f = limba_source_add(&s, "t.luxia", src, strlen(src));
    limba_report rep;
    limba_report_init(&rep, &s, 'L', 0);
    limba_lx lx;
    limba_lx_init(&lx);
    limba_lx_run(&lx, &s, f, &rep);
    limba_lx_ast t;
    limba_lx_ast_init(&t);
    limba_lx_parse(&t, &lx, &s, &rep);
    limba_lxs sema;
    limba_lxs_init(&sema, &t, &lx, &s, &rep);
    if (rep.errors == 0)
        limba_lxs_check(&sema);
    limba_module *m = rep.errors ? NULL : limba_lxl_program(&sema);
    char *out = NULL;
    if (!m) {
        size_t n = (size_t)snprintf(end, size, "errors");
        for (uint32_t k = 0; k < rep.count && n < size; k++) {
            limba_where w;
            if (!limba_source_where(&s, rep.item[k].loc, &w))
                w.line = w.col = 0;
            n += (size_t)snprintf(end + n, size - n, " L%04u@%u:%u",
                                  rep.item[k].code, w.line, w.col);
        }
    } else {
        limba_diag d = {{0}, 0};
        if (limba_verify(m, &d) != 0) {
            snprintf(end, size, "invalid IR: %.200s", d.msg);
        } else {
            out = run_module(m, in, inlen, argc, argv, end, size, &n1);
            char end2[256];
            /* bounds too, which is off by default */
            limba_opt_options on = {false, NULL, NULL, "bounds"};
            optctx oc = {limba_optimizer_new(&on), false, {{0}, 0}};
            limba_module *m2 = limba_lxl_program_each(&sema, opt_func, &oc);
            limba_optimizer_free(oc.z);
            if (!m2 || oc.bad || limba_verify(m2, &d) != 0) {
                snprintf(end, size, "optimiser: %.200s",
                         oc.bad ? oc.d.msg
                         : m2   ? d.msg
                                : "no module");
            } else {
                char *out2 = run_module(m2, in, inlen, argc, argv, end2,
                                        sizeof(end2), &n2);
                if (n1 != n2 || (n1 && memcmp(out, out2, n1)) ||
                    strcmp(end, end2)) {
                    char first[128];
                    snprintf(first, sizeof(first), "%s", end);
                    snprintf(end, size, "OPTDIFF: %.100s / %.100s", first,
                             end2);
                }
                free(out2);
            }
            if (m2)
                limba_module_free(m2);
        }
        limba_module_free(m);
    }
    if (keep && getenv("LIMBA_TEST_VERBOSE"))
        limba_report_print(&rep, stderr);
    limba_lxs_free(&sema);
    limba_lx_ast_free(&t);
    limba_lx_free(&lx);
    limba_report_free(&rep);
    limba_source_free(&s);
    if (len)
        *len = n1;
    return out;
}

static int test_run(void)
{
    int failures = 0;
    for (size_t i = 0; i < sizeof(run_cases) / sizeof(run_cases[0]); i++) {
        const run_case *c = &run_cases[i];
        char end[256];
        limba_report dummy;
        char *out = compile_run(c->src, NULL, 0, 0, NULL, end, sizeof(end),
                                &dummy, NULL);
        const char *o = out ? out : "";
        /* the place of a trap is compared only when the case gives it */
        char *at = strstr(end, " at ");
        if (at && !strstr(c->end, " at "))
            *at = 0;
        if (strcmp(o, c->out) || strcmp(end, c->end)) {
            fprintf(stderr,
                    "test_luxia: run case %zu\n  printed  \"%s\"\n  expected "
                    "\"%s\"\n  ended    %s\n  expected %s\n",
                    i, o, c->out, end, c->end);
            failures++;
        }
        free(out);
    }
    return failures;
}

/* programs that read an input: the source, the input, what they print and
   how they end, as run_cases. readbytes and writebytes (§ 9.1, § 9.2):
   one input for readline and readbytes (a block that ends between CR and
   LF leaves an empty line), read up to its end, which is final; count 0
   reads nothing and checks no bound; one output; the checks of move; 141
   kept for a closed output; an error in reading is the trap IO where the
   read is, never the end */
static const struct {
    const char *src, *in, *out, *end;
} io_cases[] = {
    {"program t;\nvar a: array[Int32 range 1..8] of Byte;\n  s: String;\n  n: Int32;\n  b: Boolean;\nbegin\n  n := readbytes(a, 1, 3);\n  b := readline(s);\n  writeln(n, \" \", b, \" [\", s, \"]\");\n  writebytes(a, 1, 2);\n  writeln();\n  n := readbytes(a, 99, 0);\n  b := readline(s);\n  writeln(s, \" \", b, \" \", n);\n  n := readbytes(a, 2, 7);\n  write(n, \" \");\n  writebytes(a, 2, n);\n  writeln();\n  n := readbytes(a, 1, 8);\n  b := readline(s);\n  writeln(n, \" \", b, \" [\", s, \"]\");\n  writebytes(\"xyz\", 2, 2);\n  writebytes(s, 5, 0);\n  writeln();\nend t.",
     "ab\r\ncd\nefgh", "3 true []\nab\ncd true 0\n4 efgh\n0 false []\nyz\n", "ok"},
    {"program t; var a: array[Int32 range 1..8] of Byte; n: Int32; begin var c: Int32 := -1;\n  n := readbytes(a, 1, c);\nend t.",
     "x", "", "trap 101 at 2:8"},
    {"program t; var a: array[Int32 range 1..8] of Byte; n: Int32; begin\n  n := readbytes(a, 6, 4);\nend t.",
     "x", "", "trap 100 at 2:8"},
    {"program t; begin var s: String := \"abc\";\n  writebytes(s, 3, 2);\nend t.",
     "", "", "trap 100 at 2:3"},
    {"program t; begin var s: String := \"abc\";\n  writebytes(s, 0, 1);\nend t.",
     "", "", "trap 100 at 2:3"},
    {"program t; type Bs = array[Int32 range <>] of Byte; begin var p := new(Bs range 1..3); dispose(p);\n  writebytes(p^, 1, 1);\nend t.",
     "", "", "trap 105 at 2:3"},
    {"program t; type Bs = array[Int32 range <>] of Byte; var n: Int32; begin var p := new(Bs range 1..3); dispose(p);\n  n := readbytes(p^, 1, 1);\nend t.",
     "x", "", "trap 105 at 2:8"},
    {"program t; type Bs = array[Int32 range <>] of Byte; var p: ^Bs; n: Int32; begin\n  n := readbytes(p^, 1, 1);\nend t.",
     "x", "", "trap 102 at 2:8"},
    {"program p; var n: Int32 := 141; begin halt(n); end p.",
     "", "", "trap 101"},
    {"program t; var s: String; b: Boolean; begin\n  b := readline(s);\nend t.",
     unreadable, "", "trap 107 at 2:8"},
    {"program t; var a: array[Int32 range 1..8] of Byte; n: Int32; begin\n  n := readbytes(a, 1, 8);\nend t.",
     unreadable, "", "trap 107 at 2:8"},
};

static int test_io(void)
{
    int failures = 0;
    for (size_t i = 0; i < COUNT(io_cases); i++) {
        char end[256];
        limba_report dummy;
        size_t len;
        char *out = compile_run(io_cases[i].src, io_cases[i].in,
                                strlen(io_cases[i].in), 0, NULL, end,
                                sizeof(end), &dummy, &len);
        char *at = strstr(end, " at ");
        if (at && !strstr(io_cases[i].end, " at "))
            *at = 0;
        if (len != strlen(io_cases[i].out) ||
            (len && memcmp(out, io_cases[i].out, len)) ||
            strcmp(end, io_cases[i].end)) {
            fprintf(stderr,
                    "test_luxia: input case %zu\n  printed  \"%s\"\n  "
                    "expected \"%s\"\n  ended    %s\n  expected %s\n",
                    i, out ? out : "", io_cases[i].out, end, io_cases[i].end);
            failures++;
        }
        free(out);
    }
    return failures;
}

/* the whole of a file, malloc'd and NUL-terminated; NULL if it cannot be
   opened */
static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *text = NULL;
    FILE *m = open_memstream(&text, len);
    int c;
    while ((c = fgetc(f)) != EOF)
        fputc(c, m);
    fclose(m);
    fclose(f);
    return text;
}

/* every program of tests/luxia/benchmarks is valid Luxia 0 and prints
   the .out of the same name in expected/, reading the .in there as its
   input, if any */
static int test_programs(unsigned *count)
{
    static const char dir[] = "tests/luxia/benchmarks";
    int failures = 0;
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "test_luxia: %s: cannot open (run from the root)\n",
                dir);
        return 1;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n < 7 || strcmp(e->d_name + n - 6, ".luxia"))
            continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        size_t len;
        char *text = slurp(path, &len);
        if (!text) {
            failures++;
            continue;
        }
        char *errors;
        check_string(text, &errors);
        if (errors[0]) {
            fprintf(stderr, "test_luxia: %s: %s\n", path, errors);
            failures++;
        }
        size_t elen, inlen = 0;
        snprintf(path, sizeof(path), "%s/expected/%.*s.out", dir, (int)(n - 6),
                 e->d_name);
        char *exp = slurp(path, &elen);
        snprintf(path + strlen(path) - 4, 5, ".in");
        char *in = slurp(path, &inlen);
        if (!exp) {
            fprintf(stderr, "test_luxia: %s: no expected output\n", e->d_name);
            failures++;
        }
        if (exp && !errors[0]) {
            char end[256];
            size_t olen;
            char *out = compile_run(text, in, inlen, 0, NULL, end, sizeof(end),
                                    NULL, &olen);
            if (strcmp(end, "ok") || olen != elen ||
                (olen && memcmp(out, exp, olen))) {
                fprintf(stderr,
                        "test_luxia: %s: ended %s, printed %zu bytes (%zu "
                        "expected)\n%s\n",
                        e->d_name, end, olen, elen, out ? out : "");
                failures++;
            }
            free(out);
        }
        free(exp);
        free(in);
        (*count)++;
        free(errors);
        free(text);
    }
    closedir(d);
    return failures;
}

static int test_sema(void)
{
    int failures = 0;
    for (size_t i = 0; i < sizeof(sema_cases) / sizeof(sema_cases[0]); i++) {
        char *errors;
        check_string(sema_cases[i].src, &errors);
        if (strcmp(errors, sema_cases[i].errors)) {
            fprintf(stderr,
                    "test_luxia: semantic case %zu\n  errors   %s\n  "
                    "expected %s\n",
                    i, errors, sema_cases[i].errors);
            failures++;
        }
        free(errors);
    }
    return failures;
}

static uint64_t env(const char *name, uint64_t def)
{
    const char *s = getenv(name);
    return s && *s ? strtoull(s, NULL, 0) : def;
}

/* random programs (src/luxia/gen.c) print what their generator says, and
   end where it says:
     LIMBA_LXGEN_SEEDS  how many seeds (default 200)
     LIMBA_LXGEN_FIRST  the first seed (default 1) */
static int test_random(void)
{
    uint64_t first = env("LIMBA_LXGEN_FIRST", 1);
    uint64_t count = env("LIMBA_LXGEN_SEEDS", 200);
    unsigned ok = 0, trap[128] = {0}, other = 0, failures = 0;
    size_t bytes = 0;
    for (uint64_t seed = first; seed < first + count; seed++) {
        limba_lxgen p;
        if (!limba_lxgen_make(seed, &p)) {
            other++;
            continue;
        }
        char end[256];
        size_t olen;
        char *out = compile_run(p.src, p.in, p.inlen, p.argc, p.argv, end,
                                sizeof(end), NULL, &olen);
        if (strcmp(end, p.end) || olen != p.outlen ||
            (olen && memcmp(out, p.out, olen))) {
            size_t k = 0;
            while (k < olen && k < p.outlen && out[k] == p.out[k])
                k++;
            fprintf(stderr,
                    "test_luxia: random seed %llu: ended %s, expected %s; "
                    "output differs at byte %zu of %zu (%zu expected); "
                    "tools/lx_gen %llu\n",
                    (unsigned long long)seed, end, p.end, k, olen, p.outlen,
                    (unsigned long long)seed);
            failures++;
        }
        unsigned code;
        if (!strncmp(p.end, "ok", 2))
            ok++;
        else if (sscanf(p.end, "trap %u", &code) == 1 && code < 128)
            trap[code]++;
        bytes += p.outlen;
        free(out);
        limba_lxgen_free(&p);
    }
    printf("test_luxia: %llu random programs from %llu: %u ended, trapped "
           "%u overflow, %u division, %u index, %u range, %u nil, %u "
           "conversion, %u shift; %u over the limits; %zu bytes printed, %u "
           "failures\n",
           (unsigned long long)count, (unsigned long long)first, ok, trap[6],
           trap[11], trap[100], trap[101], trap[102], trap[103], trap[104],
           other, bytes, failures);
    return (int)failures;
}

/* every program of tests/luxia/ffi calls C (§ 8.5): valid Luxia 0, its
   IR valid, for x86_64-linux (the reference interpreter does not call C:
   a back end runs them with probe.c and compares expected/) */
/* the IR of the program at path for platform target (LXS_*), NULL with
   the errors reported */
static limba_module *ffi_module(const char *path, unsigned target)
{
    size_t len;
    char *text = slurp(path, &len);
    if (!text)
        return NULL;
    limba_source s;
    limba_source_init(&s);
    uint32_t f = limba_source_add(&s, "t.luxia", text, len);
    limba_report rep;
    limba_report_init(&rep, &s, 'L', 0);
    limba_lx lx;
    limba_lx_init(&lx);
    limba_lx_run(&lx, &s, f, &rep);
    limba_lx_ast t;
    limba_lx_ast_init(&t);
    limba_lx_parse(&t, &lx, &s, &rep);
    limba_lxs sema;
    limba_lxs_init(&sema, &t, &lx, &s, &rep);
    sema.target = target;
    if (rep.errors == 0)
        limba_lxs_check(&sema);
    limba_module *m = rep.errors ? NULL : limba_lxl_program(&sema);
    if (rep.errors && getenv("LIMBA_TEST_VERBOSE"))
        limba_report_print(&rep, stderr);
    limba_lxs_free(&sema);
    limba_lx_ast_free(&t);
    limba_lx_free(&lx);
    limba_report_free(&rep);
    limba_source_free(&s);
    free(text);
    return m;
}

/* the name of the target of m is want */
static bool target_is(const limba_module *m, const char *want)
{
    size_t n = 0;
    const char *s = m->target != LIMBA_NONE ? limba_str(m, m->target, &n) : "";
    return n == strlen(want) && !memcmp(s, want, n);
}

/* every program of tests/luxia/ffi calls C (§ 8.5): valid Luxia 0, its
   IR valid, for x86_64-linux (the reference interpreter does not call C:
   a back end runs them with probe.c and compares expected/); probes.luxia
   for x86_64-windows too, where C's long is 32 bits */
static int test_ffi(unsigned *count)
{
    static const char dir[] = "tests/luxia/ffi";
    int failures = 0;
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "test_luxia: %s: cannot open\n", dir);
        return 1;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n < 7 || strcmp(e->d_name + n - 6, ".luxia"))
            continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        limba_module *m = ffi_module(path, LXS_X86_64_LINUX);
        limba_diag dg = {{0}, 0};
        if (!m || limba_verify(m, &dg) != 0 || m->nexterns == 0 ||
            !target_is(m, "x86_64-linux")) {
            fprintf(stderr, "test_luxia: %s: %s\n", path,
                    m ? (dg.msg[0] ? dg.msg : "no extern, or no target")
                      : "compile errors");
            failures++;
        }
        (*count)++;
        limba_module_free(m);
    }
    closedir(d);
    /* the extensions C wants: unsigned char zext, signed sext, _Bool
       none */
    limba_module *x =
        ffi_module("tests/luxia/ffi/probes.luxia", LXS_X86_64_LINUX);
    unsigned seen = 0;
    for (uint32_t i = 0; x && i < x->nexterns; i++) {
        size_t k;
        const char *nm = limba_str(x, x->externs[i].name, &k);
        const limba_type *ft = &x->types[x->externs[i].type];
        uint32_t p0 = ft->count ? x->members[ft->first].offset : 0;
        if (k == 8 && !memcmp(nm, "NextByte", 8))
            seen += p0 == LIMBA_EXT_ZEXT && ft->rext == LIMBA_EXT_ZEXT;
        else if (k == 4 && !memcmp(nm, "Sub8", 4))
            seen += p0 == LIMBA_EXT_SEXT && ft->rext == LIMBA_EXT_SEXT;
        else if (k == 4 && !memcmp(nm, "Even", 4))
            seen += p0 == LIMBA_EXT_NONE && ft->rext == LIMBA_EXT_NONE;
    }
    if (seen != 3) {
        fprintf(stderr, "test_luxia: probes.luxia: the extensions of the "
                        "narrow integers are wrong\n");
        failures++;
    }
    limba_module_free(x);
    /* long of Win64: 32 bits */
    limba_module *w =
        ffi_module("tests/luxia/ffi/probes.luxia", LXS_X86_64_WINDOWS);
    bool ok = w && target_is(w, "x86_64-windows");
    for (uint32_t i = 0; ok && i < w->nexterns; i++) {
        size_t k;
        const char *nm = limba_str(w, w->externs[i].name, &k);
        const limba_type *ft = &w->types[w->externs[i].type];
        if (k == 3 && !memcmp(nm, "Inc", 3))
            ok = ft->elem == LIMBA_T_I32 &&
                 w->members[ft->first].type == LIMBA_T_I32;
    }
    if (!ok) {
        fprintf(stderr, "test_luxia: probes.luxia for x86_64-windows: long "
                        "is not 32 bits, or no target\n");
        failures++;
    }
    limba_module_free(w);
    return failures;
}

int main(void)
{
    int failures = test_lexer() + test_report() + test_limit();
    failures += test_parser(expr_cases, COUNT(expr_cases), false, "expression");
    failures +=
        test_parser(program_cases, COUNT(program_cases), true, "program");
    failures += test_sema();
    unsigned programs = 0;
    failures += test_programs(&programs);
    unsigned ffi = 0;
    failures += test_ffi(&ffi);
    failures += test_run();
    failures += test_io();
    failures += test_random();
    printf("test_luxia: %zu lexer, %zu expression, %zu program, %zu "
           "semantic, %zu run and %zu input cases, %u valid programs, %u "
           "calling C, report and limit, %d failures\n",
           COUNT(lex_cases), COUNT(expr_cases), COUNT(program_cases),
           COUNT(sema_cases), COUNT(run_cases), COUNT(io_cases), programs,
           ffi, failures);
    return failures ? 1 : 0;
}
