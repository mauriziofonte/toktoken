/*
 * test_parser_perl.c -- Unit tests for the Perl parser (in-memory sources).
 *
 * Lexer edge cases (POD, heredocs, quote-like operators with braces,
 * __END__), symbol extraction (subroutine kinds, signatures, packages,
 * constants), imports (require/do, Webmin foreign_require, computed
 * `do $script`), security sites and taint-lite masks.
 */

#include "test_framework.h"
#include "parser_perl.h"
#include "symbol_kind.h"

#include <stdlib.h>
#include <string.h>

static int parse(const char *path, const char *src, tt_perl_result_t *r)
{
    memset(r, 0, sizeof(*r));
    return tt_parse_perl_content(path, src, strlen(src), r);
}

static const tt_symbol_t *sym_named(const tt_perl_result_t *r, const char *name)
{
    for (int i = 0; i < r->sym_count; i++)
    {
        if (r->symbols[i].name && strcmp(r->symbols[i].name, name) == 0)
            return &r->symbols[i];
    }
    return NULL;
}

static const tt_import_t *import_to(const tt_perl_result_t *r, const char *target)
{
    for (int i = 0; i < r->imp_count; i++)
    {
        if (r->imports[i].to_specifier && strcmp(r->imports[i].to_specifier, target) == 0)
            return &r->imports[i];
    }
    return NULL;
}

static const tt_site_t *site_at(const tt_perl_result_t *r, tt_site_category_e cat, const char *name,
                                int line)
{
    for (int i = 0; i < r->site_count; i++)
    {
        const tt_site_t *s = &r->sites[i];
        if (s->category == cat && strcmp(s->name, name) == 0 && (line == 0 || s->line == line))
            return s;
    }
    return NULL;
}

TT_TEST(test_perl_builtins_table_sorted)
{
    TT_ASSERT_TRUE(tt_perl_builtins_sorted());
}

TT_TEST(test_perl_is_perl_path)
{
    TT_ASSERT_TRUE(tt_perl_is_perl_path("a/b.pl"));
    TT_ASSERT_TRUE(tt_perl_is_perl_path("Lib/Mod.pm"));
    TT_ASSERT_TRUE(tt_perl_is_perl_path("save.cgi"));
    TT_ASSERT_TRUE(tt_perl_is_perl_path("app.psgi"));
    TT_ASSERT_FALSE(tt_perl_is_perl_path("t/basic.t"));
    TT_ASSERT_FALSE(tt_perl_is_perl_path("x.py"));
}

TT_TEST(test_perl_shebang_language)
{
    const char *p = "#!/usr/bin/perl -w\nprint 1;\n";
    const char *e = "#!/usr/bin/env perl\n";
    const char *b = "#!/bin/sh\necho\n";
    TT_ASSERT_EQ_STR("perl", tt_perl_shebang_language(p, strlen(p)));
    TT_ASSERT_EQ_STR("perl", tt_perl_shebang_language(e, strlen(e)));
    TT_ASSERT_EQ_STR("bash", tt_perl_shebang_language(b, strlen(b)));
    TT_ASSERT_NULL(tt_perl_shebang_language("print 1;\n", 9));
}

TT_TEST(test_perl_sub_body_with_braces_in_strings_heredocs_regex)
{
    const char *src =
        "sub tricky\n"                          /* 1 */
        "{\n"                                   /* 2 */
        "my ($a) = @_;\n"                       /* 3 */
        "my $s = \"}\";\n"                      /* 4 */
        "my $q = q{ { } };\n"                   /* 5 */
        "my $h = <<'EOT';\n"                    /* 6 */
        "}}}\n"                                 /* 7 */
        "EOT\n"                                 /* 8 */
        "my $i = <<~EOI;\n"                     /* 9 */
        "    { indented }\n"                    /* 10 */
        "    EOI\n"                             /* 11 */
        "(my $c = $a) =~ s{/+$}{};\n"           /* 12 */
        "my $re = qr{\\{(\\w+)\\}};\n"          /* 13 */
        "return $c;\n"                          /* 14 */
        "}\n"                                   /* 15 */
        "sub after { return 1; }\n";            /* 16 */
    tt_perl_result_t r;
    TT_ASSERT_EQ_INT(0, parse("lib/t.pl", src, &r));
    const tt_symbol_t *s = sym_named(&r, "tricky");
    TT_ASSERT_NOT_NULL(s);
    if (s)
    {
        TT_ASSERT_EQ_INT(TT_KIND_SUBROUTINE, s->kind);
        TT_ASSERT_EQ_INT(1, s->line);
        TT_ASSERT_EQ_INT(15, s->end_line);
        TT_ASSERT_EQ_STR("tricky($a)", s->signature);
        TT_ASSERT_EQ_STR("lib/t.pl::tricky#subroutine", s->id);
    }
    const tt_symbol_t *a = sym_named(&r, "after");
    TT_ASSERT_NOT_NULL(a);
    if (a)
        TT_ASSERT_EQ_INT(16, a->line);
    tt_perl_result_free(&r);
}

TT_TEST(test_perl_pod_and_end_markers)
{
    const char *src =
        "=head2 documented(x)\n"
        "\n"
        "Does a thing.\n"
        "\n"
        "=cut\n"
        "sub documented\n"
        "{\n"
        "return 1;\n"
        "}\n"
        "\n"
        "=pod\n"
        "\n"
        "sub in_pod { }\n"
        "\n"
        "=cut\n"
        "1;\n"
        "__END__\n"
        "sub after_end { system('rm'); }\n";
    tt_perl_result_t r;
    TT_ASSERT_EQ_INT(0, parse("x.pl", src, &r));
    const tt_symbol_t *d = sym_named(&r, "documented");
    TT_ASSERT_NOT_NULL(d);
    if (d)
        TT_ASSERT_EQ_STR("Does a thing.", d->docstring);
    TT_ASSERT_NULL(sym_named(&r, "in_pod"));
    TT_ASSERT_NULL(sym_named(&r, "after_end"));
    TT_ASSERT_NULL(site_at(&r, TT_SITE_SINK, "system", 0));
    tt_perl_result_free(&r);
}

TT_TEST(test_perl_pod_docstring_picks_named_heading)
{
    const char *src =
        "=head1 lib.pl\n"
        "\n"
        "Module overview.\n"
        "\n"
        "=head2 run(dir)\n"
        "\n"
        "Runs in a directory.\n"
        "\n"
        "=cut\n"
        "sub run { my ($dir) = @_; }\n";
    tt_perl_result_t r;
    TT_ASSERT_EQ_INT(0, parse("lib.pl", src, &r));
    const tt_symbol_t *s = sym_named(&r, "run");
    TT_ASSERT_NOT_NULL(s);
    if (s)
        TT_ASSERT_EQ_STR("Runs in a directory.", s->docstring);
    tt_perl_result_free(&r);
}

TT_TEST(test_perl_package_methods_constants)
{
    const char *src =
        "package My::Thing;\n"
        "use strict;\n"
        "use constant LIMIT => 3;\n"
        "our $VERSION = '1.0';\n"
        "sub new { my ($class, %a) = @_; return bless {}, $class; }\n"
        "sub helper { my ($x, $y) = @_; return $x; }\n"
        "1;\n";
    tt_perl_result_t r;
    TT_ASSERT_EQ_INT(0, parse("lib/My/Thing.pm", src, &r));
    const tt_symbol_t *pkg = sym_named(&r, "My::Thing");
    TT_ASSERT_NOT_NULL(pkg);
    if (pkg)
        TT_ASSERT_EQ_INT(TT_KIND_NAMESPACE, pkg->kind);
    const tt_symbol_t *n = sym_named(&r, "new");
    TT_ASSERT_NOT_NULL(n);
    if (n)
    {
        TT_ASSERT_EQ_INT(TT_KIND_METHOD, n->kind);
        TT_ASSERT_EQ_STR("My::Thing::new", n->qualified_name);
    }
    const tt_symbol_t *h = sym_named(&r, "helper");
    TT_ASSERT_NOT_NULL(h);
    if (h)
    {
        TT_ASSERT_EQ_INT(TT_KIND_SUBROUTINE, h->kind);
        TT_ASSERT_EQ_STR("helper($x, $y)", h->signature);
    }
    const tt_symbol_t *c = sym_named(&r, "LIMIT");
    TT_ASSERT_NOT_NULL(c);
    if (c)
        TT_ASSERT_EQ_INT(TT_KIND_CONSTANT, c->kind);
    tt_perl_result_free(&r);
}

TT_TEST(test_perl_signature_never_invented)
{
    const char *src = "sub bare { return scalar(@_); }\n";
    tt_perl_result_t r;
    TT_ASSERT_EQ_INT(0, parse("b.pl", src, &r));
    const tt_symbol_t *s = sym_named(&r, "bare");
    TT_ASSERT_NOT_NULL(s);
    if (s)
        TT_ASSERT_EQ_STR("sub bare", s->signature);
    tt_perl_result_free(&r);
}

TT_TEST(test_perl_imports_require_do_foreign)
{
    const char *src =
        "require './mod-lib.pl';\n"
        "do '../web-lib.pl';\n"
        "use POSIX;\n"
        "&foreign_require(\"cron\", \"cron-lib.pl\");\n"
        "&foreign_call(\"useradmin\", \"list_users\");\n";
    tt_perl_result_t r;
    TT_ASSERT_EQ_INT(0, parse("mod/save.cgi", src, &r));
    const tt_import_t *a = import_to(&r, "./mod-lib.pl");
    TT_ASSERT_NOT_NULL(a);
    if (a)
    {
        TT_ASSERT_EQ_STR("require", a->import_type);
        TT_ASSERT_EQ_INT(1, a->line);
        TT_ASSERT_EQ_INT(0, a->dynamic);
    }
    const tt_import_t *d = import_to(&r, "../web-lib.pl");
    TT_ASSERT_NOT_NULL(d);
    if (d)
        TT_ASSERT_EQ_STR("do", d->import_type);
    TT_ASSERT_NOT_NULL(import_to(&r, "POSIX"));
    const tt_import_t *f = import_to(&r, "cron/cron-lib.pl");
    TT_ASSERT_NOT_NULL(f);
    if (f)
    {
        TT_ASSERT_EQ_STR("foreign_require", f->import_type);
        TT_ASSERT_EQ_INT(1, f->dynamic);
        TT_ASSERT_TRUE(f->confidence > 0.99);
    }
    const tt_import_t *fc = import_to(&r, "useradmin/useradmin-lib.pl");
    TT_ASSERT_NOT_NULL(fc);
    if (fc)
        TT_ASSERT_EQ_STR("foreign_call", fc->import_type);
    tt_perl_result_free(&r);
}

TT_TEST(test_perl_do_variable_assigned_literals)
{
    const char *src =
        "my $script = -r '../funcs.pl' ? '../funcs.pl'\n"
        "                               : 'funcs.pl';\n"
        "do $script;\n";
    tt_perl_result_t r;
    TT_ASSERT_EQ_INT(0, parse("web-lib.pl", src, &r));
    TT_ASSERT_NOT_NULL(import_to(&r, "$script"));
    const tt_import_t *a = import_to(&r, "../funcs.pl");
    TT_ASSERT_NOT_NULL(a);
    if (a)
    {
        TT_ASSERT_EQ_INT(1, a->dynamic);
        TT_ASSERT_TRUE(a->confidence > 0.49 && a->confidence < 0.51);
        TT_ASSERT_EQ_INT(3, a->line);
    }
    TT_ASSERT_NOT_NULL(import_to(&r, "funcs.pl"));
    tt_perl_result_free(&r);
}

TT_TEST(test_perl_sites_and_masks)
{
    const char *src =
        "#!/usr/bin/perl\n"                                 /* 1 */
        "require './lib.pl';\n"                             /* 2 */
        "&ReadParse();\n"                                   /* 3 */
        "my $d = $in{'dir'};\n"                             /* 4 */
        "system(\"ls $d\");\n"                              /* 5 */
        "system(\"ls \".quotemeta($in{'x'}));\n"            /* 6 */
        "&do_work($in{'job'}, 'literal');\n"                /* 7 */
        "my $out = `date`;\n"                               /* 8 */
        "open(my $fh, \"|mail $ENV{'USER'}\");\n";          /* 9 */
    tt_perl_result_t r;
    TT_ASSERT_EQ_INT(0, parse("m/run.cgi", src, &r));
    TT_ASSERT_TRUE((r.file_flags & TT_FILE_FLAG_CGI) != 0);

    const tt_site_t *s5 = site_at(&r, TT_SITE_SINK, "system", 5);
    TT_ASSERT_NOT_NULL(s5);
    if (s5)
    {
        TT_ASSERT_TRUE((s5->mask & TT_TAINT_IN) != 0);
        TT_ASSERT_FALSE((s5->mask & TT_TAINT_SANITIZED) != 0);
        TT_ASSERT_NOT_NULL(s5->trace);
        TT_ASSERT_NULL(s5->scope_id);
    }
    const tt_site_t *s6 = site_at(&r, TT_SITE_SINK, "system", 6);
    TT_ASSERT_NOT_NULL(s6);
    if (s6)
        TT_ASSERT_TRUE((s6->mask & TT_TAINT_SANITIZED) != 0);
    TT_ASSERT_NOT_NULL(site_at(&r, TT_SITE_SANITIZER, "quotemeta", 6));
    TT_ASSERT_NOT_NULL(site_at(&r, TT_SITE_SOURCE, "in", 4));

    const tt_site_t *c7 = site_at(&r, TT_SITE_CALL, "do_work", 7);
    TT_ASSERT_NOT_NULL(c7);
    if (c7)
    {
        TT_ASSERT_NOT_NULL(c7->arg_masks);
        if (c7->arg_masks)
            TT_ASSERT_EQ_STR("[1,0]", c7->arg_masks);
    }
    const tt_site_t *b8 = site_at(&r, TT_SITE_SINK, "backtick", 8);
    TT_ASSERT_NOT_NULL(b8);
    if (b8)
        TT_ASSERT_EQ_INT(0, (int)(b8->mask & TT_TAINT_SOURCES));
    const tt_site_t *p9 = site_at(&r, TT_SITE_SINK, "open_pipe", 9);
    TT_ASSERT_NOT_NULL(p9);
    if (p9)
        TT_ASSERT_TRUE((p9->mask & TT_TAINT_ENV) != 0);
    tt_perl_result_free(&r);
}

TT_TEST(test_perl_param_masks_inside_sub)
{
    const char *src =
        "sub run_it\n"
        "{\n"
        "my ($cmd, $user) = @_;\n"
        "return system(\"su $user -c $cmd\");\n"
        "}\n";
    tt_perl_result_t r;
    TT_ASSERT_EQ_INT(0, parse("lib.pl", src, &r));
    const tt_site_t *s = site_at(&r, TT_SITE_SINK, "system", 4);
    TT_ASSERT_NOT_NULL(s);
    if (s)
    {
        TT_ASSERT_TRUE((s->mask & TT_TAINT_PARAM(0)) != 0);
        TT_ASSERT_TRUE((s->mask & TT_TAINT_PARAM(1)) != 0);
        TT_ASSERT_NOT_NULL(s->scope_id);
        if (s->scope_id)
            TT_ASSERT_EQ_STR("lib.pl::run_it#subroutine", s->scope_id);
    }
    tt_perl_result_free(&r);
}

TT_TEST(test_perl_unauthcgi_harness)
{
    const char *src = "my %d = (\"unauthcgi\", \"^/forgot.cgi\\$ ^/reset.cgi\\$\");\n";
    tt_perl_result_t r;
    TT_ASSERT_EQ_INT(0, parse("miniserv-lib.pl", src, &r));
    const tt_site_t *h = site_at(&r, TT_SITE_HARNESS, "unauthcgi", 1);
    TT_ASSERT_NOT_NULL(h);
    if (h)
        TT_ASSERT_STR_CONTAINS(h->args, "forgot.cgi");
    tt_perl_result_free(&r);
}

TT_TEST(test_perl_malformed_input)
{
    const char *cases[] = {
        "sub broken { if ($x) { ",
        "my $h = <<EOT;\nno terminator\n",
        "=head1 never closed\n\nsub x {}\n",
        "s{unterminated",
        "}}}}{{{{",
        "",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        tt_perl_result_t r;
        TT_ASSERT_EQ_INT(0, parse("bad.pl", cases[i], &r));
        tt_perl_result_free(&r);
    }
}

void run_parser_perl_tests(void)
{
    TT_RUN(test_perl_builtins_table_sorted);
    TT_RUN(test_perl_is_perl_path);
    TT_RUN(test_perl_shebang_language);
    TT_RUN(test_perl_sub_body_with_braces_in_strings_heredocs_regex);
    TT_RUN(test_perl_pod_and_end_markers);
    TT_RUN(test_perl_pod_docstring_picks_named_heading);
    TT_RUN(test_perl_package_methods_constants);
    TT_RUN(test_perl_signature_never_invented);
    TT_RUN(test_perl_imports_require_do_foreign);
    TT_RUN(test_perl_do_variable_assigned_literals);
    TT_RUN(test_perl_sites_and_masks);
    TT_RUN(test_perl_param_masks_inside_sub);
    TT_RUN(test_perl_unauthcgi_harness);
    TT_RUN(test_perl_malformed_input);
}
