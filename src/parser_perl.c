/*
 * parser_perl.c -- Perl 5 parser: symbols, imports and security sites.
 *
 * Two phases:
 *
 * 1. Lexer. Produces a token stream for code only. POD blocks, comments,
 *    string bodies, heredoc bodies and quote-like operators (q qq qw qx m
 *    qr s tr y) are consumed whole, so braces and keywords inside them
 *    never reach the analyzer. Regex-vs-division and heredoc-vs-shift are
 *    disambiguated from the previous significant token. Lexing stops at
 *    __END__ / __DATA__ (POD headings after the marker are still read).
 *
 * 2. Analyzer. Walks the tokens with matched brackets:
 *    - symbols: subroutine/method (body = brace-matched span), namespace
 *      (package), variable (file-scope my/our/local), constant
 *      (use constant), directive (use/require/do), POD section/subsection;
 *    - imports: use, require, do, use lib, push @INC, plus Webmin dynamic
 *      dispatch (foreign_require, foreign_call, remote_foreign_*) recorded
 *      as dynamic edges with a confidence;
 *    - sites: sinks, sources, sanitizers, ACL gates, user calls, with a
 *      per-scope taint-lite origin mask (see site.h). Flow-insensitive
 *      weak updates; no evaluation of Perl expressions.
 */

#include "parser_perl.h"
#include "parser_common.h"
#include "line_offsets.h"
#include "fast_hash.h"
#include "hashmap.h"
#include "str_util.h"
#include "platform.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===================================================================== */
/* Tokens                                                                 */
/* ===================================================================== */

typedef enum
{
    PT_IDENT,
    PT_VAR,
    PT_STR,
    PT_REGEX,
    PT_NUM,
    PT_OP,
    PT_READLINE
} ptype_e;

enum
{
    STR_SQ = 0,
    STR_DQ,
    STR_CMD,
    STR_QW,
    STR_HEREDOC_SQ,
    STR_HEREDOC_DQ,
    STR_HEREDOC_CMD
};

enum
{
    RX_MATCH = 0,
    RX_SUBST,
    RX_TRANS
};

typedef struct
{
    unsigned char type;
    unsigned char sub;
    unsigned char interp;
    int start, end;   /* token span in source */
    int cstart, cend; /* content span (strings, regex, heredoc body) */
    int line;         /* 1-indexed line of token start */
    int match;        /* matching bracket token index, -1 if none */
    int depth;        /* number of enclosing '{' */
} ptok_t;

typedef struct
{
    int tok;
    int term_start;
    int term_len;
    bool indent;
} pheredoc_t;

typedef struct
{
    int line;
    int level; /* 1..4 */
    int text_start;
    int text_len;
    int block; /* index into pods */
} ppod_head_t;

typedef struct
{
    int start_line; /* line of the opening =directive */
    int end_line;   /* line of =cut (or last line) */
} ppod_block_t;

#define MAX_PENDING_HEREDOCS 16

typedef struct
{
    const char *s;
    int len;
    int pos;
    int line;
    bool at_line_start;
    ptok_t *toks;
    int ntok;
    int captok;
    pheredoc_t heredocs[MAX_PENDING_HEREDOCS];
    int nheredoc;
    ppod_head_t *heads;
    int nhead;
    int caphead;
    ppod_block_t *pods;
    int npod;
    int cappod;
    unsigned char bkind[512]; /* per open '{': 1 = expression brace, 0 = block */
    int nbkind;
    bool oom;
} plex_t;

#define BRACE_BLOCK 0
#define BRACE_EXPR 1

#define CH(L, p) ((p) >= 0 && (p) < (L)->len ? (unsigned char)(L)->s[(p)] : 0)

static inline bool is_id_start(int c) { return isalpha(c) || c == '_'; }
static inline bool is_id_char(int c) { return isalnum(c) || c == '_'; }

static int push_tok(plex_t *L, ptype_e type, int start, int end, int line)
{
    if (L->ntok >= L->captok)
    {
        int nc = L->captok ? L->captok * 2 : 1024;
        ptok_t *t = realloc(L->toks, (size_t)nc * sizeof(ptok_t));
        if (!t)
        {
            L->oom = true;
            return -1;
        }
        L->toks = t;
        L->captok = nc;
    }
    ptok_t *t = &L->toks[L->ntok];
    memset(t, 0, sizeof(*t));
    t->type = (unsigned char)type;
    t->start = start;
    t->end = end;
    t->cstart = start;
    t->cend = end;
    t->line = line;
    t->match = -1;
    return L->ntok++;
}

static bool span_eq(const plex_t *L, int start, int end, const char *w)
{
    size_t n = strlen(w);
    return (size_t)(end - start) == n && memcmp(L->s + start, w, n) == 0;
}

static bool tok_is(const plex_t *L, int i, ptype_e type, const char *w)
{
    if (i < 0 || i >= L->ntok)
        return false;
    const ptok_t *t = &L->toks[i];
    return t->type == type && span_eq(L, t->start, t->end, w);
}

#define IS_OP(L, i, w) tok_is((L), (i), PT_OP, (w))
#define IS_IDENT(L, i, w) tok_is((L), (i), PT_IDENT, (w))

/* Identifiers after which a term (not an operator) is expected:
 * '/' starts a regex, '<' may start <FH>, '%' '&' '*' are sigils. */
static const char *TERM_IDENTS[] = {
    "if", "unless", "while", "until", "elsif", "and", "or", "not", "xor",
    "return", "split", "grep", "map", "join", "push", "unshift", "print",
    "printf", "say", "sort", "reverse", "when", "die", "warn", "lt", "gt",
    "le", "ge", "eq", "ne", "cmp", "x", "defined", "ref", "scalar", "lc",
    "uc", "length", "eval", "do", "foreach", "for", "last", "next", "redo",
    NULL};

static bool ident_expects_term(const plex_t *L, const ptok_t *t)
{
    for (const char **w = TERM_IDENTS; *w; w++)
    {
        if (span_eq(L, t->start, t->end, *w))
            return true;
    }
    return false;
}

static bool prev_is_operand_ex(const plex_t *L, bool ident_is_operand);

static bool prev_is_operand(const plex_t *L)
{
    return prev_is_operand_ex(L, true);
}

/* For '%' '&' '*': after an identifier a sigil is expected (keys %h). */
static bool prev_is_operand_sigil(const plex_t *L)
{
    return prev_is_operand_ex(L, false);
}

static bool prev_is_operand_ex(const plex_t *L, bool ident_is_operand)
{
    if (L->ntok == 0)
        return false;
    const ptok_t *t = &L->toks[L->ntok - 1];
    switch (t->type)
    {
    case PT_VAR:
    case PT_NUM:
    case PT_STR:
    case PT_READLINE:
    case PT_REGEX:
        return true;
    case PT_IDENT:
        return ident_is_operand && !ident_expects_term(L, t);
    case PT_OP:
    {
        if (t->end - t->start != 1)
            return false;
        char c = L->s[t->start];
        if (c == '}')
            return t->sub == BRACE_EXPR;
        return c == ')' || c == ']';
    }
    default:
        return false;
    }
}

/* ===================================================================== */
/* Lexer                                                                  */
/* ===================================================================== */

static void lex_pod(plex_t *L)
{
    if (L->npod >= L->cappod)
    {
        int nc = L->cappod ? L->cappod * 2 : 32;
        ppod_block_t *p = realloc(L->pods, (size_t)nc * sizeof(ppod_block_t));
        if (!p)
        {
            L->oom = true;
            return;
        }
        L->pods = p;
        L->cappod = nc;
    }
    int block = L->npod++;
    L->pods[block].start_line = L->line;
    L->pods[block].end_line = L->line;

    for (;;)
    {
        int ls = L->pos;
        int le = ls;
        while (le < L->len && L->s[le] != '\n')
            le++;

        if (le - ls > 6 && memcmp(L->s + ls, "=head", 5) == 0 &&
            L->s[ls + 5] >= '1' && L->s[ls + 5] <= '6')
        {
            if (L->nhead >= L->caphead)
            {
                int nc = L->caphead ? L->caphead * 2 : 64;
                ppod_head_t *h = realloc(L->heads, (size_t)nc * sizeof(ppod_head_t));
                if (!h)
                {
                    L->oom = true;
                    return;
                }
                L->heads = h;
                L->caphead = nc;
            }
            int ts = ls + 6;
            while (ts < le && (L->s[ts] == ' ' || L->s[ts] == '\t'))
                ts++;
            int te = le;
            while (te > ts && (L->s[te - 1] == ' ' || L->s[te - 1] == '\t' ||
                               L->s[te - 1] == '\r'))
                te--;
            if (te > ts)
            {
                ppod_head_t *h = &L->heads[L->nhead++];
                h->line = L->line;
                int lvl = L->s[ls + 5] - '0';
                h->level = lvl > 4 ? 4 : lvl;
                h->text_start = ts;
                h->text_len = te - ts;
                h->block = block;
            }
        }

        bool is_cut = (le - ls >= 4 && memcmp(L->s + ls, "=cut", 4) == 0 &&
                       (le - ls == 4 || !is_id_char((unsigned char)L->s[ls + 4])));
        L->pods[block].end_line = L->line;
        L->pos = le;
        if (L->pos < L->len)
        {
            L->pos++;
            L->line++;
        }
        if (is_cut || L->pos >= L->len)
            break;
    }
    L->at_line_start = true;
}

/* After __END__/__DATA__: only POD headings are of interest. */
static void scan_pod_tail(plex_t *L)
{
    /* Skip the marker line. */
    while (L->pos < L->len && L->s[L->pos] != '\n')
        L->pos++;
    while (L->pos < L->len && !L->oom)
    {
        if (L->s[L->pos] == '\n')
        {
            L->pos++;
            L->line++;
            if (L->pos < L->len && L->s[L->pos] == '=' &&
                is_id_start(CH(L, L->pos + 1)))
                lex_pod(L);
            continue;
        }
        L->pos++;
    }
}

static void read_heredocs(plex_t *L)
{
    for (int h = 0; h < L->nheredoc; h++)
    {
        pheredoc_t *hd = &L->heredocs[h];
        int body_start = L->pos;
        int body_end = -1;
        while (L->pos < L->len)
        {
            int ls = L->pos;
            int le = ls;
            while (le < L->len && L->s[le] != '\n')
                le++;
            int cs = ls, ce = le;
            if (ce > cs && L->s[ce - 1] == '\r')
                ce--;
            if (hd->indent)
            {
                while (cs < ce && (L->s[cs] == ' ' || L->s[cs] == '\t'))
                    cs++;
            }
            bool is_term = (ce - cs == hd->term_len &&
                            memcmp(L->s + cs, L->s + hd->term_start, (size_t)hd->term_len) == 0);
            L->pos = le < L->len ? le + 1 : le;
            if (le < L->len)
                L->line++;
            if (is_term)
            {
                body_end = ls;
                break;
            }
        }
        if (body_end < 0)
            body_end = L->len;
        if (hd->tok >= 0 && hd->tok < L->ntok)
        {
            L->toks[hd->tok].cstart = body_start;
            L->toks[hd->tok].cend = body_end;
        }
    }
    L->nheredoc = 0;
}

/* Scan a delimited body starting AT the opening delimiter p.
 * On return *pp points after the closing delimiter. */
static bool scan_delimited(plex_t *L, int *pp, unsigned char open, int *cs, int *ce)
{
    unsigned char close = open;
    bool nest = false;
    switch (open)
    {
    case '(': close = ')'; nest = true; break;
    case '[': close = ']'; nest = true; break;
    case '{': close = '}'; nest = true; break;
    case '<': close = '>'; nest = true; break;
    default: break;
    }
    int p = *pp + 1;
    *cs = p;
    int depth = 1;
    while (p < L->len)
    {
        unsigned char c = (unsigned char)L->s[p];
        if (c == '\\' && p + 1 < L->len)
        {
            if (L->s[p + 1] == '\n')
                L->line++;
            p += 2;
            continue;
        }
        if (c == '\n')
            L->line++;
        if (nest && c == open)
            depth++;
        else if (c == close)
        {
            if (--depth == 0)
            {
                *ce = p;
                *pp = p + 1;
                return true;
            }
        }
        p++;
    }
    *ce = L->len;
    *pp = L->len;
    return false;
}

/* Scan until the next unescaped delim (second part of s/a/b/ or tr/a/b/).
 * p points at the first char of the part. */
static bool scan_until(plex_t *L, int *pp, unsigned char delim, int *cs, int *ce)
{
    int p = *pp;
    *cs = p;
    while (p < L->len)
    {
        unsigned char c = (unsigned char)L->s[p];
        if (c == '\\' && p + 1 < L->len)
        {
            if (L->s[p + 1] == '\n')
                L->line++;
            p += 2;
            continue;
        }
        if (c == '\n')
            L->line++;
        if (c == delim)
        {
            *ce = p;
            *pp = p + 1;
            return true;
        }
        p++;
    }
    *ce = L->len;
    *pp = L->len;
    return false;
}

static void skip_ws_comments(plex_t *L, int *pp)
{
    int p = *pp;
    while (p < L->len)
    {
        unsigned char c = (unsigned char)L->s[p];
        if (c == '\n')
        {
            L->line++;
            p++;
        }
        else if (c == ' ' || c == '\t' || c == '\r' || c == '\f')
            p++;
        else if (c == '#')
        {
            while (p < L->len && L->s[p] != '\n')
                p++;
        }
        else
            break;
    }
    *pp = p;
}

static int quote_op_kind(const plex_t *L, int start, int wlen)
{
    const char *w = L->s + start;
    if (wlen == 1)
    {
        switch (w[0])
        {
        case 'q': return 1;
        case 'm': return 5;
        case 's': return 7;
        case 'y': return 8;
        default: return 0;
        }
    }
    if (wlen == 2)
    {
        if (w[0] == 'q' && w[1] == 'q') return 2;
        if (w[0] == 'q' && w[1] == 'w') return 3;
        if (w[0] == 'q' && w[1] == 'x') return 4;
        if (w[0] == 'q' && w[1] == 'r') return 6;
        if (w[0] == 't' && w[1] == 'r') return 8;
    }
    return 0;
}

static bool quote_op_allowed(const plex_t *L, int start)
{
    if (L->ntok == 0)
        return true;
    const ptok_t *prev = &L->toks[L->ntok - 1];
    if (prev->type == PT_OP)
    {
        if (span_eq(L, prev->start, prev->end, "->"))
            return false;
        /* -s $file is a file test, not s/// */
        if (span_eq(L, prev->start, prev->end, "-") && prev->end == start)
            return false;
    }
    if (prev->type == PT_IDENT && span_eq(L, prev->start, prev->end, "sub"))
        return false;
    return true;
}

/* Try to lex a quote-like operator whose keyword spans [start, wend). */
static bool lex_quote_like(plex_t *L, int start, int wend, int kind)
{
    int p = wend;
    bool had_ws = false;
    while (p < L->len && (L->s[p] == ' ' || L->s[p] == '\t'))
    {
        p++;
        had_ws = true;
    }
    unsigned char d = CH(L, p);
    if (d == 0 || d == '\n' || d == '\r' || is_id_char(d))
        return false;
    if (d == ',' || d == ';' || d == ')' || d == '}' || d == '=' || d == '>')
        return false;
    if (d == '#' && had_ws)
        return false;
    if (d == '-' && CH(L, p + 1) == '>')
        return false;
    /* Hash key: {s} {q} {y} */
    if (L->ntok > 0 && IS_OP(L, L->ntok - 1, "{"))
    {
        int q = wend;
        while (q < L->len && (L->s[q] == ' ' || L->s[q] == '\t'))
            q++;
        if (CH(L, q) == '}')
            return false;
    }

    int line0 = L->line;
    int c1s = 0, c1e = 0;
    scan_delimited(L, &p, d, &c1s, &c1e);

    bool two_part = (kind == 7 || kind == 8);
    if (two_part && p < L->len)
    {
        int c2s, c2e;
        bool bracketing = (d == '(' || d == '[' || d == '{' || d == '<');
        if (bracketing)
        {
            skip_ws_comments(L, &p);
            unsigned char d2 = CH(L, p);
            if (d2)
                scan_delimited(L, &p, d2, &c2s, &c2e);
        }
        else
        {
            scan_until(L, &p, d, &c2s, &c2e);
        }
    }
    /* Modifiers */
    while (p < L->len && isalpha((unsigned char)L->s[p]))
        p++;

    ptype_e type = PT_STR;
    unsigned char sub = STR_SQ;
    unsigned char interp = (d != '\'');
    switch (kind)
    {
    case 1: sub = STR_SQ; interp = 0; break;
    case 2: sub = STR_DQ; break;
    case 3: sub = STR_QW; interp = 0; break;
    case 4: sub = STR_CMD; break;
    case 5:
    case 6: type = PT_REGEX; sub = RX_MATCH; break;
    case 7: type = PT_REGEX; sub = RX_SUBST; break;
    case 8: type = PT_REGEX; sub = RX_TRANS; interp = 0; break;
    default: break;
    }
    int idx = push_tok(L, type, start, p, line0);
    if (idx >= 0)
    {
        L->toks[idx].sub = sub;
        L->toks[idx].interp = interp;
        L->toks[idx].cstart = c1s;
        L->toks[idx].cend = c1e;
    }
    L->pos = p;
    return true;
}

static int read_ident(const plex_t *L, int p)
{
    while (p < L->len)
    {
        unsigned char c = (unsigned char)L->s[p];
        if (is_id_char(c))
        {
            p++;
            continue;
        }
        if (c == ':' && CH(L, p + 1) == ':' && is_id_start(CH(L, p + 2)))
        {
            p += 2;
            continue;
        }
        break;
    }
    return p;
}

static void lex_word(plex_t *L)
{
    int start = L->pos;
    int p = read_ident(L, start);
    int wlen = p - start;
    int kind = quote_op_kind(L, start, wlen);
    if (kind && quote_op_allowed(L, start))
    {
        if (lex_quote_like(L, start, p, kind))
            return;
    }
    push_tok(L, PT_IDENT, start, p, L->line);
    L->pos = p;
}

static void lex_number(plex_t *L)
{
    int p = L->pos;
    if (CH(L, p) == '0' && (CH(L, p + 1) == 'x' || CH(L, p + 1) == 'X' ||
                            CH(L, p + 1) == 'b' || CH(L, p + 1) == 'B'))
    {
        p += 2;
        while (isxdigit(CH(L, p)) || CH(L, p) == '_')
            p++;
    }
    else
    {
        if (CH(L, p) == '.')
            p++;
        while (isdigit(CH(L, p)) || CH(L, p) == '_')
            p++;
        if (CH(L, p) == '.' && isdigit(CH(L, p + 1)))
        {
            p++;
            while (isdigit(CH(L, p)) || CH(L, p) == '_')
                p++;
        }
        if ((CH(L, p) == 'e' || CH(L, p) == 'E') &&
            (isdigit(CH(L, p + 1)) ||
             ((CH(L, p + 1) == '+' || CH(L, p + 1) == '-') && isdigit(CH(L, p + 2)))))
        {
            p += 2;
            while (isdigit(CH(L, p)))
                p++;
        }
    }
    if (p == L->pos)
        p++;
    push_tok(L, PT_NUM, L->pos, p, L->line);
    L->pos = p;
}

static void lex_string(plex_t *L, unsigned char q, unsigned char sub, unsigned char interp)
{
    int start = L->pos;
    int line0 = L->line;
    int p = start;
    int cs, ce;
    scan_delimited(L, &p, q, &cs, &ce);
    int idx = push_tok(L, PT_STR, start, p, line0);
    if (idx >= 0)
    {
        L->toks[idx].sub = sub;
        L->toks[idx].interp = interp;
        L->toks[idx].cstart = cs;
        L->toks[idx].cend = ce;
    }
    L->pos = p;
}

static const char *OPS3[] = {"<=>", "**=", "||=", "&&=", "//=", "...", "<<=", ">>=", NULL};
static const char *OPS2[] = {"->", "++", "--", "**", "=~", "!~", "==", "!=", "<=", ">=",
                             "&&", "||", "//", "..", "::", "+=", "-=", "*=", "/=", ".=",
                             "%=", "|=", "^=", "&=", "<<", ">>", "=>", NULL};

/* Kind of a '{' from the previous token: subscript, deref and anonymous
 * hash braces are expressions; everything else opens a block. */
static unsigned char brace_kind_for_open(const plex_t *L)
{
    if (L->ntok == 0)
        return BRACE_BLOCK;
    const ptok_t *t = &L->toks[L->ntok - 1];
    if (t->type == PT_VAR)
        return BRACE_EXPR;
    if (t->type == PT_IDENT)
        return span_eq(L, t->start, t->end, "return") ? BRACE_EXPR : BRACE_BLOCK;
    if (t->type != PT_OP)
        return BRACE_BLOCK;
    if (span_eq(L, t->start, t->end, "->") || span_eq(L, t->start, t->end, "=>") ||
        span_eq(L, t->start, t->end, "="))
        return BRACE_EXPR;
    if (t->end - t->start == 1)
    {
        char c = L->s[t->start];
        if (c == ']' || c == ',' || c == '(' || c == '[' || c == '?' || c == ':')
            return BRACE_EXPR;
        if (c == '}')
            return t->sub;
    }
    return BRACE_BLOCK;
}

static void lex_op(plex_t *L)
{
    int p = L->pos;
    for (const char **o = OPS3; *o; o++)
    {
        if (p + 3 <= L->len && memcmp(L->s + p, *o, 3) == 0)
        {
            push_tok(L, PT_OP, p, p + 3, L->line);
            L->pos = p + 3;
            return;
        }
    }
    for (const char **o = OPS2; *o; o++)
    {
        if (p + 2 <= L->len && memcmp(L->s + p, *o, 2) == 0)
        {
            push_tok(L, PT_OP, p, p + 2, L->line);
            L->pos = p + 2;
            return;
        }
    }
    char ch = L->s[p];
    unsigned char kind = BRACE_BLOCK;
    if (ch == '{')
        kind = brace_kind_for_open(L);
    else if (ch == '}' && L->nbkind > 0)
        kind = L->bkind[--L->nbkind];
    int idx = push_tok(L, PT_OP, p, p + 1, L->line);
    if (idx >= 0 && (ch == '{' || ch == '}'))
        L->toks[idx].sub = kind;
    if (ch == '{')
    {
        if (L->nbkind < (int)sizeof(L->bkind))
            L->bkind[L->nbkind] = kind;
        L->nbkind++;
        if (L->nbkind > (int)sizeof(L->bkind))
            L->nbkind = (int)sizeof(L->bkind);
    }
    L->pos = p + 1;
}

static void lex_scalar(plex_t *L)
{
    int start = L->pos;
    int p = start + 1;
    unsigned char c = CH(L, p);

    if (c == '#')
    {
        unsigned char n = CH(L, p + 1);
        if (n == '{' || n == '$')
            p += 1;
        else if (is_id_start(n))
            p = read_ident(L, p + 1);
        else
            p += 1;
    }
    else if (c == '$')
    {
        int q = p;
        while (CH(L, q) == '$')
            q++;
        if (is_id_start(CH(L, q)))
            p = read_ident(L, q);
        else if (CH(L, q) == '{')
            p = q;
        else
            p += 1; /* $$ (pid) */
    }
    else if (c == ':' && CH(L, p + 1) == ':')
    {
        p = read_ident(L, p + 2);
    }
    else if (is_id_start(c))
    {
        p = read_ident(L, p);
    }
    else if (isdigit(c))
    {
        while (isdigit(CH(L, p)))
            p++;
    }
    else if (c == '^' && isalpha(CH(L, p + 1)))
    {
        p += 2;
    }
    else if (c == '{')
    {
        /* ${name} / ${ expr }: VAR "$" followed by a brace token */
    }
    else if (c && strchr("&`'+!@/\\,;.<>|?\"~=%:-0", (int)c))
    {
        p += 1;
    }
    push_tok(L, PT_VAR, start, p, L->line);
    L->pos = p;
}

static void lex_sigil(plex_t *L, unsigned char sig)
{
    int start = L->pos;
    if (sig == '%' && prev_is_operand_sigil(L))
    {
        lex_op(L);
        return;
    }
    int p = start + 1;
    unsigned char c = CH(L, p);
    if (is_id_start(c) || (c == ':' && CH(L, p + 1) == ':'))
    {
        p = read_ident(L, c == ':' ? p + 2 : p);
    }
    else if (c == '$')
    {
        while (CH(L, p) == '$')
            p++;
        if (is_id_start(CH(L, p)))
            p = read_ident(L, p);
    }
    else if (c == '{')
    {
        /* @{...} / %{...} deref */
    }
    else if (sig == '@' && (c == '-' || c == '+'))
    {
        p += 1;
    }
    else if (sig == '%' && (c == '+' || c == '-' || c == '!'))
    {
        p += 1;
    }
    else if (sig == '%' && c == '^' && isupper(CH(L, p + 1)))
    {
        p += 2;
    }
    else
    {
        lex_op(L);
        return;
    }
    push_tok(L, PT_VAR, start, p, L->line);
    L->pos = p;
}

static void lex_amp(plex_t *L)
{
    int start = L->pos;
    unsigned char c = CH(L, start + 1);
    if (c == '&' || prev_is_operand_sigil(L))
    {
        lex_op(L);
        return;
    }
    int p = start + 1;
    if (is_id_start(c) || (c == ':' && CH(L, p + 1) == ':'))
        p = read_ident(L, c == ':' ? p + 2 : p);
    else if (c == '$')
    {
        while (CH(L, p) == '$')
            p++;
        if (is_id_start(CH(L, p)))
            p = read_ident(L, p);
    }
    else if (c != '{')
    {
        lex_op(L);
        return;
    }
    push_tok(L, PT_VAR, start, p, L->line);
    L->pos = p;
}

static void lex_star(plex_t *L)
{
    int start = L->pos;
    unsigned char c = CH(L, start + 1);
    if (c == '*' || prev_is_operand_sigil(L))
    {
        lex_op(L);
        return;
    }
    int p = start + 1;
    if (is_id_start(c) || (c == ':' && CH(L, p + 1) == ':'))
        p = read_ident(L, c == ':' ? p + 2 : p);
    else if (c != '{' && c != '$')
    {
        lex_op(L);
        return;
    }
    push_tok(L, PT_VAR, start, p, L->line);
    L->pos = p;
}

/* '<' : heredoc, <FH>/<STDIN>/<$fh> readline, or operator. */
static bool lex_angle(plex_t *L)
{
    int p = L->pos;
    if (CH(L, p + 1) == '<')
    {
        int q = p + 2;
        bool indent = false;
        if (CH(L, q) == '~')
        {
            indent = true;
            q++;
        }
        int qq = q;
        while (CH(L, qq) == ' ' || CH(L, qq) == '\t')
            qq++;
        unsigned char d = CH(L, qq);
        int ts, te, endp;
        unsigned char sub;
        unsigned char interp;
        if (d == '"' || d == '\'' || d == '`')
        {
            int e = qq + 1;
            while (e < L->len && L->s[e] != (char)d && L->s[e] != '\n')
                e++;
            if (CH(L, e) != d)
                return false;
            ts = qq + 1;
            te = e;
            endp = e + 1;
            sub = (d == '\'') ? STR_HEREDOC_SQ : (d == '`') ? STR_HEREDOC_CMD : STR_HEREDOC_DQ;
            interp = (d != '\'');
        }
        else if (qq == q && is_id_start(d))
        {
            int e = q;
            while (is_id_char(CH(L, e)))
                e++;
            ts = q;
            te = e;
            endp = e;
            sub = STR_HEREDOC_DQ;
            interp = 1;
        }
        else
        {
            return false;
        }
        int idx = push_tok(L, PT_STR, p, endp, L->line);
        if (idx >= 0)
        {
            L->toks[idx].sub = sub;
            L->toks[idx].interp = interp;
            L->toks[idx].cstart = endp;
            L->toks[idx].cend = endp;
            if (L->nheredoc < MAX_PENDING_HEREDOCS)
            {
                pheredoc_t *hd = &L->heredocs[L->nheredoc++];
                hd->tok = idx;
                hd->term_start = ts;
                hd->term_len = te - ts;
                hd->indent = indent;
            }
        }
        L->pos = endp;
        return true;
    }
    if (!prev_is_operand(L))
    {
        int q = p + 1;
        while (q < L->len && L->s[q] != '>' && L->s[q] != '\n' && L->s[q] != '<' &&
               L->s[q] != ';' && L->s[q] != '=' && (q - p) < 256)
            q++;
        if (CH(L, q) == '>')
        {
            int idx = push_tok(L, PT_READLINE, p, q + 1, L->line);
            if (idx >= 0)
            {
                L->toks[idx].cstart = p + 1;
                L->toks[idx].cend = q;
            }
            L->pos = q + 1;
            return true;
        }
    }
    return false;
}

static bool starts_marker(const plex_t *L, int p, const char *m)
{
    size_t n = strlen(m);
    if ((size_t)(L->len - p) < n || memcmp(L->s + p, m, n) != 0)
        return false;
    unsigned char after = CH(L, p + (int)n);
    return after == 0 || after == '\n' || after == '\r' || after == ' ' || after == '\t';
}

static void lex(plex_t *L)
{
    L->at_line_start = true;
    while (L->pos < L->len && !L->oom)
    {
        int p = L->pos;
        unsigned char c = (unsigned char)L->s[p];
        if (c == '\n')
        {
            L->pos++;
            L->line++;
            if (L->nheredoc > 0)
                read_heredocs(L);
            L->at_line_start = true;
            continue;
        }
        if (L->at_line_start)
        {
            L->at_line_start = false;
            if (c == '=' && is_id_start(CH(L, p + 1)))
            {
                lex_pod(L);
                continue;
            }
            if (c == '_' && (starts_marker(L, p, "__END__") || starts_marker(L, p, "__DATA__")))
            {
                scan_pod_tail(L);
                return;
            }
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v')
        {
            L->pos++;
            continue;
        }
        if (c == '#')
        {
            while (L->pos < L->len && L->s[L->pos] != '\n')
                L->pos++;
            continue;
        }
        if (is_id_start(c))
        {
            lex_word(L);
            continue;
        }
        if (isdigit(c) || (c == '.' && isdigit(CH(L, p + 1)) && !prev_is_operand(L)))
        {
            lex_number(L);
            continue;
        }
        switch (c)
        {
        case '$': lex_scalar(L); continue;
        case '@': lex_sigil(L, '@'); continue;
        case '%': lex_sigil(L, '%'); continue;
        case '&': lex_amp(L); continue;
        case '*': lex_star(L); continue;
        case '\'': lex_string(L, '\'', STR_SQ, 0); continue;
        case '"': lex_string(L, '"', STR_DQ, 1); continue;
        case '`': lex_string(L, '`', STR_CMD, 1); continue;
        case '/':
            if (!prev_is_operand(L))
            {
                int line0 = L->line;
                int q = p;
                int cs, ce;
                scan_delimited(L, &q, '/', &cs, &ce);
                while (q < L->len && isalpha((unsigned char)L->s[q]))
                    q++;
                int idx = push_tok(L, PT_REGEX, p, q, line0);
                if (idx >= 0)
                {
                    L->toks[idx].sub = RX_MATCH;
                    L->toks[idx].interp = 1;
                    L->toks[idx].cstart = cs;
                    L->toks[idx].cend = ce;
                }
                L->pos = q;
                continue;
            }
            break;
        case '<':
            if (lex_angle(L))
                continue;
            break;
        default:
            break;
        }
        lex_op(L);
    }
    if (L->nheredoc > 0)
        read_heredocs(L);
}

/* Match brackets and compute brace depth. */
static void match_brackets(plex_t *L)
{
    int cap = 256, n = 0;
    int *stack = malloc((size_t)cap * sizeof(int));
    if (!stack)
    {
        L->oom = true;
        return;
    }
    int depth = 0;
    for (int i = 0; i < L->ntok; i++)
    {
        ptok_t *t = &L->toks[i];
        t->depth = depth;
        if (t->type != PT_OP || t->end - t->start != 1)
            continue;
        char c = L->s[t->start];
        if (c == '(' || c == '[' || c == '{')
        {
            if (n >= cap)
            {
                cap *= 2;
                int *tmp = realloc(stack, (size_t)cap * sizeof(int));
                if (!tmp)
                {
                    L->oom = true;
                    break;
                }
                stack = tmp;
            }
            stack[n++] = i;
            if (c == '{')
                depth++;
        }
        else if (c == ')' || c == ']' || c == '}')
        {
            char want = (c == ')') ? '(' : (c == ']') ? '[' : '{';
            /* Find the nearest matching opener; discard unbalanced ones. */
            int k = n - 1;
            while (k >= 0 && L->s[L->toks[stack[k]].start] != want)
                k--;
            if (k < 0)
                continue;
            for (int j = n - 1; j >= k; j--)
            {
                if (L->s[L->toks[stack[j]].start] == '{' && depth > 0)
                    depth--;
            }
            int o = stack[k];
            L->toks[o].match = i;
            t->match = o;
            n = k;
            t->depth = depth;
        }
    }
    free(stack);
}

/* ===================================================================== */
/* Analyzer context                                                       */
/* ===================================================================== */

typedef struct
{
    int sub_tok;  /* 'sub' keyword */
    int name_tok; /* name token */
    int proto;    /* '(' of prototype/signature or -1 */
    int open;     /* '{' or -1 (forward declaration) */
    int close;    /* matching '}' or the ';' of a forward declaration */
    char *name;   /* bare name */
    char *qname;  /* package-qualified name ("name" in main) */
    char *pkg;    /* package or NULL (main) */
    char *id;     /* symbol ID */
    bool forward;
    bool is_method;
} psub_t;

typedef struct
{
    int tok;     /* 'package' keyword */
    int end_tok; /* last token covered */
    int open;    /* '{' for block form, -1 otherwise */
    char *name;
    char *id;
} ppkg_t;

typedef struct
{
    uint32_t mask;
    int line; /* first line where a source bit arrived */
} pvar_t;

typedef struct
{
    const char *rel_path;
    plex_t *L;
    tt_perl_result_t *out;
    tt_line_offsets_t lo;
    psub_t *subs;
    int nsub;
    ppkg_t *pkgs;
    int npkg;
    int *tok_sub;     /* per token: innermost named sub index or -1 */
    int *shift_param; /* per token: param index for bare 'shift', -1 */
    tt_hashmap_t **scope_vars; /* [nsub + 1]: per-scope var map (0 = file) */
    tt_hashmap_t *id_counts;
    bool oom;
} pctx_t;

static char *tok_text(const pctx_t *c, int i)
{
    const ptok_t *t = &c->L->toks[i];
    return tt_strndup(c->L->s + t->start, (size_t)(t->end - t->start));
}

static int tok_line(const pctx_t *c, int i)
{
    if (i < 0)
        return 1;
    if (i >= c->L->ntok)
        return c->L->ntok > 0 ? c->L->toks[c->L->ntok - 1].line : 1;
    return c->L->toks[i].line;
}

/* Collapse whitespace of [start,end) into a heap string, max bytes. */
static char *collapse_text(const char *s, int start, int end, size_t max)
{
    tt_strbuf_t sb;
    tt_strbuf_init(&sb);
    bool sp = false;
    for (int i = start; i < end; i++)
    {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '\n' || ch == '\t' || ch == '\r' || ch == ' ')
        {
            sp = true;
            continue;
        }
        if (sp && sb.len > 0)
            tt_strbuf_append_char(&sb, ' ');
        sp = false;
        tt_strbuf_append_char(&sb, (char)ch);
        if (sb.len >= max + 8)
            break;
    }
    char *r = tt_strbuf_detach(&sb);
    if (r && strlen(r) > max)
    {
        tt_utf8_truncate(r, max);
        size_t l = strlen(r);
        char *r2 = realloc(r, l + 4);
        if (r2)
        {
            memcpy(r2 + l, "...", 4);
            r = r2;
        }
    }
    return r ? r : tt_strdup("");
}

/* Literal value of a string token: returns heap string or NULL when the
 * token interpolates variables (or is not a string). */
static char *str_literal(const pctx_t *c, int i, bool *interpolated)
{
    if (interpolated)
        *interpolated = false;
    if (i < 0 || i >= c->L->ntok)
        return NULL;
    const ptok_t *t = &c->L->toks[i];
    if (t->type != PT_STR || t->sub == STR_QW)
        return NULL;
    const char *s = c->L->s;
    tt_strbuf_t sb;
    tt_strbuf_init(&sb);
    bool interp = false;
    for (int p = t->cstart; p < t->cend; p++)
    {
        char ch = s[p];
        if (ch == '\\' && p + 1 < t->cend)
        {
            char n = s[p + 1];
            if (t->interp)
            {
                if (n == 'n') tt_strbuf_append_char(&sb, '\n');
                else if (n == 't') tt_strbuf_append_char(&sb, '\t');
                else tt_strbuf_append_char(&sb, n);
            }
            else
            {
                if (n == '\\' || n == '\'')
                    tt_strbuf_append_char(&sb, n);
                else
                {
                    tt_strbuf_append_char(&sb, ch);
                    tt_strbuf_append_char(&sb, n);
                }
            }
            p++;
            continue;
        }
        if (t->interp && (ch == '$' || ch == '@') && p + 1 < t->cend &&
            (is_id_start((unsigned char)s[p + 1]) || s[p + 1] == '{' ||
             s[p + 1] == ':'))
            interp = true;
        tt_strbuf_append_char(&sb, ch);
    }
    if (interpolated)
        *interpolated = interp;
    return tt_strbuf_detach(&sb);
}

/* ===================================================================== */
/* Symbols                                                                */
/* ===================================================================== */

static char *unique_id(pctx_t *c, const char *qname, tt_symbol_kind_e kind)
{
    char *id = tt_symbol_make_id(c->rel_path, qname, tt_kind_to_str(kind), 0);
    if (!id)
        return NULL;
    intptr_t n = (intptr_t)tt_hashmap_get(c->id_counts, id);
    tt_hashmap_set(c->id_counts, id, (void *)(n + 1));
    if (n > 0)
    {
        char *id2 = tt_symbol_make_id(c->rel_path, qname, tt_kind_to_str(kind), (int)n);
        free(id);
        id = id2;
    }
    return id;
}

static int add_symbol(pctx_t *c, const char *id_in, const char *name, const char *qname,
                      tt_symbol_kind_e kind, int line, int end_line,
                      const char *sig, const char *doc, const char *parent_id,
                      const char *decorators)
{
    tt_perl_result_t *o = c->out;
    if (tt_parser_grow(&o->symbols, &o->sym_count, &o->sym_cap) < 0)
        return -1;
    if (line < 1)
        line = 1;
    if (end_line < line)
        end_line = line;
    if (end_line > c->lo.count)
        end_line = c->lo.count > 0 ? c->lo.count : line;

    int off = tt_line_offsets_offset_at(&c->lo, line);
    int end_off = (end_line < c->lo.count) ? tt_line_offsets_offset_at(&c->lo, end_line + 1)
                                           : c->L->len;
    if (end_off < off)
        end_off = off;

    tt_symbol_t *sym = &o->symbols[o->sym_count];
    memset(sym, 0, sizeof(*sym));
    sym->id = id_in ? tt_strdup(id_in) : unique_id(c, qname, kind);
    sym->file = tt_strdup(c->rel_path);
    sym->name = tt_strdup(name);
    sym->qualified_name = tt_strdup(qname);
    sym->kind = kind;
    sym->language = tt_strdup("perl");
    sym->signature = tt_strdup(sig ? sig : "");
    sym->docstring = tt_strdup(doc ? doc : "");
    sym->summary = tt_strdup("");
    sym->decorators = tt_strdup(decorators ? decorators : "[]");
    sym->keywords = tt_parser_extract_keywords(qname);
    sym->parent_id = parent_id ? tt_strdup(parent_id) : NULL;
    sym->line = line;
    sym->end_line = end_line;
    sym->byte_offset = off;
    sym->byte_length = end_off - off;
    sym->content_hash = (end_off > off) ? tt_fast_hash_hex(c->L->s + off, (size_t)(end_off - off))
                                        : tt_strdup("");
    if (!sym->content_hash)
        sym->content_hash = tt_strdup("");
    if (!sym->id || !sym->file || !sym->name)
    {
        tt_symbol_free(sym);
        return -1;
    }
    o->sym_count++;
    return 0;
}

/* Index of the first token after a statement starting at i (the ';' or
 * the unmatched closer / EOF). */
static int stmt_end(const pctx_t *c, int i)
{
    const plex_t *L = c->L;
    while (i < L->ntok)
    {
        const ptok_t *t = &L->toks[i];
        if (t->type == PT_OP && t->end - t->start == 1)
        {
            char ch = L->s[t->start];
            if (ch == ';')
                return i;
            if (ch == '(' || ch == '[' || ch == '{')
            {
                if (t->match > i)
                {
                    i = t->match + 1;
                    continue;
                }
                return i;
            }
            if (ch == ')' || ch == ']' || ch == '}')
                return i;
        }
        i++;
    }
    return L->ntok;
}

static bool is_low_prec_keyword(const pctx_t *c, int i)
{
    static const char *KW[] = {"or", "and", "xor", "if", "unless", "while", "until",
                               "foreach", "for", NULL};
    if (c->L->toks[i].type != PT_IDENT)
        return false;
    for (const char **k = KW; *k; k++)
    {
        if (IS_IDENT(c->L, i, *k))
            return true;
    }
    return false;
}

/* End (exclusive) of an expression starting at i: stops at ';', an
 * unmatched closer, a low-precedence keyword at depth 0, or (when
 * stop_comma) a ',' / '=>' at depth 0. */
static int expr_end(const pctx_t *c, int i, bool stop_comma)
{
    const plex_t *L = c->L;
    while (i < L->ntok)
    {
        const ptok_t *t = &L->toks[i];
        if (t->type == PT_OP)
        {
            int n = t->end - t->start;
            char ch = L->s[t->start];
            if (n == 1)
            {
                if (ch == ';')
                    return i;
                if (ch == '(' || ch == '[' || ch == '{')
                {
                    if (t->match > i)
                    {
                        i = t->match + 1;
                        continue;
                    }
                    return i;
                }
                if (ch == ')' || ch == ']' || ch == '}')
                    return i;
                if (ch == ',' && stop_comma)
                    return i;
            }
            else if (stop_comma && span_eq(L, t->start, t->end, "=>"))
            {
                return i;
            }
        }
        else if (is_low_prec_keyword(c, i))
        {
            return i;
        }
        i++;
    }
    return L->ntok;
}

/* Split [a,b) at depth-0 commas. Writes up to max ranges. */
static int split_args(const pctx_t *c, int a, int b, int (*ranges)[2], int max)
{
    const plex_t *L = c->L;
    int n = 0;
    int start = a;
    int i = a;
    while (i < b)
    {
        const ptok_t *t = &L->toks[i];
        if (t->type == PT_OP)
        {
            int len = t->end - t->start;
            char ch = L->s[t->start];
            if (len == 1 && (ch == '(' || ch == '[' || ch == '{') && t->match > i && t->match < b)
            {
                i = t->match + 1;
                continue;
            }
            if ((len == 1 && ch == ',') || span_eq(L, t->start, t->end, "=>"))
            {
                if (n < max && i > start)
                {
                    ranges[n][0] = start;
                    ranges[n][1] = i;
                    n++;
                }
                start = i + 1;
            }
        }
        i++;
    }
    if (n < max && b > start)
    {
        ranges[n][0] = start;
        ranges[n][1] = b;
        n++;
    }
    return n;
}

/* Argument token range of a call whose name token is at n.
 * Returns false when no argument list follows. */
static bool call_args(const pctx_t *c, int n, int *a, int *b)
{
    const plex_t *L = c->L;
    if (n + 1 < L->ntok && IS_OP(L, n + 1, "(") && L->toks[n + 1].match > n + 1)
    {
        *a = n + 2;
        *b = L->toks[n + 1].match;
        return true;
    }
    *a = n + 1;
    *b = expr_end(c, n + 1, false);
    return *b > *a;
}

static char *range_text(const pctx_t *c, int a, int b, size_t max)
{
    if (a >= b || a < 0 || b > c->L->ntok)
        return tt_strdup("");
    int s = c->L->toks[a].start;
    int e = c->L->toks[b - 1].end;
    return collapse_text(c->L->s, s, e, max);
}

/* ---- POD association ---- */

static const ppod_block_t *pod_before_line(const pctx_t *c, int line)
{
    const plex_t *L = c->L;
    const ppod_block_t *best = NULL;
    for (int i = 0; i < L->npod; i++)
    {
        if (L->pods[i].end_line < line)
            best = &L->pods[i];
        else
            break;
    }
    if (!best)
        return NULL;
    /* Only blank or comment lines may separate the POD from the symbol. */
    for (int ln = best->end_line + 1; ln < line; ln++)
    {
        int off = tt_line_offsets_offset_at(&c->lo, ln);
        int next = (ln < c->lo.count) ? tt_line_offsets_offset_at(&c->lo, ln + 1) : L->len;
        for (int p = off; p < next; p++)
        {
            char ch = L->s[p];
            if (ch == '#')
                break;
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
                return NULL;
        }
    }
    return best;
}

static char *pod_docstring(const pctx_t *c, const ppod_block_t *b)
{
    /* Text between the first line (directive) and the =cut line. */
    int from = b->start_line + 1;
    int to = b->end_line - 1;
    if (to < from)
        return tt_strdup("");
    int s = tt_line_offsets_offset_at(&c->lo, from);
    int e = (to < c->lo.count) ? tt_line_offsets_offset_at(&c->lo, to + 1) : c->L->len;
    if (e < s)
        return tt_strdup("");
    return collapse_text(c->L->s, s, e, 400);
}

/*
 * pod_docstring_for -- Docstring of a named sub from the POD block above it:
 * the paragraph after the =headN/=item naming it, else the paragraph after
 * the block's last command (closest to the sub), never the whole block.
 */
static char *pod_docstring_for(const pctx_t *c, const ppod_block_t *b, const char *name)
{
    const plex_t *L = c->L;
    size_t nlen = name ? strlen(name) : 0;
    if (nlen == 0)
        return pod_docstring(c, b);
    int match = -1, last_cmd = -1;
    for (int ln = b->start_line; ln < b->end_line; ln++)
    {
        int off = tt_line_offsets_offset_at(&c->lo, ln);
        int next = (ln < c->lo.count) ? tt_line_offsets_offset_at(&c->lo, ln + 1) : L->len;
        if (off < 0 || off >= L->len || L->s[off] != '=')
            continue;
        last_cmd = ln;
        if (next - off < 6 || (strncmp(L->s + off, "=head", 5) != 0 && strncmp(L->s + off, "=item", 5) != 0))
            continue;
        int p = off + 5;
        while (p < next && !isspace((unsigned char)L->s[p]))
            p++;
        while (p < next && (L->s[p] == ' ' || L->s[p] == '\t' || L->s[p] == '*' || L->s[p] == '&'))
            p++;
        if (p + (int)nlen <= next && strncmp(L->s + p, name, nlen) == 0 &&
            (p + (int)nlen == next || !is_id_char((unsigned char)L->s[p + nlen])))
            match = ln;
    }
    int from = (match >= 0 ? match : last_cmd) + 1;
    if (last_cmd < 0)
        from = b->start_line + 1;
    int to = b->end_line - 1;
    for (int ln = from; ln <= to; ln++)
    {
        int off = tt_line_offsets_offset_at(&c->lo, ln);
        if (off >= 0 && off < L->len && L->s[off] == '=')
        {
            to = ln - 1;
            break;
        }
    }
    if (to < from)
        return tt_strdup("");
    int s = tt_line_offsets_offset_at(&c->lo, from);
    int e = (to < c->lo.count) ? tt_line_offsets_offset_at(&c->lo, to + 1) : L->len;
    if (e < s)
        return tt_strdup("");
    return collapse_text(L->s, s, e, 400);
}

static char *comment_docstring(const pctx_t *c, int line)
{
    int ln = line - 1;
    int first = line;
    while (ln >= 1)
    {
        int off = tt_line_offsets_offset_at(&c->lo, ln);
        int p = off;
        while (p < c->L->len && (c->L->s[p] == ' ' || c->L->s[p] == '\t'))
            p++;
        if (p < c->L->len && c->L->s[p] == '#' && CH(c->L, p + 1) != '!')
        {
            first = ln;
            ln--;
            continue;
        }
        break;
    }
    if (first == line)
        return tt_strdup("");
    tt_strbuf_t sb;
    tt_strbuf_init(&sb);
    for (int l = first; l < line; l++)
    {
        int off = tt_line_offsets_offset_at(&c->lo, l);
        int end = (l < c->lo.count) ? tt_line_offsets_offset_at(&c->lo, l + 1) : c->L->len;
        int p = off;
        while (p < end && (c->L->s[p] == ' ' || c->L->s[p] == '\t' || c->L->s[p] == '#'))
            p++;
        if (sb.len > 0)
            tt_strbuf_append_char(&sb, ' ');
        tt_strbuf_append(&sb, c->L->s + p, (size_t)(end - p));
    }
    char *raw = tt_strbuf_detach(&sb);
    if (!raw)
        return tt_strdup("");
    char *r = collapse_text(raw, 0, (int)strlen(raw), 400);
    free(raw);
    return r;
}

/* ---- Signatures ---- */

static char *join_params(const char *name, char params[][64], int np)
{
    tt_strbuf_t sb;
    tt_strbuf_init(&sb);
    tt_strbuf_appendf(&sb, "%s(", name);
    for (int i = 0; i < np; i++)
    {
        if (i > 0)
            tt_strbuf_append_str(&sb, ", ");
        tt_strbuf_append_str(&sb, params[i]);
    }
    tt_strbuf_append_char(&sb, ')');
    return tt_strbuf_detach(&sb);
}

/* Literal parameter names from the first statements of the body:
 *   my ($a, $b) = @_;   local ($a, $b) = @_;
 *   my $a = shift; my $b = shift;   my $a = $_[0]; */
static int body_params(const pctx_t *c, const psub_t *sub, char params[][64], int max)
{
    const plex_t *L = c->L;
    if (sub->open < 0)
        return 0;
    int i = sub->open + 1;
    int end = sub->close;
    int np = 0;

    if (i < end && (IS_IDENT(L, i, "my") || IS_IDENT(L, i, "local")) &&
        IS_OP(L, i + 1, "(") && L->toks[i + 1].match > i + 1)
    {
        int m = L->toks[i + 1].match;
        if (IS_OP(L, m + 1, "=") && tok_is(L, m + 2, PT_VAR, "@_"))
        {
            for (int k = i + 2; k < m && np < max; k++)
            {
                if (L->toks[k].type == PT_VAR)
                {
                    int len = L->toks[k].end - L->toks[k].start;
                    if (len > 63)
                        len = 63;
                    memcpy(params[np], L->s + L->toks[k].start, (size_t)len);
                    params[np][len] = '\0';
                    np++;
                }
            }
            return np;
        }
        return 0;
    }

    /* my $x = shift; / my $x = $_[N]; sequences */
    while (i + 3 < end && np < max &&
           (IS_IDENT(L, i, "my") || IS_IDENT(L, i, "local")) &&
           L->toks[i + 1].type == PT_VAR && IS_OP(L, i + 2, "="))
    {
        int v = i + 1;
        int k = i + 3;
        bool ok = false;
        if (IS_IDENT(L, k, "shift"))
        {
            k++;
            if (tok_is(L, k, PT_VAR, "@_"))
                k++;
            else if (IS_OP(L, k, "(") && L->toks[k].match > k)
                k = L->toks[k].match + 1;
            ok = true;
        }
        else if (tok_is(L, k, PT_VAR, "$_") && IS_OP(L, k + 1, "[") &&
                 L->toks[k + 2].type == PT_NUM && IS_OP(L, k + 3, "]"))
        {
            k += 4;
            ok = true;
        }
        if (!ok || !IS_OP(L, k, ";"))
            break;
        int len = L->toks[v].end - L->toks[v].start;
        if (len > 63)
            len = 63;
        memcpy(params[np], L->s + L->toks[v].start, (size_t)len);
        params[np][len] = '\0';
        np++;
        i = k + 1;
    }
    return np;
}

static char *sub_signature(const pctx_t *c, const psub_t *sub, char params[][64], int np)
{
    const plex_t *L = c->L;
    if (np > 0)
        return join_params(sub->name, params, np);

    /* Perl 5.20+ signature: sub f ($a, $b) { */
    if (sub->proto >= 0 && L->toks[sub->proto].match > sub->proto)
    {
        int m = L->toks[sub->proto].match;
        char sp[TT_TAINT_MAX_PARAMS][64];
        int n = 0;
        for (int k = sub->proto + 1; k < m && n < TT_TAINT_MAX_PARAMS; k++)
        {
            if (L->toks[k].type == PT_VAR && L->toks[k].end - L->toks[k].start > 1)
            {
                int len = L->toks[k].end - L->toks[k].start;
                if (len > 63)
                    len = 63;
                memcpy(sp[n], L->s + L->toks[k].start, (size_t)len);
                sp[n][len] = '\0';
                n++;
            }
        }
        if (n > 0)
            return join_params(sub->name, sp, n);
    }

    /* POD heading "=head2 name(args)" immediately above. */
    int line = tok_line(c, sub->sub_tok);
    const ppod_block_t *pb = pod_before_line(c, line);
    if (pb)
    {
        size_t nlen = strlen(sub->name);
        for (int h = 0; h < L->nhead; h++)
        {
            const ppod_head_t *hd = &L->heads[h];
            if (hd->line < pb->start_line || hd->line > pb->end_line)
                continue;
            const char *t = L->s + hd->text_start;
            if ((size_t)hd->text_len > nlen && memcmp(t, sub->name, nlen) == 0 &&
                t[nlen] == '(')
            {
                int len = hd->text_len;
                const char *close = memchr(t, ')', (size_t)len);
                if (close)
                    len = (int)(close - t) + 1;
                return collapse_text(t, 0, len, 200);
            }
        }
    }

    tt_strbuf_t sb;
    tt_strbuf_init(&sb);
    tt_strbuf_appendf(&sb, "sub %s", sub->name);
    return tt_strbuf_detach(&sb);
}

/* ===================================================================== */
/* Structure pre-pass: packages and subs                                  */
/* ===================================================================== */

static bool is_hash_key_ctx(const plex_t *L, int i)
{
    /* {word} or word => */
    if (IS_OP(L, i + 1, "=>"))
        return true;
    if (i > 0 && IS_OP(L, i - 1, "{") && IS_OP(L, i + 1, "}"))
        return true;
    return false;
}

static void find_structure(pctx_t *c)
{
    plex_t *L = c->L;
    int capsub = 0, cappkg = 0;

    for (int i = 0; i < L->ntok && !c->oom; i++)
    {
        const ptok_t *t = &L->toks[i];
        if (t->type != PT_IDENT)
            continue;
        if (i > 0 && IS_OP(L, i - 1, "->"))
            continue;
        if (is_hash_key_ctx(L, i))
            continue;

        if (span_eq(L, t->start, t->end, "package") && i + 1 < L->ntok &&
            L->toks[i + 1].type == PT_IDENT)
        {
            int k = i + 2;
            if (k < L->ntok && L->toks[k].type == PT_NUM)
                k++;
            if (c->npkg >= cappkg)
            {
                cappkg = cappkg ? cappkg * 2 : 4;
                ppkg_t *p = realloc(c->pkgs, (size_t)cappkg * sizeof(ppkg_t));
                if (!p)
                {
                    c->oom = true;
                    return;
                }
                c->pkgs = p;
            }
            ppkg_t *pk = &c->pkgs[c->npkg++];
            memset(pk, 0, sizeof(*pk));
            pk->tok = i;
            pk->name = tok_text(c, i + 1);
            pk->open = -1;
            pk->end_tok = L->ntok - 1;
            if (IS_OP(L, k, "{") && L->toks[k].match > k)
            {
                pk->open = k;
                pk->end_tok = L->toks[k].match;
            }
            else
            {
                /* Statement form: ends at the enclosing block close. */
                int d = t->depth;
                for (int j = i + 1; j < L->ntok; j++)
                {
                    if (L->toks[j].depth < d)
                    {
                        pk->end_tok = j - 1;
                        break;
                    }
                }
                /* ...or at the next statement-form package at same depth. */
                for (int j = 0; j < c->npkg - 1; j++)
                {
                    ppkg_t *prev = &c->pkgs[j];
                    if (prev->open < 0 && prev->end_tok >= i && L->toks[prev->tok].depth == d)
                        prev->end_tok = i - 1;
                }
            }
            continue;
        }

        if (span_eq(L, t->start, t->end, "sub") && i + 1 < L->ntok &&
            L->toks[i + 1].type == PT_IDENT)
        {
            int k = i + 2;
            int proto = -1;
            if (IS_OP(L, k, "(") && L->toks[k].match > k)
            {
                proto = k;
                k = L->toks[k].match + 1;
            }
            while (IS_OP(L, k, ":"))
            {
                k++;
                if (k < L->ntok && L->toks[k].type == PT_IDENT)
                    k++;
                if (IS_OP(L, k, "(") && L->toks[k].match > k)
                    k = L->toks[k].match + 1;
            }
            int open = -1, close = -1;
            bool forward = false;
            if (IS_OP(L, k, "{") && L->toks[k].match > k)
            {
                open = k;
                close = L->toks[k].match;
            }
            else if (IS_OP(L, k, ";"))
            {
                forward = true;
                close = k;
            }
            else
            {
                continue;
            }
            if (c->nsub >= capsub)
            {
                capsub = capsub ? capsub * 2 : 64;
                psub_t *s = realloc(c->subs, (size_t)capsub * sizeof(psub_t));
                if (!s)
                {
                    c->oom = true;
                    return;
                }
                c->subs = s;
            }
            psub_t *s = &c->subs[c->nsub++];
            memset(s, 0, sizeof(*s));
            s->sub_tok = i;
            s->name_tok = i + 1;
            s->proto = proto;
            s->open = open;
            s->close = close;
            s->forward = forward;
        }
    }
}

static const ppkg_t *pkg_at(const pctx_t *c, int i)
{
    const ppkg_t *best = NULL;
    for (int k = 0; k < c->npkg; k++)
    {
        const ppkg_t *p = &c->pkgs[k];
        if (p->tok < i && p->end_tok >= i)
            best = p; /* later (inner) packages win */
    }
    return best;
}

/* ===================================================================== */
/* Symbol emission                                                        */
/* ===================================================================== */

static void emit_packages(pctx_t *c)
{
    for (int k = 0; k < c->npkg; k++)
    {
        ppkg_t *p = &c->pkgs[k];
        int line = tok_line(c, p->tok);
        int end_line = (p->open >= 0) ? tok_line(c, p->end_tok) : line;
        tt_strbuf_t sb;
        tt_strbuf_init(&sb);
        tt_strbuf_appendf(&sb, "package %s", p->name ? p->name : "");
        char *sig = tt_strbuf_detach(&sb);
        p->id = unique_id(c, p->name ? p->name : "", TT_KIND_NAMESPACE);
        char *doc = NULL;
        const ppod_block_t *pb = pod_before_line(c, line);
        doc = pb ? pod_docstring(c, pb) : comment_docstring(c, line);
        add_symbol(c, p->id, p->name ? p->name : "", p->name ? p->name : "",
                   TT_KIND_NAMESPACE, line, end_line, sig, doc, NULL, NULL);
        free(sig);
        free(doc);
    }
}

static void emit_subs(pctx_t *c)
{
    plex_t *L = c->L;
    for (int k = 0; k < c->nsub; k++)
    {
        psub_t *s = &c->subs[k];
        char *raw = tok_text(c, s->name_tok);
        if (!raw)
            continue;
        const ppkg_t *pk = pkg_at(c, s->sub_tok);
        const char *last = raw;
        for (const char *q = raw; *q; q++)
        {
            if (q[0] == ':' && q[1] == ':')
                last = q + 2;
        }
        s->name = tt_strdup(last);
        if (last != raw)
        {
            s->qname = tt_strdup(raw);
            s->pkg = tt_strndup(raw, (size_t)(last - raw - 2));
        }
        else if (pk && pk->name && strcmp(pk->name, "main") != 0)
        {
            tt_strbuf_t sb;
            tt_strbuf_init(&sb);
            tt_strbuf_appendf(&sb, "%s::%s", pk->name, raw);
            s->qname = tt_strbuf_detach(&sb);
            s->pkg = tt_strdup(pk->name);
        }
        else
        {
            s->qname = tt_strdup(raw);
        }
        free(raw);

        char params[TT_TAINT_MAX_PARAMS][64];
        int np = body_params(c, s, params, TT_TAINT_MAX_PARAMS);
        s->is_method = (s->pkg && np > 0 &&
                        (strcmp(params[0], "$self") == 0 || strcmp(params[0], "$class") == 0 ||
                         strcmp(params[0], "$this") == 0));
        tt_symbol_kind_e kind = s->is_method ? TT_KIND_METHOD : TT_KIND_SUBROUTINE;

        char *sig = sub_signature(c, s, params, np);
        int line = tok_line(c, s->sub_tok);
        int end_line = tok_line(c, s->close >= 0 ? s->close : s->name_tok);

        const ppod_block_t *pb = pod_before_line(c, line);
        char *doc = pb ? pod_docstring_for(c, pb, s->name) : comment_docstring(c, line);

        const char *parent = NULL;
        if (s->pkg)
        {
            for (int p = 0; p < c->npkg; p++)
            {
                if (c->pkgs[p].name && strcmp(c->pkgs[p].name, s->pkg) == 0)
                {
                    parent = c->pkgs[p].id;
                    break;
                }
            }
        }

        s->id = unique_id(c, s->qname ? s->qname : s->name, kind);
        add_symbol(c, s->id, s->name, s->qname ? s->qname : s->name, kind, line, end_line,
                   sig, doc, parent, s->forward ? "[\"forward\"]" : NULL);
        free(sig);
        free(doc);
    }

    /* Innermost named sub per token. */
    for (int k = 0; k < c->nsub; k++)
    {
        const psub_t *s = &c->subs[k];
        if (s->open < 0)
            continue;
        for (int i = s->open; i <= s->close && i < L->ntok; i++)
            c->tok_sub[i] = k;
    }
}

static void emit_pod_headings(pctx_t *c)
{
    plex_t *L = c->L;
    for (int h = 0; h < L->nhead; h++)
    {
        const ppod_head_t *hd = &L->heads[h];
        int end_line = L->pods[hd->block].end_line;
        for (int n = h + 1; n < L->nhead; n++)
        {
            if (L->heads[n].block != hd->block)
                break;
            if (L->heads[n].level <= hd->level)
            {
                end_line = L->heads[n].line - 1;
                break;
            }
        }
        char *name = collapse_text(L->s, hd->text_start, hd->text_start + hd->text_len, 120);
        tt_symbol_kind_e kind = (hd->level <= 1) ? TT_KIND_SECTION : TT_KIND_SUBSECTION;
        add_symbol(c, NULL, name, name, kind, hd->line, end_line, "", "", NULL, NULL);
        free(name);
    }
}

/* File-scope my/our/local variables, use constant, directives. */
static void emit_statement_symbols(pctx_t *c)
{
    plex_t *L = c->L;
    for (int i = 0; i < L->ntok && !c->oom; i++)
    {
        const ptok_t *t = &L->toks[i];
        if (t->type != PT_IDENT)
            continue;
        if (i > 0 && (IS_OP(L, i - 1, "->") || IS_OP(L, i - 1, "{")))
            continue;
        if (IS_OP(L, i + 1, "=>"))
            continue;

        bool is_decl = (span_eq(L, t->start, t->end, "my") || span_eq(L, t->start, t->end, "our") ||
                        span_eq(L, t->start, t->end, "local"));
        if (is_decl && i > 0 && (IS_IDENT(L, i - 1, "foreach") || IS_IDENT(L, i - 1, "for")))
            continue;
        if (is_decl && t->depth == 0 && c->tok_sub[i] < 0)
        {
            int end = stmt_end(c, i);
            int last_line = tok_line(c, end < L->ntok ? end : L->ntok - 1);
            const ppkg_t *pk = pkg_at(c, i);
            int k = i + 1;
            int stop = k + 1;
            if (IS_OP(L, k, "(") && L->toks[k].match > k)
            {
                stop = L->toks[k].match;
                k++;
            }
            for (; k < stop && k < L->ntok; k++)
            {
                if (L->toks[k].type != PT_VAR)
                    continue;
                char *name = tok_text(c, k);
                if (!name)
                    continue;
                char *qname = name;
                char *tmp = NULL;
                if (pk && pk->name && strcmp(pk->name, "main") != 0 &&
                    span_eq(L, t->start, t->end, "our"))
                {
                    tt_strbuf_t sb;
                    tt_strbuf_init(&sb);
                    tt_strbuf_appendf(&sb, "%c%s::%s", name[0], pk->name, name + 1);
                    tmp = tt_strbuf_detach(&sb);
                    qname = tmp;
                }
                char *sig = range_text(c, i, end, 160);
                add_symbol(c, NULL, name, qname, TT_KIND_VARIABLE, tok_line(c, i), last_line,
                           sig, "", pk ? pk->id : NULL, NULL);
                free(sig);
                free(tmp);
                free(name);
            }
            continue;
        }

        bool is_use = span_eq(L, t->start, t->end, "use");
        bool is_req = span_eq(L, t->start, t->end, "require");
        bool is_do = span_eq(L, t->start, t->end, "do");
        if (!is_use && !is_req && !is_do)
            continue;
        if (is_do && !(i + 1 < L->ntok && L->toks[i + 1].type == PT_STR))
            continue; /* do BLOCK / do SUB */

        int end = stmt_end(c, i);
        int last_line = tok_line(c, end < L->ntok ? end : L->ntok - 1);
        char *target = NULL;
        if (i + 1 < L->ntok)
        {
            const ptok_t *n = &L->toks[i + 1];
            if (n->type == PT_IDENT || n->type == PT_NUM)
                target = tok_text(c, i + 1);
            else if (n->type == PT_STR)
            {
                bool interp;
                target = str_literal(c, i + 1, &interp);
            }
            else if (n->type == PT_VAR)
                target = tok_text(c, i + 1);
        }
        if (!target || !target[0])
        {
            free(target);
            continue;
        }

        /* use constant NAME => ... / use constant { A => 1, ... } */
        if (is_use && strcmp(target, "constant") == 0)
        {
            int k = i + 2;
            if (IS_OP(L, k, "{") && L->toks[k].match > k)
            {
                int m = L->toks[k].match;
                for (int j = k + 1; j < m; j++)
                {
                    if ((L->toks[j].type == PT_IDENT || L->toks[j].type == PT_STR) &&
                        IS_OP(L, j + 1, "=>") && L->toks[j].depth == L->toks[k].depth + 1)
                    {
                        char *cn = (L->toks[j].type == PT_IDENT) ? tok_text(c, j)
                                                                 : str_literal(c, j, NULL);
                        if (cn && cn[0])
                        {
                            char *sig = range_text(c, j, expr_end(c, j, true), 160);
                            add_symbol(c, NULL, cn, cn, TT_KIND_CONSTANT, tok_line(c, j),
                                       tok_line(c, j), sig, "", NULL, NULL);
                            free(sig);
                        }
                        free(cn);
                    }
                }
            }
            else if (k < L->ntok && (L->toks[k].type == PT_IDENT || L->toks[k].type == PT_STR))
            {
                char *cn = (L->toks[k].type == PT_IDENT) ? tok_text(c, k) : str_literal(c, k, NULL);
                if (cn && cn[0])
                {
                    char *sig = range_text(c, i, end, 160);
                    add_symbol(c, NULL, cn, cn, TT_KIND_CONSTANT, tok_line(c, i), last_line,
                               sig, "", NULL, NULL);
                    free(sig);
                }
                free(cn);
            }
        }

        char *sig = range_text(c, i, end, 160);
        add_symbol(c, NULL, target, target, TT_KIND_DIRECTIVE, tok_line(c, i), last_line,
                   sig, "", NULL, NULL);
        free(sig);
        free(target);
    }
}

/* ===================================================================== */
/* Imports                                                                */
/* ===================================================================== */

static void add_import(pctx_t *c, const char *target, const char *symbol, int line,
                       const char *type, int dynamic, double confidence)
{
    if (!target || !target[0])
        return;
    tt_perl_result_t *o = c->out;
    if (o->imp_count >= o->imp_cap)
    {
        int nc = o->imp_cap ? o->imp_cap * 2 : 16;
        tt_import_t *tmp = realloc(o->imports, (size_t)nc * sizeof(tt_import_t));
        if (!tmp)
        {
            c->oom = true;
            return;
        }
        o->imports = tmp;
        o->imp_cap = nc;
    }
    tt_import_t *imp = &o->imports[o->imp_count++];
    memset(imp, 0, sizeof(*imp));
    imp->from_file = tt_strdup(c->rel_path);
    imp->to_specifier = tt_strdup(target);
    imp->symbol_name = symbol ? tt_strdup(symbol) : NULL;
    imp->line = line;
    imp->import_type = tt_strdup(type);
    imp->dynamic = dynamic;
    imp->confidence = confidence;
}

static bool is_lower_pragma(const char *name)
{
    for (const char *p = name; *p; p++)
    {
        if (isupper((unsigned char)*p) || *p == ':')
            return false;
    }
    return true;
}

/* Import target from a single argument token range: literal string,
 * bareword, or raw text (variables). Sets *confidence. */
static char *arg_target(const pctx_t *c, int a, int b, double *confidence)
{
    *confidence = 1.0;
    if (b - a == 1)
    {
        const ptok_t *t = &c->L->toks[a];
        if (t->type == PT_STR)
        {
            bool interp = false;
            char *v = str_literal(c, a, &interp);
            if (interp)
                *confidence = 0.5;
            return v;
        }
        if (t->type == PT_IDENT)
            return tok_text(c, a);
    }
    *confidence = 0.2;
    return range_text(c, a, b, 200);
}

/* Words of a qw() / list of string tokens in [a,b). Calls fn per word. */
static void list_words(const pctx_t *c, int a, int b,
                       void (*fn)(pctx_t *, const char *, int, void *), void *ud)
{
    const plex_t *L = c->L;
    for (int k = a; k < b; k++)
    {
        const ptok_t *t = &L->toks[k];
        if (t->type == PT_STR && t->sub == STR_QW)
        {
            int p = t->cstart;
            while (p < t->cend)
            {
                while (p < t->cend && isspace((unsigned char)L->s[p]))
                    p++;
                int s = p;
                while (p < t->cend && !isspace((unsigned char)L->s[p]))
                    p++;
                if (p > s)
                {
                    char *w = tt_strndup(L->s + s, (size_t)(p - s));
                    if (w)
                    {
                        fn((pctx_t *)c, w, t->line, ud);
                        free(w);
                    }
                }
            }
        }
        else if (t->type == PT_STR)
        {
            char *w = str_literal(c, k, NULL);
            if (w && w[0])
                fn((pctx_t *)c, w, t->line, ud);
            free(w);
        }
    }
}

static void cb_use_lib(pctx_t *c, const char *w, int line, void *ud)
{
    (void)ud;
    add_import(c, w, NULL, line, "use_lib", 0, 1.0);
}

static void cb_use_parent(pctx_t *c, const char *w, int line, void *ud)
{
    (void)ud;
    if (w[0] == '-')
        return; /* -norequire */
    add_import(c, w, "parent", line, "use", 0, 1.0);
}

static void cb_inc_push(pctx_t *c, const char *w, int line, void *ud)
{
    (void)ud;
    add_import(c, w, NULL, line, "inc_push", 0, 1.0);
}

/* Name of a call at token i: IDENT name or VAR "&name". NULL otherwise. */
static const char *call_name_at(const pctx_t *c, int i, int *len)
{
    const ptok_t *t = &c->L->toks[i];
    if (t->type == PT_IDENT)
    {
        *len = t->end - t->start;
        return c->L->s + t->start;
    }
    if (t->type == PT_VAR && c->L->s[t->start] == '&' && t->end - t->start > 1 &&
        is_id_start((unsigned char)c->L->s[t->start + 1]))
    {
        *len = t->end - t->start - 1;
        return c->L->s + t->start + 1;
    }
    return NULL;
}

static bool name_is(const char *n, int len, const char *w)
{
    size_t wl = strlen(w);
    return (size_t)len == wl && memcmp(n, w, wl) == 0;
}

/* Webmin dynamic dispatch. remote_off = 1 for remote_* variants
 * (first argument is the server). */
static void webmin_dispatch(pctx_t *c, int i, const char *fname, int flen)
{
    bool is_call = name_is(fname, flen, "foreign_call") || name_is(fname, flen, "remote_foreign_call");
    bool remote = (flen > 7 && memcmp(fname, "remote_", 7) == 0);
    int a, b;
    if (!call_args(c, i, &a, &b))
        return;
    int ranges[8][2];
    int n = split_args(c, a, b, ranges, 8);
    int base = remote ? 1 : 0;
    if (n <= base)
        return;

    double conf_mod;
    char *mod = arg_target(c, ranges[base][0], ranges[base][1], &conf_mod);
    if (!mod)
        return;
    int line = tok_line(c, i);
    bool mod_literal = (conf_mod >= 1.0);
    double rconf = remote ? 0.7 : 1.0;

    if (!is_call)
    {
        char *file = NULL;
        double conf_file = 1.0;
        if (n > base + 1)
            file = arg_target(c, ranges[base + 1][0], ranges[base + 1][1], &conf_file);
        tt_strbuf_t sb;
        tt_strbuf_init(&sb);
        double conf;
        if (mod_literal)
        {
            if (file && file[0] && conf_file >= 1.0)
            {
                tt_strbuf_appendf(&sb, "%s/%s", mod, file);
                conf = 1.0;
            }
            else
            {
                tt_strbuf_appendf(&sb, "%s/%s-lib.pl", mod, mod);
                conf = (file && file[0]) ? 0.5 : 0.9;
            }
        }
        else
        {
            tt_strbuf_append_str(&sb, mod);
            conf = 0.2;
        }
        char *target = tt_strbuf_detach(&sb);
        add_import(c, target, NULL, line, remote ? "remote_foreign_require" : "foreign_require",
                   1, conf * rconf);
        free(target);
        free(file);
    }
    else
    {
        char *func = NULL;
        double conf_func = 0.0;
        if (n > base + 1)
            func = arg_target(c, ranges[base + 1][0], ranges[base + 1][1], &conf_func);
        tt_strbuf_t sb;
        tt_strbuf_init(&sb);
        if (mod_literal)
            tt_strbuf_appendf(&sb, "%s/%s-lib.pl", mod, mod);
        else
            tt_strbuf_append_str(&sb, mod);
        char *target = tt_strbuf_detach(&sb);
        double conf = mod_literal ? 0.8 : 0.2;
        if (conf_func < 1.0)
            conf *= 0.5;
        add_import(c, target, (func && conf_func >= 1.0) ? func : NULL, line,
                   remote ? "remote_foreign_call" : "foreign_call", 1, conf * rconf);
        free(target);
        free(func);
    }
    free(mod);
}

/*
 * var_literal_targets -- `do $script` / `require $f` where the variable was
 * assigned plain string literals earlier in the file
 * (my $script = -r '../x.pl' ? '../x.pl' : 'x.pl'): one dynamic import per
 * literal that looks like a Perl file, confidence 0.5. The raw variable
 * import is kept as well.
 */
static void var_literal_targets(pctx_t *c, int var_tok, int line, const char *type)
{
    const plex_t *L = c->L;
    const ptok_t *v = &L->toks[var_tok];
    int vlen = v->end - v->start;
    if (vlen < 2 || L->s[v->start] != '$')
        return;
    int added = 0;
    for (int k = var_tok - 1; k >= 0 && var_tok - k < 4000 && added == 0; k--)
    {
        const ptok_t *t = &L->toks[k];
        if (t->type != PT_VAR || t->end - t->start != vlen ||
            memcmp(L->s + t->start, L->s + v->start, (size_t)vlen) != 0 || !IS_OP(L, k + 1, "="))
            continue;
        int e = stmt_end(c, k + 2);
        for (int m = k + 2; m < e && m < L->ntok && added < 4; m++)
        {
            if (L->toks[m].type != PT_STR)
                continue;
            bool interp = false;
            char *lit = str_literal(c, m, &interp);
            if (lit && !interp && (tt_str_ends_with(lit, ".pl") || tt_str_ends_with(lit, ".pm") ||
                                   tt_str_ends_with(lit, ".cgi")))
            {
                bool dup = false;
                for (int x = c->out->imp_count - added; x < c->out->imp_count; x++)
                {
                    if (strcmp(c->out->imports[x].to_specifier, lit) == 0)
                        dup = true;
                }
                if (!dup)
                {
                    add_import(c, lit, NULL, line, type, 1, 0.5);
                    added++;
                }
            }
            free(lit);
        }
    }
}

static void extract_imports(pctx_t *c)
{
    plex_t *L = c->L;
    for (int i = 0; i < L->ntok && !c->oom; i++)
    {
        const ptok_t *t = &L->toks[i];
        if (i > 0 && IS_OP(L, i - 1, "->"))
            continue;
        int flen = 0;
        const char *fname = call_name_at(c, i, &flen);
        if (!fname)
            continue;
        if (t->type == PT_IDENT && is_hash_key_ctx(L, i))
            continue;

        if (t->type == PT_IDENT && name_is(fname, flen, "use") && i + 1 < L->ntok &&
            L->toks[i + 1].type == PT_IDENT)
        {
            char *mod = tok_text(c, i + 1);
            int end = stmt_end(c, i);
            int line = tok_line(c, i);
            if (mod)
            {
                if (strcmp(mod, "lib") == 0)
                    list_words(c, i + 2, end, cb_use_lib, NULL);
                else if (strcmp(mod, "parent") == 0 || strcmp(mod, "base") == 0)
                    list_words(c, i + 2, end, cb_use_parent, NULL);
                else if (!is_lower_pragma(mod))
                    add_import(c, mod, NULL, line, "use", 0, 1.0);
            }
            free(mod);
            continue;
        }

        if (t->type == PT_IDENT && (name_is(fname, flen, "require") || name_is(fname, flen, "do")))
        {
            bool is_do = name_is(fname, flen, "do");
            int j = i + 1;
            if (j >= L->ntok)
                continue;
            const ptok_t *n = &L->toks[j];
            int line = tok_line(c, i);
            if (n->type == PT_IDENT && !is_do)
            {
                /* require Foo::Bar; (not require if/unless...) */
                if (!is_low_prec_keyword(c, j))
                {
                    char *mod = tok_text(c, j);
                    add_import(c, mod, NULL, line, "require", 0, 1.0);
                    free(mod);
                }
            }
            else if (n->type == PT_STR || n->type == PT_VAR ||
                     (IS_OP(L, j, "(") && L->toks[j].match > j))
            {
                int a = j, b;
                if (IS_OP(L, j, "("))
                {
                    a = j + 1;
                    b = L->toks[j].match;
                }
                else
                {
                    b = expr_end(c, j, true);
                }
                if (b <= a)
                    continue;
                double conf;
                char *target = arg_target(c, a, b, &conf);
                add_import(c, target, NULL, line, is_do ? "do" : "require", 0, conf);
                free(target);
                if (b - a == 1 && L->toks[a].type == PT_VAR)
                    var_literal_targets(c, a, line, is_do ? "do" : "require");
            }
            continue;
        }

        if (name_is(fname, flen, "push") || name_is(fname, flen, "unshift"))
        {
            int a, b;
            if (!call_args(c, i, &a, &b))
                continue;
            if (a < b && tok_is(L, a, PT_VAR, "@INC"))
                list_words(c, a + 1, b, cb_inc_push, NULL);
            continue;
        }

        if (name_is(fname, flen, "foreign_require") || name_is(fname, flen, "foreign_call") ||
            name_is(fname, flen, "remote_foreign_require") ||
            name_is(fname, flen, "remote_foreign_call"))
        {
            webmin_dispatch(c, i, fname, flen);
        }
    }
}

/* ===================================================================== */
/* Sites and taint-lite                                                   */
/* ===================================================================== */

/* Perl builtins and keywords: never recorded as user calls. Sorted. */
static const char *BUILTINS[] = {
    "BEGIN", "CHECK", "DESTROY", "END", "INIT", "__FILE__", "__LINE__", "__PACKAGE__",
    "__SUB__", "abs", "accept", "alarm", "and", "atan2", "bind", "binmode", "bless",
    "break", "caller", "chdir", "chmod", "chomp", "chop", "chown", "chr", "chroot",
    "close", "closedir", "cmp", "connect", "continue", "cos", "crypt", "dbmclose",
    "dbmopen", "default", "defined", "delete", "die", "do", "dump", "each", "else",
    "elsif", "eof", "eq", "eval", "exec", "exists", "exit", "exp", "fc", "fcntl",
    "fileno", "flock", "for", "foreach", "fork", "format", "formline", "ge", "getc",
    "getgrent", "getgrgid", "getgrnam", "gethostbyaddr", "gethostbyname", "getlogin",
    "getpeername", "getppid", "getpwent", "getpwnam", "getpwuid", "getsockname",
    "getsockopt", "given", "glob", "gmtime", "goto", "grep", "gt", "hex", "if", "index",
    "int", "ioctl", "join", "keys", "kill", "last", "lc", "lcfirst", "le", "length",
    "link", "listen", "local", "localtime", "lock", "log", "lstat", "lt", "m", "map",
    "mkdir", "msgctl", "msgget", "msgrcv", "msgsnd", "my", "ne", "next", "no", "not",
    "oct", "open", "opendir", "or", "ord", "our", "pack", "package", "pipe", "pop",
    "pos", "print", "printf", "prototype", "push", "q", "qq", "qr", "quotemeta", "qw",
    "qx", "rand", "read", "readdir", "readline", "readlink", "readpipe", "recv", "redo",
    "ref", "rename", "require", "reset", "return", "reverse", "rewinddir", "rindex",
    "rmdir", "s", "say", "scalar", "seek", "seekdir", "select", "semctl", "semget",
    "semop", "send", "setpgrp", "setpriority", "setsockopt", "shift", "shmctl",
    "shmget", "shmread", "shmwrite", "shutdown", "sin", "sleep", "socket", "socketpair",
    "sort", "splice", "split", "sprintf", "sqrt", "srand", "stat", "state", "study",
    "sub", "substr", "symlink", "syscall", "sysopen", "sysread", "sysseek", "system",
    "syswrite", "tell", "telldir", "tie", "tied", "time", "times", "tr", "truncate",
    "uc", "ucfirst", "umask", "undef", "unless", "unlink", "unpack", "unshift", "untie",
    "until", "use", "utime", "values", "vec", "wait", "waitpid", "wantarray", "warn",
    "when", "while", "write", "x", "xor", "y"};

#define BUILTIN_COUNT ((int)(sizeof(BUILTINS) / sizeof(BUILTINS[0])))

static bool is_builtin(const char *n, int len)
{
    int lo = 0, hi = BUILTIN_COUNT - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        const char *w = BUILTINS[mid];
        size_t wl = strlen(w);
        size_t ml = (size_t)len < wl ? (size_t)len : wl;
        int r = memcmp(n, w, ml);
        if (r == 0)
            r = (size_t)len < wl ? -1 : ((size_t)len > wl ? 1 : 0);
        if (r == 0)
            return true;
        if (r < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return false;
}

bool tt_perl_builtins_sorted(void)
{
    for (int i = 1; i < BUILTIN_COUNT; i++)
    {
        if (strcmp(BUILTINS[i - 1], BUILTINS[i]) >= 0)
            return false;
    }
    return true;
}

/* Command-executing subroutines (Webmin and common wrappers). */
static const char *COMMAND_WRAPPERS[] = {
    "system_logged", "backquote_command", "backquote_logged", "backquote_with_timeout",
    "backquote_with_timeout_logged", "execute_command", "open_execute_command",
    "safe_process_exec", "safe_process_exec_logged", NULL};

/* File-writing wrappers whose 2nd argument is an open() mode string. */
static const char *OPEN_WRAPPERS[] = {"open_tempfile", "open_lock_tempfile", NULL};

static const char *SANITIZERS[] = {
    "quotemeta", "quote", "quote_path", "quote_escape", "command_as_user", "urlize",
    "html_escape", "is_under_directory", NULL};

static const char *ACL_GATES[] = {
    "get_module_acl", "foreign_available", "can_use_module", "allowed_auth_file",
    "check_allowed_path", NULL};

static bool in_list(const char *n, int len, const char **list)
{
    for (const char **w = list; *w; w++)
    {
        if (name_is(n, len, *w))
            return true;
    }
    return false;
}

static bool is_sanitizer(const char *n, int len)
{
    if (in_list(n, len, SANITIZERS))
        return true;
    if (len > 8 && memcmp(n, "allowed_", 8) == 0)
        return true;
    if (len > 4 && memcmp(n, "can_", 4) == 0)
        return true;
    return false;
}

static bool is_read_file(const char *n, int len)
{
    return len >= 9 && memcmp(n, "read_file", 9) == 0;
}

static int add_site(pctx_t *c, tt_site_category_e cat, int line, const char *name,
                    const char *qualifier, int scope, const char *args, uint32_t mask,
                    const char *arg_masks, const char *trace)
{
    tt_perl_result_t *o = c->out;
    if (o->site_count >= o->site_cap)
    {
        int nc = o->site_cap ? o->site_cap * 2 : 64;
        tt_site_t *tmp = realloc(o->sites, (size_t)nc * sizeof(tt_site_t));
        if (!tmp)
        {
            c->oom = true;
            return -1;
        }
        o->sites = tmp;
        o->site_cap = nc;
    }
    tt_site_t *s = &o->sites[o->site_count++];
    memset(s, 0, sizeof(*s));
    s->file = tt_strdup(c->rel_path);
    s->line = line;
    s->category = cat;
    s->name = tt_strdup(name ? name : "");
    s->qualifier = qualifier ? tt_strdup(qualifier) : NULL;
    s->scope_id = (scope >= 0 && scope < c->nsub && c->subs[scope].id) ? tt_strdup(c->subs[scope].id)
                                                                       : NULL;
    s->args = tt_strdup(args ? args : "");
    s->mask = mask;
    s->arg_masks = arg_masks ? tt_strdup(arg_masks) : NULL;
    s->trace = (trace && trace[0]) ? tt_strdup(trace) : NULL;
    return 0;
}

/* ---- Variable maps ---- */

static tt_hashmap_t *scope_map(pctx_t *c, int scope)
{
    int idx = scope + 1;
    if (!c->scope_vars[idx])
        c->scope_vars[idx] = tt_hashmap_new(32);
    return c->scope_vars[idx];
}

static pvar_t *var_get(pctx_t *c, int scope, const char *key, bool create)
{
    tt_hashmap_t *m = scope_map(c, scope);
    if (!m)
        return NULL;
    pvar_t *v = tt_hashmap_get(m, key);
    if (!v && create)
    {
        v = calloc(1, sizeof(pvar_t));
        if (v)
            tt_hashmap_set(m, key, v);
    }
    return v;
}

/* Normalize a variable reference for the var map: container identity. */
static void var_key(char *out, size_t outsz, char sigil, const char *name, int nlen,
                    char next, bool arrow_next)
{
    char sg = sigil;
    if (sigil == '$' && !arrow_next)
    {
        if (next == '{')
            sg = '%';
        else if (next == '[')
            sg = '@';
    }
    else if (sigil == '@' && next == '{')
    {
        sg = '%';
    }
    if (nlen > (int)outsz - 2)
        nlen = (int)outsz - 2;
    out[0] = sg;
    memcpy(out + 1, name, (size_t)nlen);
    out[nlen + 1] = '\0';
}

/* Source bit for a normalized key ("%in", "%ENV", ...). */
static uint32_t source_bit(const char *key, const char **sname)
{
    static const struct
    {
        const char *key;
        uint32_t bit;
        const char *name;
    } SRC[] = {
        {"%in", TT_TAINT_IN, "in"},
        {"$in", TT_TAINT_IN, "in"},
        {"%ENV", TT_TAINT_ENV, "ENV"},
        {"@ARGV", TT_TAINT_ARGV, "ARGV"},
        {"%config", TT_TAINT_CONFIG, "config"},
        {"%gconfig", TT_TAINT_CONFIG, "config"},
    };
    for (size_t i = 0; i < sizeof(SRC) / sizeof(SRC[0]); i++)
    {
        if (strcmp(key, SRC[i].key) == 0)
        {
            if (sname)
                *sname = SRC[i].name;
            return SRC[i].bit;
        }
    }
    return 0;
}

typedef struct
{
    tt_strbuf_t sb;
    int n;
} ptrace_t;

static void trace_add(ptrace_t *tr, const char *what, int line)
{
    if (!tr || tr->n >= 4)
        return;
    if (tr->sb.len > 0)
        tt_strbuf_append_char(&tr->sb, ',');
    tt_strbuf_appendf(&tr->sb, "%s@%d", what, line);
    tr->n++;
}

/* Mask contribution of one variable occurrence. */
static uint32_t var_mask(pctx_t *c, int scope, const char *key, int line, ptrace_t *tr)
{
    const char *sname = NULL;
    uint32_t b = source_bit(key, &sname);
    if (b)
    {
        trace_add(tr, key, line);
        return b;
    }
    if (strcmp(key, "@_") == 0)
        return TT_TAINT_ALL_ARGS | TT_TAINT_PARAMS;
    pvar_t *v = var_get(c, scope, key, false);
    if (!v)
        return 0;
    if ((v->mask & TT_TAINT_SOURCES) && v->line > 0)
        trace_add(tr, key, v->line);
    return v->mask;
}

/* Interpolated variables in a string token. */
static uint32_t interp_mask(pctx_t *c, int scope, int ti, ptrace_t *tr)
{
    const ptok_t *t = &c->L->toks[ti];
    const char *s = c->L->s;
    uint32_t m = 0;
    int line = t->line;
    for (int p = t->cstart; p < t->cend; p++)
    {
        char ch = s[p];
        if (ch == '\n')
        {
            line++;
            continue;
        }
        if (ch == '\\')
        {
            p++;
            continue;
        }
        if (ch != '$' && ch != '@')
            continue;
        int q = p + 1;
        bool braced = false;
        if (q < t->cend && s[q] == '{')
        {
            braced = true;
            q++;
        }
        if (q >= t->cend || !is_id_start((unsigned char)s[q]))
            continue;
        int ns = q;
        while (q < t->cend && (is_id_char((unsigned char)s[q]) ||
                               (s[q] == ':' && q + 2 < t->cend && s[q + 1] == ':' &&
                                is_id_start((unsigned char)s[q + 2]))))
            q += (s[q] == ':') ? 2 : 1;
        int ne = q;
        if (braced && q < t->cend && s[q] == '}')
            q++;
        char next = (q < t->cend) ? s[q] : 0;
        bool arrow = (q + 1 < t->cend && s[q] == '-' && s[q + 1] == '>');
        if (ch == '@' && next != '[' && next != '{')
            next = 0;
        char key[80];
        var_key(key, sizeof(key), ch, s + ns, ne - ns, next, arrow);
        if (strcmp(key, "@_") == 0 || (ch == '$' && ne - ns == 1 && s[ns] == '_' && next == '['))
        {
            m |= TT_TAINT_ALL_ARGS | TT_TAINT_PARAMS;
            continue;
        }
        m |= var_mask(c, scope, key, line, tr);
        p = ne - 1;
    }
    return m;
}

/* Taint mask of tokens [a,b) in a scope. */
static uint32_t mask_range(pctx_t *c, int scope, int a, int b, ptrace_t *tr)
{
    plex_t *L = c->L;
    uint32_t m = 0;
    for (int i = a; i < b && i < L->ntok; i++)
    {
        const ptok_t *t = &L->toks[i];
        switch (t->type)
        {
        case PT_VAR:
        {
            int len = t->end - t->start;
            if (len < 2)
                break;
            char sig = L->s[t->start];
            const char *name = L->s + t->start + 1;
            int nlen = len - 1;
            if (sig == '&')
                break;
            /* $_[N] -> param N */
            if (sig == '$' && nlen == 1 && name[0] == '_' && IS_OP(L, i + 1, "[") &&
                i + 3 < L->ntok && L->toks[i + 2].type == PT_NUM && IS_OP(L, i + 3, "]"))
            {
                int n = atoi(L->s + L->toks[i + 2].start);
                if (n >= 0 && n < TT_TAINT_MAX_PARAMS)
                    m |= TT_TAINT_PARAM(n);
                break;
            }
            /* Derefs: $$x / @$x / %$x -> $x */
            char dsig = sig;
            while (nlen > 0 && name[0] == '$')
            {
                dsig = '$';
                name++;
                nlen--;
            }
            if (sig == '$' && name[0] == '#')
            {
                name++;
                nlen--;
                dsig = '@';
            }
            if (nlen <= 0)
                break;
            char next = 0;
            bool arrow = IS_OP(L, i + 1, "->");
            if (i + 1 < L->ntok && L->toks[i + 1].type == PT_OP &&
                L->toks[i + 1].end - L->toks[i + 1].start == 1)
                next = L->s[L->toks[i + 1].start];
            char key[80];
            var_key(key, sizeof(key), dsig, name, nlen, (dsig == sig) ? next : 0, arrow);
            m |= var_mask(c, scope, key, t->line, tr);
            break;
        }
        case PT_STR:
            if (t->interp)
                m |= interp_mask(c, scope, i, tr);
            break;
        case PT_REGEX:
            break;
        case PT_READLINE:
        {
            if (span_eq(L, t->cstart, t->cend, "STDIN"))
            {
                m |= TT_TAINT_STDIN;
                trace_add(tr, "<STDIN>", t->line);
            }
            else
            {
                m |= TT_TAINT_FILE;
            }
            break;
        }
        case PT_IDENT:
        {
            int len = t->end - t->start;
            const char *n = L->s + t->start;
            if (c->shift_param[i] >= 0)
                m |= TT_TAINT_PARAM(c->shift_param[i]);
            else if (is_read_file(n, len))
            {
                m |= TT_TAINT_FILE;
                trace_add(tr, "read_file", t->line);
            }
            else if (is_sanitizer(n, len) && IS_OP(L, i + 1, "("))
                m |= TT_TAINT_SANITIZED;
            break;
        }
        default:
            break;
        }
        if (t->type == PT_VAR && L->s[t->start] == '&')
        {
            int len = t->end - t->start - 1;
            const char *n = L->s + t->start + 1;
            if (is_read_file(n, len))
            {
                m |= TT_TAINT_FILE;
                trace_add(tr, "read_file", t->line);
            }
            else if (is_sanitizer(n, len))
                m |= TT_TAINT_SANITIZED;
        }
    }
    return m;
}

/* Base variable key of an lvalue ending at token j (inclusive). Returns
 * the index of the base VAR token or -1. */
static int lvalue_base(const pctx_t *c, int j, char *key, size_t keysz)
{
    const plex_t *L = c->L;
    char first_sub = 0;
    bool arrow = false;
    while (j >= 0)
    {
        const ptok_t *t = &L->toks[j];
        if (t->type == PT_OP && t->end - t->start == 1 &&
            (L->s[t->start] == '}' || L->s[t->start] == ']') && t->match >= 0 && t->match < j)
        {
            first_sub = L->s[L->toks[t->match].start];
            j = t->match - 1;
            if (j >= 0 && IS_OP(L, j, "->"))
            {
                arrow = true;
                j--;
            }
            continue;
        }
        break;
    }
    if (j < 0 || L->toks[j].type != PT_VAR)
        return -1;
    const ptok_t *v = &L->toks[j];
    int len = v->end - v->start;
    if (len < 2)
        return -1;
    char sig = L->s[v->start];
    const char *name = L->s + v->start + 1;
    int nlen = len - 1;
    while (nlen > 0 && name[0] == '$')
    {
        sig = '$';
        name++;
        nlen--;
    }
    if (nlen <= 0 || sig == '&' || sig == '*')
        return -1;
    var_key(key, keysz, sig, name, nlen, arrow ? 0 : first_sub, arrow);
    return j;
}

static void taint_assign_key(pctx_t *c, int scope, const char *key, uint32_t mask, int line)
{
    if (!mask)
        return;
    if (source_bit(key, NULL))
        return; /* sources stay sources */
    pvar_t *v = var_get(c, scope, key, true);
    if (!v)
        return;
    v->mask |= mask;
    if ((mask & TT_TAINT_SOURCES) && v->line == 0)
        v->line = line;
}

/* Assignment operator at token i. */
static void handle_assign(pctx_t *c, int scope, int i)
{
    plex_t *L = c->L;
    int j = i - 1;
    if (j < 0)
        return;
    char keys[TT_TAINT_MAX_PARAMS][80];
    int nkeys = 0;
    bool list = false;

    if (IS_OP(L, j, ")") && L->toks[j].match >= 0 && L->toks[j].match < j)
    {
        int o = L->toks[j].match;
        list = true;
        for (int k = o + 1; k < j && nkeys < TT_TAINT_MAX_PARAMS; k++)
        {
            if (L->toks[k].type != PT_VAR)
                continue;
            if (lvalue_base(c, k, keys[nkeys], sizeof(keys[0])) >= 0)
                nkeys++;
        }
    }
    else
    {
        if (lvalue_base(c, j, keys[0], sizeof(keys[0])) >= 0)
            nkeys = 1;
    }
    if (nkeys == 0)
        return;

    int rs = i + 1;
    int re = expr_end(c, rs, !list);
    if (re <= rs)
        return;
    int line = tok_line(c, i);

    /* my ($a, $b) = @_;  -> positional params */
    if (list && re == rs + 1 && tok_is(L, rs, PT_VAR, "@_"))
    {
        for (int k = 0; k < nkeys; k++)
            taint_assign_key(c, scope, keys[k], TT_TAINT_PARAM(k), line);
        return;
    }

    uint32_t m = mask_range(c, scope, rs, re, NULL);
    for (int k = 0; k < nkeys; k++)
        taint_assign_key(c, scope, keys[k], m, line);
}

/* foreach my $x (LIST) / for $x (LIST) / foreach (LIST) -> $_ */
static void handle_foreach(pctx_t *c, int scope, int i)
{
    plex_t *L = c->L;
    int j = i + 1;
    if (IS_IDENT(L, j, "my") || IS_IDENT(L, j, "our") || IS_IDENT(L, j, "local") ||
        IS_IDENT(L, j, "state"))
        j++;
    char key[80];
    int paren;
    if (j < L->ntok && L->toks[j].type == PT_VAR && IS_OP(L, j + 1, "("))
    {
        if (lvalue_base(c, j, key, sizeof(key)) < 0)
            return;
        paren = j + 1;
    }
    else if (IS_OP(L, j, "("))
    {
        strcpy(key, "$_");
        paren = j;
    }
    else
        return;
    int m = L->toks[paren].match;
    if (m <= paren)
        return;
    uint32_t mask = mask_range(c, scope, paren + 1, m, NULL);
    taint_assign_key(c, scope, key, mask, tok_line(c, i));
}

static char *masks_json(const uint32_t *masks, int n)
{
    bool any = false;
    for (int k = 0; k < n; k++)
    {
        if (masks[k])
            any = true;
    }
    if (!any)
        return NULL;
    tt_strbuf_t sb;
    tt_strbuf_init(&sb);
    tt_strbuf_append_char(&sb, '[');
    for (int k = 0; k < n; k++)
    {
        if (k > 0)
            tt_strbuf_append_char(&sb, ',');
        tt_strbuf_appendf(&sb, "%u", masks[k]);
    }
    tt_strbuf_append_char(&sb, ']');
    return tt_strbuf_detach(&sb);
}

static void record_sink(pctx_t *c, int scope, int at, const char *name, const char *klass,
                        int a, int b)
{
    ptrace_t tr;
    tt_strbuf_init(&tr.sb);
    tr.n = 0;
    uint32_t m = (b > a) ? mask_range(c, scope, a, b, &tr) : 0;
    char *args = range_text(c, a, b, 240);
    add_site(c, TT_SITE_SINK, tok_line(c, at), name, klass, scope, args, m, NULL,
             tr.sb.data ? tr.sb.data : "");
    free(args);
    tt_strbuf_free(&tr.sb);
}

/* Classify an open()-style mode from a literal or the start of an
 * interpolated string: returns "open_pipe", "open_write" or NULL. */
static const char *open_mode_class(const pctx_t *c, int a, int b, bool three_arg_mode)
{
    if (b - a < 1)
        return NULL;
    const ptok_t *t = &c->L->toks[a];
    if (t->type != PT_STR || b - a != 1)
        return "open_dynamic";
    const char *s = c->L->s;
    int p = t->cstart, e = t->cend;
    while (p < e && isspace((unsigned char)s[p]))
        p++;
    while (e > p && isspace((unsigned char)s[e - 1]))
        e--;
    if (p >= e)
        return NULL;
    if (t->interp && (s[p] == '$' || s[p] == '@'))
        return "open_dynamic";
    if (three_arg_mode)
    {
        if (memchr(s + p, '|', (size_t)(e - p)))
            return "open_pipe";
        if (s[p] == '>' || (s[p] == '+' && p + 1 < e && (s[p + 1] == '>' || s[p + 1] == '<')))
            return "open_write";
        return NULL;
    }
    if (s[p] == '|' || s[e - 1] == '|')
        return "open_pipe";
    if (s[p] == '>' || (s[p] == '+' && p + 1 < e && (s[p + 1] == '>' || s[p + 1] == '<')))
        return "open_write";
    return NULL;
}

static void handle_open(pctx_t *c, int scope, int i, bool wrapper)
{
    int a, b;
    if (!call_args(c, i, &a, &b))
        return;
    int ranges[4][2];
    int n = split_args(c, a, b, ranges, 4);
    if (n < 2)
        return;
    const char *klass = NULL;
    if (n >= 3 && !wrapper)
        klass = open_mode_class(c, ranges[1][0], ranges[1][1], true);
    else
        klass = open_mode_class(c, ranges[1][0], ranges[1][1], false);
    if (!klass && wrapper)
    {
        /* open_tempfile(FH, $file): default mode is write */
        const ptok_t *t = &c->L->toks[ranges[1][0]];
        if (t->type != PT_STR || c->L->s[t->cstart] != '<')
            klass = "open_write";
    }
    if (!klass)
        return;
    int name_len = 0;
    const char *fname = call_name_at(c, i, &name_len);
    char nm[64];
    if (wrapper && fname)
    {
        snprintf(nm, sizeof(nm), "%.*s", name_len > 60 ? 60 : name_len, fname);
        record_sink(c, scope, i, nm, strcmp(klass, "open_write") == 0 ? "file_write" : "command",
                    a, b);
    }
    else
    {
        record_sink(c, scope, i, klass, strcmp(klass, "open_write") == 0 ? "file_write" : "command",
                    a, b);
    }
}

static void record_call(pctx_t *c, int scope, int at, const char *name, int nlen,
                        const char *qualifier, int a, int b)
{
    int ranges[TT_TAINT_MAX_PARAMS][2];
    int n = (b > a) ? split_args(c, a, b, ranges, TT_TAINT_MAX_PARAMS) : 0;
    uint32_t masks[TT_TAINT_MAX_PARAMS];
    for (int k = 0; k < n; k++)
        masks[k] = mask_range(c, scope, ranges[k][0], ranges[k][1], NULL);
    char *mj = masks_json(masks, n);
    char *args = range_text(c, a, b, 160);
    char nm[128];
    snprintf(nm, sizeof(nm), "%.*s", nlen > 120 ? 120 : nlen, name);
    add_site(c, TT_SITE_CALL, tok_line(c, at), nm, qualifier, scope, args, 0, mj, NULL);
    free(mj);
    free(args);
}

/* read_file($f, \%h): %h receives file content. */
static void handle_read_file_out(pctx_t *c, int scope, int i)
{
    int a, b;
    if (!call_args(c, i, &a, &b))
        return;
    for (int k = a; k + 1 < b; k++)
    {
        if (IS_OP(c->L, k, "\\") && c->L->toks[k + 1].type == PT_VAR)
        {
            char key[80];
            if (lvalue_base(c, k + 1, key, sizeof(key)) >= 0)
                taint_assign_key(c, scope, key, TT_TAINT_FILE, tok_line(c, i));
        }
    }
}

/* push @list, EXPR / unshift @list, EXPR */
static void handle_push(pctx_t *c, int scope, int i)
{
    int a, b;
    if (!call_args(c, i, &a, &b) || b - a < 3)
        return;
    if (c->L->toks[a].type != PT_VAR || !IS_OP(c->L, a + 1, ","))
        return;
    char key[80];
    if (lvalue_base(c, a, key, sizeof(key)) < 0)
        return;
    uint32_t m = mask_range(c, scope, a + 2, b, NULL);
    taint_assign_key(c, scope, key, m, tok_line(c, i));
}

static char *hash_key_text(const pctx_t *c, int brace)
{
    const plex_t *L = c->L;
    if (!IS_OP(L, brace, "{") || L->toks[brace].match <= brace)
        return tt_strdup("");
    return range_text(c, brace + 1, L->toks[brace].match, 80);
}

/* Source/ACL occurrences inside an interpolating string. */
static void string_sources(pctx_t *c, int scope, int ti)
{
    const ptok_t *t = &c->L->toks[ti];
    const char *s = c->L->s;
    int line = t->line;
    for (int p = t->cstart; p < t->cend; p++)
    {
        char ch = s[p];
        if (ch == '\n')
        {
            line++;
            continue;
        }
        if (ch == '\\')
        {
            p++;
            continue;
        }
        if (ch != '$' && ch != '@' && ch != '%')
            continue;
        int q = p + 1;
        if (q < t->cend && s[q] == '{')
            q++;
        int ns = q;
        while (q < t->cend && is_id_char((unsigned char)s[q]))
            q++;
        int nl = q - ns;
        if (nl <= 0)
            continue;
        if (q < t->cend && s[q] == '}' && ns > p + 1)
            q++;
        char next = (q < t->cend) ? s[q] : 0;
        if (ch != '$' || (next != '{' && next != '['))
        {
            p = q - 1;
            continue;
        }
        const char *sname = NULL;
        char key[80];
        var_key(key, sizeof(key), '$', s + ns, nl, next, false);
        uint32_t b = source_bit(key, &sname);
        bool is_acl = (strcmp(key, "%access") == 0);
        if (b || is_acl)
        {
            int ke = q + 1;
            int depth = 1;
            while (ke < t->cend && depth > 0)
            {
                if (s[ke] == '{' || s[ke] == '[')
                    depth++;
                else if (s[ke] == '}' || s[ke] == ']')
                    depth--;
                ke++;
            }
            char *k = collapse_text(s, q + 1, ke > q + 1 ? ke - 1 : q + 1, 80);
            if (b)
                add_site(c, TT_SITE_SOURCE, line, sname, key + 1, scope, k, b, NULL, NULL);
            else
                add_site(c, TT_SITE_ACL, line, "access", NULL, scope, k, 0, NULL, NULL);
            free(k);
        }
        p = q - 1;
    }
}

/* Call of a named subroutine at token i (IDENT name or VAR &name). */
static void handle_named_call(pctx_t *c, int scope, int i, const char *fname, int flen, bool amp)
{
    plex_t *L = c->L;
    const char *last = fname;
    for (int k = 0; k + 1 < flen; k++)
    {
        if (fname[k] == ':' && fname[k + 1] == ':')
            last = fname + k + 2;
    }
    int llen = flen - (int)(last - fname);
    if (llen <= 0)
        return;
    char *qual = (last != fname) ? tt_strndup(fname, (size_t)(last - fname - 2)) : NULL;
    int line = tok_line(c, i);

    int a, b;
    bool has_parens = (IS_OP(L, i + 1, "(") && L->toks[i + 1].match > i + 1);
    if (has_parens)
    {
        a = i + 2;
        b = L->toks[i + 1].match;
    }
    else if (amp || !call_args(c, i, &a, &b))
    {
        a = b = i + 1;
    }

    char nm[128];
    snprintf(nm, sizeof(nm), "%.*s", llen > 120 ? 120 : llen, last);

    if (in_list(last, llen, COMMAND_WRAPPERS))
        record_sink(c, scope, i, nm, "command", a, b);
    else if (in_list(last, llen, OPEN_WRAPPERS))
        handle_open(c, scope, i, true);

    if (is_read_file(last, llen))
    {
        char *args = range_text(c, a, b, 160);
        add_site(c, TT_SITE_SOURCE, line, "read_file", nm, scope, args, TT_TAINT_FILE, NULL, NULL);
        free(args);
        handle_read_file_out(c, scope, i);
    }
    if (is_sanitizer(last, llen))
    {
        char *args = range_text(c, a, b, 160);
        add_site(c, TT_SITE_SANITIZER, line, nm, NULL, scope, args, 0, NULL, NULL);
        free(args);
    }
    if (in_list(last, llen, ACL_GATES))
    {
        char *args = range_text(c, a, b, 160);
        add_site(c, TT_SITE_ACL, line, nm, NULL, scope, args, 0, NULL, NULL);
        free(args);
    }

    if (name_is(last, llen, "foreign_call") || name_is(last, llen, "remote_foreign_call"))
    {
        /* foreign_call("mod", "func", args...) is a call of mod::func */
        int ranges[TT_TAINT_MAX_PARAMS + 3][2];
        int nr = split_args(c, a, b, ranges, TT_TAINT_MAX_PARAMS + 3);
        int base = name_is(last, llen, "remote_foreign_call") ? 1 : 0;
        if (nr > base + 1 && ranges[base + 1][1] - ranges[base + 1][0] == 1)
        {
            char *mod = (ranges[base][1] - ranges[base][0] == 1) ? str_literal(c, ranges[base][0], NULL)
                                                                 : NULL;
            char *func = str_literal(c, ranges[base + 1][0], NULL);
            if (func && func[0])
            {
                int ca = (nr > base + 2) ? ranges[base + 2][0] : b;
                record_call(c, scope, i, func, (int)strlen(func), (mod && mod[0]) ? mod : NULL, ca, b);
            }
            free(mod);
            free(func);
        }
    }
    else if (amp && !has_parens)
    {
        /* &name; -> callee receives the caller's @_ */
        uint32_t all = TT_TAINT_ALL_ARGS;
        char *mj = masks_json(&all, 1);
        add_site(c, TT_SITE_CALL, line, nm, qual, scope, "@_", 0, mj, NULL);
        free(mj);
    }
    else
    {
        record_call(c, scope, i, last, llen, qual, a, b);
    }
    free(qual);
}

static void extract_sites(pctx_t *c)
{
    plex_t *L = c->L;

    /* Bare 'shift' -> positional parameter index, per sub. */
    int *shift_count = calloc((size_t)c->nsub + 1, sizeof(int));
    if (!shift_count)
    {
        c->oom = true;
        return;
    }
    for (int i = 0; i < L->ntok; i++)
    {
        c->shift_param[i] = -1;
        int scope = c->tok_sub[i];
        if (scope < 0 || !IS_IDENT(L, i, "shift"))
            continue;
        if (i > 0 && IS_OP(L, i - 1, "->"))
            continue;
        bool bare = !(i + 1 < L->ntok && (L->toks[i + 1].type == PT_VAR ||
                                           (IS_OP(L, i + 1, "(") && L->toks[i + 1].match > i + 2 &&
                                            !tok_is(L, i + 2, PT_VAR, "@_"))));
        if (bare && shift_count[scope] < TT_TAINT_MAX_PARAMS)
            c->shift_param[i] = shift_count[scope]++;
    }
    free(shift_count);

    for (int i = 0; i < L->ntok && !c->oom; i++)
    {
        const ptok_t *t = &L->toks[i];
        int scope = c->tok_sub[i];

        if (t->type == PT_OP)
        {
            if (span_eq(L, t->start, t->end, "=") || span_eq(L, t->start, t->end, ".=") ||
                span_eq(L, t->start, t->end, "||=") || span_eq(L, t->start, t->end, "//=") ||
                span_eq(L, t->start, t->end, "+="))
            {
                handle_assign(c, scope, i);
            }
            else if (span_eq(L, t->start, t->end, "->") && i + 1 < L->ntok &&
                     L->toks[i + 1].type == PT_IDENT)
            {
                /* Method call: Class->m(...) / $obj->m(...) */
                int n = i + 1;
                char *q = NULL;
                if (i > 0 && L->toks[i - 1].type == PT_IDENT)
                    q = tok_text(c, i - 1);
                int a = n + 1, b = n + 1;
                if (IS_OP(L, n + 1, "(") && L->toks[n + 1].match > n + 1)
                {
                    a = n + 2;
                    b = L->toks[n + 1].match;
                }
                const ptok_t *nt = &L->toks[n];
                record_call(c, scope, n, L->s + nt->start, nt->end - nt->start, q ? q : "->", a, b);
                free(q);
                i = n;
            }
            continue;
        }

        if (t->type == PT_STR)
        {
            if (t->sub == STR_CMD || t->sub == STR_HEREDOC_CMD)
            {
                ptrace_t tr;
                tt_strbuf_init(&tr.sb);
                tr.n = 0;
                uint32_t m = interp_mask(c, scope, i, &tr);
                char *args = collapse_text(L->s, t->cstart, t->cend, 240);
                bool qx = (t->end - t->start > 2 && L->s[t->start] == 'q');
                add_site(c, TT_SITE_SINK, t->line, qx ? "qx" : "backtick", "command", scope, args,
                         m, NULL, tr.sb.data ? tr.sb.data : "");
                free(args);
                tt_strbuf_free(&tr.sb);
            }
            if (t->interp)
                string_sources(c, scope, i);
            /* Webmin harness: "unauthcgi", "^/forgot.cgi\$ ..." */
            if (span_eq(L, t->cstart, t->cend, "unauthcgi") && IS_OP(L, i + 1, ",") &&
                i + 2 < L->ntok && L->toks[i + 2].type == PT_STR)
            {
                const ptok_t *v = &L->toks[i + 2];
                char *pat = tt_strndup(L->s + v->cstart, (size_t)(v->cend - v->cstart));
                add_site(c, TT_SITE_HARNESS, t->line, "unauthcgi", NULL, scope, pat ? pat : "",
                         0, NULL, NULL);
                free(pat);
            }
            continue;
        }

        if (t->type == PT_READLINE)
        {
            if (span_eq(L, t->cstart, t->cend, "STDIN"))
                add_site(c, TT_SITE_SOURCE, t->line, "STDIN", NULL, scope, "", TT_TAINT_STDIN,
                         NULL, NULL);
            continue;
        }

        if (t->type == PT_VAR)
        {
            int len = t->end - t->start;
            char sig = L->s[t->start];
            if (sig == '&')
            {
                if (len >= 2 && is_id_start((unsigned char)L->s[t->start + 1]))
                    handle_named_call(c, scope, i, L->s + t->start + 1, len - 1, true);
                continue;
            }
            if (len >= 2 && (sig == '$' || sig == '%' || sig == '@'))
            {
                char next = 0;
                if (i + 1 < L->ntok && L->toks[i + 1].type == PT_OP &&
                    L->toks[i + 1].end - L->toks[i + 1].start == 1)
                    next = L->s[L->toks[i + 1].start];
                char key[80];
                var_key(key, sizeof(key), sig, L->s + t->start + 1, len - 1, next, false);
                const char *sname = NULL;
                uint32_t b = source_bit(key, &sname);
                if (b)
                {
                    char *k = (next == '{') ? hash_key_text(c, i + 1) : tt_strdup("");
                    add_site(c, TT_SITE_SOURCE, t->line, sname, key + 1, scope, k ? k : "", b,
                             NULL, NULL);
                    free(k);
                }
                else if (strcmp(key, "%access") == 0 && sig == '$' && next == '{')
                {
                    char *k = hash_key_text(c, i + 1);
                    add_site(c, TT_SITE_ACL, t->line, "access", NULL, scope, k ? k : "", 0, NULL,
                             NULL);
                    free(k);
                }
            }
            continue;
        }

        if (t->type != PT_IDENT)
            continue;
        if (i > 0 && (IS_OP(L, i - 1, "->") || IS_IDENT(L, i - 1, "sub") ||
                      IS_IDENT(L, i - 1, "package") || IS_IDENT(L, i - 1, "use") ||
                      IS_IDENT(L, i - 1, "no") || IS_IDENT(L, i - 1, "require")))
            continue;
        if (is_hash_key_ctx(L, i))
            continue;

        {
            const char *n = L->s + t->start;
            int nlen = t->end - t->start;

            if (name_is(n, nlen, "foreach") || name_is(n, nlen, "for"))
            {
                handle_foreach(c, scope, i);
                continue;
            }
            if (name_is(n, nlen, "push") || name_is(n, nlen, "unshift"))
            {
                handle_push(c, scope, i);
                continue;
            }
            if (name_is(n, nlen, "system") || name_is(n, nlen, "exec"))
            {
                int a, b;
                if (call_args(c, i, &a, &b))
                    record_sink(c, scope, i, name_is(n, nlen, "system") ? "system" : "exec",
                                "command", a, b);
                continue;
            }
            if (name_is(n, nlen, "eval"))
            {
                if (IS_OP(L, i + 1, "{"))
                    continue;
                int a, b;
                if (call_args(c, i, &a, &b))
                    record_sink(c, scope, i, "eval_string", "code_eval", a, b);
                continue;
            }
            if (name_is(n, nlen, "open"))
            {
                handle_open(c, scope, i, false);
                continue;
            }
            if (name_is(n, nlen, "quotemeta") && IS_OP(L, i + 1, "("))
            {
                int a, b;
                call_args(c, i, &a, &b);
                char *args = range_text(c, a, b, 160);
                add_site(c, TT_SITE_SANITIZER, t->line, "quotemeta", NULL, scope, args, 0, NULL,
                         NULL);
                free(args);
                continue;
            }
            if (is_builtin(n, nlen))
                continue;
            /* User calls need parens (Webmin command wrappers may be list ops). */
            if (!IS_OP(L, i + 1, "(") && !in_list(n, nlen, COMMAND_WRAPPERS))
                continue;
            handle_named_call(c, scope, i, n, nlen, false);
        }
    }
}

/* ===================================================================== */
/* Public API                                                             */
/* ===================================================================== */

bool tt_perl_is_perl_path(const char *rel_path)
{
    if (!rel_path)
        return false;
    const char *dot = strrchr(rel_path, '.');
    const char *slash = strrchr(rel_path, '/');
    if (!dot || (slash && dot < slash))
        return false;
    return tt_strcasecmp(dot, ".pl") == 0 || tt_strcasecmp(dot, ".pm") == 0 ||
           tt_strcasecmp(dot, ".cgi") == 0 || tt_strcasecmp(dot, ".psgi") == 0;
}

const char *tt_perl_shebang_language(const char *content, size_t len)
{
    if (!content || len < 3 || content[0] != '#' || content[1] != '!')
        return NULL;
    size_t e = 2;
    while (e < len && e < 256 && content[e] != '\n')
        e++;
    char buf[257];
    size_t n = e - 2 < 256 ? e - 2 : 256;
    memcpy(buf, content + 2, n);
    buf[n] = '\0';
    if (strstr(buf, "perl"))
        return "perl";
    if (strstr(buf, "python"))
        return "python";
    if (strstr(buf, "bash") || strstr(buf, "/sh") || strstr(buf, "env sh") ||
        strstr(buf, "zsh") || strstr(buf, "ksh"))
        return "bash";
    if (strstr(buf, "ruby"))
        return "ruby";
    if (strstr(buf, "php"))
        return "php";
    if (strstr(buf, "node"))
        return "javascript";
    return "unknown";
}

static void ctx_free(pctx_t *c)
{
    for (int k = 0; k < c->nsub; k++)
    {
        free(c->subs[k].name);
        free(c->subs[k].qname);
        free(c->subs[k].pkg);
        free(c->subs[k].id);
    }
    free(c->subs);
    for (int k = 0; k < c->npkg; k++)
    {
        free(c->pkgs[k].name);
        free(c->pkgs[k].id);
    }
    free(c->pkgs);
    if (c->scope_vars)
    {
        for (int k = 0; k <= c->nsub; k++)
        {
            if (c->scope_vars[k])
                tt_hashmap_free_with_values(c->scope_vars[k]);
        }
        free(c->scope_vars);
    }
    free(c->tok_sub);
    free(c->shift_param);
    if (c->id_counts)
        tt_hashmap_free(c->id_counts);
    tt_line_offsets_free(&c->lo);
}

int tt_parse_perl_content(const char *rel_path, const char *content, size_t len,
                          tt_perl_result_t *out)
{
    if (!rel_path || !content || !out)
        return -1;
    if (len > (size_t)0x7FFFFFF0)
        return -1;

    plex_t L;
    memset(&L, 0, sizeof(L));
    L.s = content;
    L.len = (int)len;
    L.line = 1;
    lex(&L);
    if (!L.oom)
        match_brackets(&L);

    pctx_t c;
    memset(&c, 0, sizeof(c));
    c.rel_path = rel_path;
    c.L = &L;
    c.out = out;
    tt_line_offsets_build(&c.lo, content, len);
    c.id_counts = tt_hashmap_new(64);
    c.tok_sub = malloc((size_t)(L.ntok > 0 ? L.ntok : 1) * sizeof(int));
    c.shift_param = malloc((size_t)(L.ntok > 0 ? L.ntok : 1) * sizeof(int));
    int rc = 0;
    if (L.oom || !c.id_counts || !c.tok_sub || !c.shift_param)
    {
        rc = -1;
        goto done;
    }
    for (int i = 0; i < L.ntok; i++)
    {
        c.tok_sub[i] = -1;
        c.shift_param[i] = -1;
    }

    find_structure(&c);
    if (c.oom)
    {
        rc = -1;
        goto done;
    }
    c.scope_vars = calloc((size_t)c.nsub + 1, sizeof(tt_hashmap_t *));
    if (!c.scope_vars)
    {
        rc = -1;
        goto done;
    }

    emit_packages(&c);
    emit_subs(&c);
    emit_statement_symbols(&c);
    emit_pod_headings(&c);
    extract_imports(&c);
    extract_sites(&c);

    /* Entry points: shebang + no package declaration. */
    if (tt_perl_shebang_language(content, len) && c.npkg == 0)
    {
        const char *dot = strrchr(rel_path, '.');
        if (dot && tt_strcasecmp(dot, ".cgi") == 0)
            out->file_flags |= TT_FILE_FLAG_CGI;
        else
            out->file_flags |= TT_FILE_FLAG_SCRIPT;
    }
    if (c.oom)
        rc = -1;

done:
    ctx_free(&c);
    free(L.toks);
    free(L.heads);
    free(L.pods);
    return rc;
}

void tt_perl_result_free(tt_perl_result_t *r)
{
    if (!r)
        return;
    if (r->symbols)
        tt_symbol_array_free(r->symbols, r->sym_count);
    if (r->imports)
        tt_import_array_free(r->imports, r->imp_count);
    if (r->sites)
        tt_site_array_free(r->sites, r->site_count);
    memset(r, 0, sizeof(*r));
}
