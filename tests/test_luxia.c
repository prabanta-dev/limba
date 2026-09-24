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
#include "luxia/lex.h"
#include "luxia/parse.h"
#include "eval/eval.h"
#include "limba/opt.h"
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
    {"'' 'ab'", "char:U+0000 char:U+0061 id:b char:U+0000 eof",
     "L0005@1:1 L0005@1:4 L0005@1:7"},
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
     "procedure q(out r: array of Byte);\n"
     "begin\n"
     "end;\n"
     "begin\n"
     "end.",
     "(program p [(type colore (tenum [rosso verde])) (type p (tptr (tname "
     "nodo - -))) (type nodo (trecord [(field [x y] (tname float64 - -)) "
     "(field [next] (tname p - -))])) (type v (tarray (tname int32 1 10) "
     "(tname float64 - -))) (type m (tnew (tname float64 - -))) (routine "
     "procedure q [(param out [r] (topen (tname byte - -)))] - (body [] "
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
     "function Somma(a: array of Float64): Float64;\n"
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
    {"program p; var a: array of Int32; begin end.", "L0049@1:19"},
    {"program p; var n: Int32 := 3; var a: array[Int32 range 1..n] of Int32;"
     " begin end.",
     "L0051@1:44"},
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

static const run_case run_cases[] = {
    {"program p; begin writeln(\"ciao, \", 42, ' ', true); end.",
     "ciao, 42 true\n", "ok"},
    /* integers: div truncates, mod has the sign of the divisor, rem of
       the dividend */
    {"program p;\nvar a, b, c: Int32;\nbegin\n  a := -7; b := 2; c := -2;\n"
     "  writeln(a div b, \" \", a mod b, \" \", a rem b);\n  a := 7;\n"
     "  writeln(a div c, \" \", a mod c, \" \", a rem c);\nend.",
     "-3 1 -1\n-3 -1 1\n", "ok"},
    {"program p; var a: Int8 := 100; begin a := a + a; writeln(a); end.", "",
     "trap 6"},
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
     "", "trap 100"},
    {"program p; type P = Int32 range 0..100; var x: P; y: Int32 := 101; "
     "begin x := y; end.",
     "", "trap 101"},
    {"program p; type N = record a: Int32; end; var p: ^N; begin p.a := 1; "
     "end.",
     "", "trap 102"},
    {"program p; var a: Int32 := 1; z: Int32 := 0; begin writeln(a div z); "
     "end.",
     "", "trap 11"},
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
     "procedure Fill(var a: array of Int32);\nbegin\n"
     "  for var i := low(a) to high(a) do a[i] := Int32(i) * 10; end;\n"
     "end;\n"
     "function Sum(a: array of Int32): Int64;\nvar s: Int64 := 0;\nbegin\n"
     "  for var i := 0 to high(a) do s := s + Int64(a[i]); end;\n"
     "  return s;\nend;\n"
     "procedure Move(var p: Pt; dx: Int32);\nbegin p.x := p.x + dx; end;\n"
     "begin\n  Fill(v);\n  writeln(Sum(v), \" \", v[4]);\n"
     "  q.x := 1; q.y := 2;\n  Move(q, 5);\n  var r: Pt := q;\n  r.y := 9;\n"
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
     "81 1024\n", "ok"},
    {"program p; begin writeln(\"a\"); halt(3); writeln(\"b\"); end.", "a\n",
     "halt 3"},
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
};

/* run main; what it printed (malloc'd) and how it ended */
static char *run_module(limba_module *m, char *end, size_t size)
{
    limba_eval_result r;
    limba_eval(m, "main", NULL, &r);
    snprintf(end, size, "ok");
    if (r.status == LIMBA_EVAL_TRAP)
        snprintf(end, size, "trap %lld", (long long)r.code);
    else if (r.status == LIMBA_EVAL_HALT)
        snprintf(end, size, "halt %lld", (long long)r.code);
    else if (r.status != LIMBA_EVAL_OK)
        snprintf(end, size, "status %d", r.status);
    char *out = r.out;
    r.out = NULL;
    limba_eval_result_free(&r);
    return out;
}

/* compile a Luxia source to a verified module, run it, then optimise it
   and run it again: both runs must agree (the OPTDIFF net) */
static char *compile_run(const char *src, char *end, size_t size,
                         limba_report *keep)
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
            out = run_module(m, end, size);
            char end2[256];
            if (limba_optimize(m, NULL, &d) != 0) {
                snprintf(end, size, "optimiser: %.200s", d.msg);
            } else {
                char *out2 = run_module(m, end2, sizeof(end2));
                if (strcmp(out ? out : "", out2 ? out2 : "") ||
                    strcmp(end, end2)) {
                    char first[128];
                    snprintf(first, sizeof(first), "%s", end);
                    snprintf(end, size, "OPTDIFF: %.100s / %.100s", first,
                             end2);
                }
                free(out2);
            }
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
    return out;
}

static int test_run(void)
{
    int failures = 0;
    for (size_t i = 0; i < sizeof(run_cases) / sizeof(run_cases[0]); i++) {
        const run_case *c = &run_cases[i];
        char end[256];
        limba_report dummy;
        char *out = compile_run(c->src, end, sizeof(end), &dummy);
        const char *o = out ? out : "";
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

/* every program of tests/luxia/benchmarks is valid Luxia 0 */
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
        FILE *f = fopen(path, "rb");
        if (!f) {
            failures++;
            continue;
        }
        char *text = NULL;
        size_t len = 0;
        FILE *m = open_memstream(&text, &len);
        int c;
        while ((c = fgetc(f)) != EOF)
            fputc(c, m);
        fclose(m);
        fclose(f);
        char *errors;
        check_string(text, &errors);
        if (errors[0]) {
            fprintf(stderr, "test_luxia: %s: %s\n", path, errors);
            failures++;
        }
        snprintf(path + strlen(path) - 6, 7, ".out");
        FILE *want = fopen(path, "rb");
        if (want && !errors[0]) {
            char *exp = NULL;
            size_t elen = 0;
            FILE *em = open_memstream(&exp, &elen);
            while ((c = fgetc(want)) != EOF)
                fputc(c, em);
            fclose(em);
            char end[256];
            char *out = compile_run(text, end, sizeof(end), NULL);
            if (strcmp(end, "ok") || strcmp(out ? out : "", exp)) {
                fprintf(stderr, "test_luxia: %s: ended %s, printed\n%s\n", path,
                        end, out ? out : "");
                failures++;
            }
            free(out);
            free(exp);
        }
        if (want)
            fclose(want);
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

int main(void)
{
    int failures = test_lexer() + test_report() + test_limit();
    failures += test_parser(expr_cases, COUNT(expr_cases), false, "expression");
    failures +=
        test_parser(program_cases, COUNT(program_cases), true, "program");
    failures += test_sema();
    unsigned programs = 0;
    failures += test_programs(&programs);
    failures += test_run();
    printf("test_luxia: %zu lexer, %zu expression, %zu program, %zu "
           "semantic and %zu run cases, %u valid programs, report and limit, "
           "%d failures\n",
           COUNT(lex_cases), COUNT(expr_cases), COUNT(program_cases),
           COUNT(sema_cases), COUNT(run_cases), programs, failures);
    return failures ? 1 : 0;
}
