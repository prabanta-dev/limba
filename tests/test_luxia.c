/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * test_luxia.c - the Luxia front end against expected results. For now the
 * lexer: each case is a source, the tokens it must give in the short form
 * of limba_lx_show, and the diagnoses it must give as code@line:column.
 */
#include "front/diag.h"
#include "front/source.h"
#include "luxia/lex.h"

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

int main(void)
{
    int failures = test_lexer() + test_report() + test_limit();
    size_t count = sizeof(lex_cases) / sizeof(lex_cases[0]);
    printf("test_luxia: %zu lexer cases, report and limit, %d failures\n",
           count, failures);
    return failures ? 1 : 0;
}
