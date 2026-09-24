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

int main(void)
{
    int failures = test_lexer() + test_report() + test_limit();
    failures += test_parser(expr_cases, COUNT(expr_cases), false, "expression");
    failures +=
        test_parser(program_cases, COUNT(program_cases), true, "program");
    printf("test_luxia: %zu lexer, %zu expression and %zu program cases, "
           "report and limit, %d failures\n",
           COUNT(lex_cases), COUNT(expr_cases), COUNT(program_cases), failures);
    return failures ? 1 : 0;
}
