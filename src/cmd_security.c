/*
 * cmd_security.c -- Security queries over parser-recorded sites.
 *
 * Listings (sinks, sources, sanitizers, ACL gates) read the sites table
 * joined with file entry flags, ordered by file, line, name.
 *
 * security:flow is taint-lite reachability over the recorded masks:
 *   - hop 0: a sink whose argument mask carries source bits;
 *   - hop n: a call passing a source (or a tainted parameter) as argument i
 *     reaches the callee's parameter i; sinks in the callee whose mask
 *     carries that parameter close the chain.
 * Masks are flow-insensitive and field-insensitive per scope; callees are
 * resolved heuristically (same file > import closure > Webmin module or
 * package > unique name). Unresolved tainted calls are reported as unknown.
 * Calls that are themselves sinks or sanitizers are not descended into:
 * the wrapper call site is the reported sink.
 */

#include "cmd_security.h"
#include "arena.h"
#include "database.h"
#include "error.h"
#include "hashmap.h"
#include "json_output.h"
#include "output_fmt.h"
#include "parser_perl.h"
#include "platform.h"
#include "site.h"
#include "storage_paths.h"
#include "str_util.h"

#include <sqlite3.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEC_DEFAULT_LIMIT 100
#define FLOW_DEFAULT_LIMIT 50
#define FLOW_DEFAULT_DEPTH 3
#define FLOW_MAX_DEPTH 6
#define FLOW_MAX_TAILS 32
#define FLOW_CLOSURE_DEPTH 4
#define FLOW_ALL_PARAMS 16 /* pseudo parameter index: @_ forwarded as a whole */

#define TAINT_PARAM_FLAG (1u << 31) /* --taint param (query filter only) */

/* ---- Shared helpers ---- */

static cJSON *make_error(const char *code, const char *message, const char *hint)
{
    cJSON *json = cJSON_CreateObject();
    if (!json)
        return NULL;
    cJSON_AddStringToObject(json, "error", code);
    cJSON_AddStringToObject(json, "message", message);
    if (hint && hint[0])
        cJSON_AddStringToObject(json, "hint", hint);
    return json;
}

static const struct
{
    const char *name;
    uint32_t bit;
} TAINT_NAMES[] = {
    {"in", TT_TAINT_IN},         {"env", TT_TAINT_ENV},   {"argv", TT_TAINT_ARGV},
    {"config", TT_TAINT_CONFIG}, {"file", TT_TAINT_FILE}, {"stdin", TT_TAINT_STDIN},
};
#define TAINT_NAME_COUNT ((int)(sizeof(TAINT_NAMES) / sizeof(TAINT_NAMES[0])))

/* Source site name (as recorded by the parser) -> origin bit. */
static uint32_t source_name_bit(const char *name)
{
    if (!name)
        return 0;
    if (strcmp(name, "in") == 0)
        return TT_TAINT_IN;
    if (strcmp(name, "ENV") == 0)
        return TT_TAINT_ENV;
    if (strcmp(name, "ARGV") == 0)
        return TT_TAINT_ARGV;
    if (strcmp(name, "config") == 0)
        return TT_TAINT_CONFIG;
    if (strcmp(name, "read_file") == 0)
        return TT_TAINT_FILE;
    if (strcmp(name, "STDIN") == 0)
        return TT_TAINT_STDIN;
    return 0;
}

/* Trace witness variable ("%in", "%config", "<STDIN>", ...) -> origin bit. */
static uint32_t witness_bit(const char *var, size_t len)
{
    static const struct
    {
        const char *key;
        uint32_t bit;
    } W[] = {
        {"%in", TT_TAINT_IN},         {"$in", TT_TAINT_IN},       {"%ENV", TT_TAINT_ENV},
        {"@ARGV", TT_TAINT_ARGV},     {"%config", TT_TAINT_CONFIG}, {"%gconfig", TT_TAINT_CONFIG},
        {"read_file", TT_TAINT_FILE}, {"<STDIN>", TT_TAINT_STDIN},
    };
    for (size_t i = 0; i < sizeof(W) / sizeof(W[0]); i++)
    {
        if (strlen(W[i].key) == len && strncmp(W[i].key, var, len) == 0)
            return W[i].bit;
    }
    return 0;
}

static cJSON *taint_array(uint32_t mask)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < TAINT_NAME_COUNT; i++)
    {
        if (mask & TAINT_NAMES[i].bit)
            cJSON_AddItemToArray(arr, cJSON_CreateString(TAINT_NAMES[i].name));
    }
    return arr;
}

/* Parameter indexes carried by a mask, or "all" when @_ is used whole. */
static cJSON *params_json(uint32_t mask)
{
    if (mask & TT_TAINT_ALL_ARGS)
        return cJSON_CreateString("all");
    cJSON *arr = cJSON_CreateArray();
    for (int k = 0; k < TT_TAINT_MAX_PARAMS; k++)
    {
        if (mask & TT_TAINT_PARAM(k))
            cJSON_AddItemToArray(arr, cJSON_CreateNumber(k));
    }
    return arr;
}

/* ---- Query options ---- */

typedef struct
{
    const char *scope;
    char **names; /* --kind list */
    int name_count;
    uint32_t taint; /* source bits (| TAINT_PARAM_FLAG), 0 = no filter */
    bool unsanitized;
    int entry_flags; /* required file flags, 0 = any */
    int limit;
    int token_budget;
    bool compact;
    bool count;
    int depth;
    const tt_cli_opts_t *opts;
} sec_query_t;

static void sec_query_free(sec_query_t *q)
{
    tt_str_split_free(q->names);
    q->names = NULL;
}

/* Returns NULL on success, or an error object. */
static cJSON *sec_query_init(sec_query_t *q, const tt_cli_opts_t *opts, int default_limit)
{
    memset(q, 0, sizeof(*q));
    q->opts = opts;
    q->scope = opts->positional_count > 0 ? opts->positional[0] : NULL;
    q->limit = opts->limit > 0 ? opts->limit : default_limit;
    q->token_budget = opts->token_budget > 0 ? opts->token_budget : 0;
    q->compact = opts->compact;
    q->count = opts->count;
    q->unsanitized = opts->unsanitized;
    q->depth = opts->depth > 0 ? opts->depth : FLOW_DEFAULT_DEPTH;
    if (q->depth > FLOW_MAX_DEPTH)
        q->depth = FLOW_MAX_DEPTH;

    if (opts->kind && opts->kind[0])
    {
        q->names = tt_str_split(opts->kind, ',', &q->name_count);
        for (int i = 0; i < q->name_count; i++)
            tt_str_trim(q->names[i]);
    }

    if (opts->taint && opts->taint[0])
    {
        int n = 0;
        char **parts = tt_str_split(opts->taint, ',', &n);
        for (int i = 0; i < n; i++)
        {
            char *t = tt_str_trim(parts[i]);
            if (!t[0])
                continue;
            uint32_t bit = 0;
            if (tt_strcasecmp(t, "any") == 0)
                bit = TT_TAINT_SOURCES;
            else if (tt_strcasecmp(t, "param") == 0 || tt_strcasecmp(t, "params") == 0)
                bit = TAINT_PARAM_FLAG;
            else
            {
                for (int k = 0; k < TAINT_NAME_COUNT; k++)
                {
                    if (tt_strcasecmp(t, TAINT_NAMES[k].name) == 0)
                        bit = TAINT_NAMES[k].bit;
                }
            }
            if (!bit)
            {
                cJSON *err = make_error("invalid_value", "Unknown --taint value.",
                                        "Valid: in, env, argv, config, file, stdin, param, any");
                cJSON_AddStringToObject(err, "invalid_value", t);
                tt_str_split_free(parts);
                sec_query_free(q);
                return err;
            }
            q->taint |= bit;
        }
        tt_str_split_free(parts);
    }

    if (opts->entry && opts->entry[0])
    {
        if (tt_strcasecmp(opts->entry, "cgi") == 0)
            q->entry_flags = TT_FILE_FLAG_CGI;
        else if (tt_strcasecmp(opts->entry, "unauth") == 0)
            q->entry_flags = TT_FILE_FLAG_UNAUTH;
        else if (tt_strcasecmp(opts->entry, "script") == 0)
            q->entry_flags = TT_FILE_FLAG_SCRIPT;
        else
        {
            cJSON *err = make_error("invalid_value", "Unknown --entry value.",
                                    "Valid: cgi, unauth, script");
            cJSON_AddStringToObject(err, "invalid_value", opts->entry);
            sec_query_free(q);
            return err;
        }
    }
    return NULL;
}

/* Scope: directory prefix, exact file, or glob. */
static bool scope_matches(const char *scope, const char *file)
{
    if (!scope || !scope[0] || strcmp(scope, ".") == 0)
        return true;
    if (strpbrk(scope, "*?["))
        return tt_fnmatch(scope, file, false);
    size_t n = strlen(scope);
    while (n > 0 && scope[n - 1] == '/')
        n--;
    if (n == 0)
        return true;
    if (strncmp(file, scope, n) != 0)
        return false;
    return file[n] == '\0' || file[n] == '/';
}

static bool file_selected(const sec_query_t *q, const char *file, int flags)
{
    if (!file)
        return false;
    if (!scope_matches(q->scope, file))
        return false;
    const tt_cli_opts_t *o = q->opts;
    if (o->file_glob && o->file_glob[0] && !tt_fnmatch(o->file_glob, file, true))
        return false;
    if (!tt_matches_path_filters(file, o->filter, o->exclude))
        return false;
    if (q->entry_flags && !(flags & q->entry_flags))
        return false;
    return true;
}

static bool name_selected(const sec_query_t *q, const char *name, const char *klass)
{
    if (q->name_count == 0)
        return true;
    for (int i = 0; i < q->name_count; i++)
    {
        if ((name && tt_strcasecmp(q->names[i], name) == 0) ||
            (klass && klass[0] && tt_strcasecmp(q->names[i], klass) == 0))
            return true;
    }
    return false;
}

static bool taint_selected(const sec_query_t *q, uint32_t mask)
{
    if (!q->taint)
        return true;
    if ((q->taint & TT_TAINT_SOURCES) & mask)
        return true;
    if ((q->taint & TAINT_PARAM_FLAG) && (mask & (TT_TAINT_PARAMS | TT_TAINT_ALL_ARGS)))
        return true;
    return false;
}

static void add_entry_flags(cJSON *obj, int flags, bool compact)
{
    bool cgi = (flags & TT_FILE_FLAG_CGI) != 0;
    bool ua = (flags & TT_FILE_FLAG_UNAUTH) != 0;
    if (compact)
    {
        if (cgi)
            cJSON_AddNumberToObject(obj, "cgi", 1);
        if (ua)
            cJSON_AddNumberToObject(obj, "ua", 1);
        return;
    }
    cJSON_AddBoolToObject(obj, "is_cgi", cgi);
    cJSON_AddBoolToObject(obj, "is_unauthenticated", ua);
}

/* Token estimate of a JSON value (bytes / 4, rounded up). */
static int json_tokens(const cJSON *item)
{
    char *s = cJSON_PrintUnformatted(item);
    if (!s)
        return 0;
    int t = (int)((strlen(s) + 3) / 4);
    free(s);
    return t;
}

/* Open the project index. Returns an error object, or NULL on success. */
static cJSON *open_index(const tt_cli_opts_t *opts, char **out_path, tt_database_t *db)
{
    *out_path = NULL;
    char *path = tt_resolve_project_path(opts->path);
    if (!path)
        return make_error("invalid_path", "Failed to resolve project path", NULL);
    if (!tt_database_exists(path))
    {
        free(path);
        return make_error("no_index", "No index found.", "Run \"toktoken index:create\" first.");
    }
    memset(db, 0, sizeof(*db));
    if (tt_database_open(db, path) < 0)
    {
        free(path);
        return make_error("storage_error", "Failed to open database", NULL);
    }
    *out_path = path;
    return NULL;
}

static bool index_has_sites(sqlite3 *db)
{
    sqlite3_stmt *st = NULL;
    bool any = false;
    if (sqlite3_prepare_v2(db, "SELECT EXISTS(SELECT 1 FROM sites)", -1, &st, NULL) == SQLITE_OK)
    {
        if (sqlite3_step(st) == SQLITE_ROW)
            any = sqlite3_column_int(st, 0) != 0;
        sqlite3_finalize(st);
    }
    return any;
}

static const char *NO_SITES_NOTE =
    "No security sites in this index. Sites are recorded by the Perl parser "
    "(.pl .pm .cgi .psgi); other languages are not analyzed yet.";

static const char *col_text(sqlite3_stmt *st, int i)
{
    const char *s = (const char *)sqlite3_column_text(st, i);
    return s ? s : "";
}

/* ---- Listings ---- */

typedef struct
{
    char *name;
    int count;
} name_count_t;

static int name_count_cmp(const void *a, const void *b)
{
    const name_count_t *x = *(const name_count_t *const *)a;
    const name_count_t *y = *(const name_count_t *const *)b;
    if (x->count != y->count)
        return y->count - x->count;
    return strcmp(x->name, y->name);
}

typedef struct
{
    tt_array_t items; /* name_count_t* */
    tt_hashmap_t *index;
} name_counter_t;

static void counter_add(name_counter_t *c, const char *name)
{
    intptr_t idx = (intptr_t)tt_hashmap_get(c->index, name);
    if (idx == 0)
    {
        name_count_t *nc = calloc(1, sizeof(*nc));
        if (!nc)
            return;
        nc->name = tt_strdup(name);
        tt_array_push(&c->items, nc);
        tt_hashmap_set(c->index, name, (void *)(intptr_t)c->items.len);
        idx = (intptr_t)c->items.len;
    }
    ((name_count_t *)c->items.items[idx - 1])->count++;
}

/* Counts ordered by count desc, name asc; frees the counter. */
static cJSON *counter_finish(name_counter_t *c)
{
    if (c->items.len > 1)
        qsort(c->items.items, c->items.len, sizeof(void *), name_count_cmp);
    cJSON *obj = cJSON_CreateObject();
    for (size_t i = 0; i < c->items.len; i++)
    {
        name_count_t *nc = c->items.items[i];
        cJSON_AddNumberToObject(obj, nc->name, nc->count);
        free(nc->name);
        free(nc);
    }
    tt_array_free(&c->items);
    tt_hashmap_free(c->index);
    c->index = NULL;
    return obj;
}

static void counter_init(name_counter_t *c)
{
    tt_array_init(&c->items);
    c->index = tt_hashmap_new(64);
}

typedef struct
{
    const char *file;
    int line;
    const char *name;
    const char *qualifier;
    const char *scope;
    const char *args;
    uint32_t mask;
    const char *trace;
    int flags;
} site_row_t;

static cJSON *site_entry(tt_site_category_e cat, const site_row_t *r, bool compact)
{
    cJSON *o = cJSON_CreateObject();
    bool sink = (cat == TT_SITE_SINK);
    uint32_t origins = r->mask & TT_TAINT_SOURCES;
    uint32_t params = r->mask & (TT_TAINT_PARAMS | TT_TAINT_ALL_ARGS);
    bool sanitized = (r->mask & TT_TAINT_SANITIZED) != 0;

    cJSON_AddStringToObject(o, compact ? "f" : "file", r->file);
    cJSON_AddNumberToObject(o, compact ? "l" : "line", r->line);
    cJSON_AddStringToObject(o, compact ? "n" : "name", r->name);
    if (sink && r->qualifier[0])
        cJSON_AddStringToObject(o, compact ? "c" : "class", r->qualifier);
    if (r->scope[0])
        cJSON_AddStringToObject(o, compact ? "s" : "scope", r->scope);
    if (r->args[0])
        cJSON_AddStringToObject(o, compact ? "a" : (cat == TT_SITE_SOURCE ? "key" : "args"), r->args);
    if (sink)
    {
        if (origins || !compact)
            cJSON_AddItemToObject(o, compact ? "t" : "taint", taint_array(origins));
        if (params)
            cJSON_AddItemToObject(o, compact ? "p" : "params", params_json(params));
        if (compact)
        {
            if (sanitized)
                cJSON_AddNumberToObject(o, "z", 1);
        }
        else
            cJSON_AddBoolToObject(o, "sanitized", sanitized);
        if (r->trace[0])
            cJSON_AddStringToObject(o, compact ? "w" : "trace", r->trace);
    }
    add_entry_flags(o, r->flags, compact);
    return o;
}

static const char *category_label(tt_site_category_e cat)
{
    switch (cat)
    {
    case TT_SITE_SINK:
        return "sink";
    case TT_SITE_SOURCE:
        return "source";
    case TT_SITE_SANITIZER:
        return "sanitizer";
    case TT_SITE_ACL:
        return "acl";
    default:
        return tt_site_category_str(cat);
    }
}

static cJSON *list_sites(tt_cli_opts_t *opts, tt_site_category_e cat)
{
    tt_timer_start();
    sec_query_t q;
    cJSON *err = sec_query_init(&q, opts, SEC_DEFAULT_LIMIT);
    if (err)
        return err;

    char *project_path = NULL;
    tt_database_t db;
    err = open_index(opts, &project_path, &db);
    if (err)
    {
        sec_query_free(&q);
        return err;
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "category", category_label(cat));
    if (!index_has_sites(db.db))
    {
        cJSON_AddNumberToObject(result, "count", 0);
        cJSON_AddStringToObject(result, "note", NO_SITES_NOTE);
        if (!q.count)
            cJSON_AddItemToObject(result, "results", cJSON_CreateArray());
        tt_database_close(&db);
        free(project_path);
        sec_query_free(&q);
        return result;
    }

    const char *sql =
        "SELECT s.file, s.line, s.name, s.qualifier, s.scope_id, s.args, s.mask, s.trace, "
        "COALESCE(f.flags, 0) FROM sites s LEFT JOIN files f ON f.path = s.file "
        "WHERE s.category = ? ORDER BY s.file, s.line, s.name, s.id";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db.db, sql, -1, &st, NULL) != SQLITE_OK)
    {
        cJSON_Delete(result);
        result = make_error("storage_error", sqlite3_errmsg(db.db), NULL);
        tt_database_close(&db);
        free(project_path);
        sec_query_free(&q);
        return result;
    }
    sqlite3_bind_text(st, 1, tt_site_category_str(cat), -1, SQLITE_STATIC);

    cJSON *results = q.count ? NULL : cJSON_CreateArray();
    name_counter_t by_name;
    counter_init(&by_name);
    int total = 0, files = 0, emitted = 0, tokens = 0;
    bool budget_hit = false;
    char *last_file = NULL;

    while (sqlite3_step(st) == SQLITE_ROW)
    {
        site_row_t r;
        r.file = col_text(st, 0);
        r.line = sqlite3_column_int(st, 1);
        r.name = col_text(st, 2);
        r.qualifier = col_text(st, 3);
        r.scope = col_text(st, 4);
        r.args = col_text(st, 5);
        r.mask = (uint32_t)sqlite3_column_int64(st, 6);
        r.trace = col_text(st, 7);
        r.flags = sqlite3_column_int(st, 8);

        if (!file_selected(&q, r.file, r.flags))
            continue;
        if (!name_selected(&q, r.name, cat == TT_SITE_SINK ? r.qualifier : NULL))
            continue;
        if (cat == TT_SITE_SINK && !taint_selected(&q, r.mask))
            continue;
        if (q.unsanitized && (r.mask & TT_TAINT_SANITIZED))
            continue;

        total++;
        counter_add(&by_name, r.name);
        if (!last_file || strcmp(last_file, r.file) != 0)
        {
            files++;
            free(last_file);
            last_file = tt_strdup(r.file);
        }
        if (!results || emitted >= q.limit || budget_hit)
            continue;

        cJSON *item = site_entry(cat, &r, q.compact);
        if (q.token_budget > 0)
        {
            int t = json_tokens(item);
            if (emitted > 0 && tokens + t > q.token_budget)
            {
                cJSON_Delete(item);
                budget_hit = true;
                continue;
            }
            tokens += t;
        }
        cJSON_AddItemToArray(results, item);
        emitted++;
    }
    sqlite3_finalize(st);
    free(last_file);

    cJSON_AddNumberToObject(result, "count", total);
    cJSON_AddNumberToObject(result, "files", files);
    if (q.count)
    {
        cJSON_AddItemToObject(result, "by_name", counter_finish(&by_name));
    }
    else
    {
        cJSON_Delete(counter_finish(&by_name));
        cJSON_AddNumberToObject(result, "n", emitted);
        cJSON_AddBoolToObject(result, "truncated", emitted < total);
        cJSON_AddItemToObject(result, "results", results);
        if (q.token_budget > 0)
        {
            cJSON_AddNumberToObject(result, "token_budget", q.token_budget);
            cJSON_AddNumberToObject(result, "tokens_used", tokens);
        }
    }

    tt_database_close(&db);
    free(project_path);
    sec_query_free(&q);
    return result;
}

cJSON *tt_cmd_security_sinks_exec(tt_cli_opts_t *opts)
{
    return list_sites(opts, TT_SITE_SINK);
}

cJSON *tt_cmd_security_sources_exec(tt_cli_opts_t *opts)
{
    return list_sites(opts, TT_SITE_SOURCE);
}

cJSON *tt_cmd_security_sanitizers_exec(tt_cli_opts_t *opts)
{
    return list_sites(opts, TT_SITE_SANITIZER);
}

cJSON *tt_cmd_security_acl_gates_exec(tt_cli_opts_t *opts)
{
    return list_sites(opts, TT_SITE_ACL);
}

/* ---- Flow ---- */

typedef struct
{
    int *v;
    int n, cap;
} ilist_t;

static void ilist_push(ilist_t *l, int x)
{
    if (l->n == l->cap)
    {
        int nc = l->cap ? l->cap * 2 : 4;
        int *nv = realloc(l->v, (size_t)nc * sizeof(int));
        if (!nv)
            return;
        l->v = nv;
        l->cap = nc;
    }
    l->v[l->n++] = x;
}

typedef struct
{
    const char *file;
    int line;
    const char *name;
    const char *klass;
    const char *args;
    const char *trace;
    uint32_t mask;
    int scope; /* scope index */
} fsink_t;

typedef struct
{
    const char *file;
    int line;
    const char *name;
    const char *qual;
    const char *args;
    uint32_t am[TT_TAINT_MAX_PARAMS];
    int nargs;
    int scope;
    int callee; /* -1 = not resolved yet, -2 = unresolved, else sub index */
    bool wrapper;
} fcall_t;

typedef struct
{
    const char *file;
    int line;
    const char *name;
    const char *args;
} fsrc_t;

typedef struct
{
    const char *id;
    const char *name;
    const char *file;
    const char *qname;
    int next_same_name; /* next sub with the same name, or -1 */
} fsub_t;

typedef struct
{
    const char *key;  /* sub ID, or file path for file scope */
    const char *file;
    const char *sub_id; /* NULL for file scope */
    ilist_t sinks, calls, srcs; /* ordered by line */
} fscope_t;

typedef struct ftail
{
    int call; /* call index, -1 for the final sink step */
    int arg;  /* argument position at call */
    int sink;
    int hops;
    struct ftail *next;
} ftail_t;

typedef struct
{
    ftail_t **v;
    int n;
    int state; /* 0 = new, 1 = computing, 2 = done */
} tails_t;

typedef struct
{
    const char *src;
    const char *dst;
} fedge_t;

typedef struct
{
    tt_arena_t *arena;
    fsink_t *sinks;
    int nsinks;
    fcall_t *calls;
    int ncalls;
    fsrc_t *srcs;
    int nsrcs;
    fsub_t *subs;
    int nsubs;
    fscope_t *scopes;
    int nscopes, capscopes;
    fedge_t *edges;
    int nedges;
    tt_hashmap_t *scope_index;  /* key -> scope idx + 1 */
    tt_hashmap_t *sub_by_name;  /* name -> first sub idx + 1 */
    tt_hashmap_t *sub_by_fname; /* "file\tname" -> sub idx + 1 */
    tt_hashmap_t *sub_by_qname; /* qualified name -> sub idx + 1 */
    tt_hashmap_t *edge_start;   /* src -> first edge idx + 1 */
    tt_hashmap_t *file_flags;   /* file -> flags + 1 */
    tt_hashmap_t *wrappers;     /* "file\tline\tname" of sink/sanitizer sites */
    tt_hashmap_t *memo;         /* "scope:k:depth" -> tails_t* */
    tt_hashmap_t *unknown_seen; /* "file\tline\tname" -> 1 */
    tt_array_t unknown;         /* unknown_t* */
    tt_array_t memo_items;      /* tails_t* (for freeing) */
    bool capped;
} flow_ctx_t;

typedef struct
{
    int call;
    int arg;
    uint32_t bits; /* source bits when reached from a source, 0 = via parameter */
} unknown_t;

static int get_index(tt_hashmap_t *m, const char *key)
{
    intptr_t v = (intptr_t)tt_hashmap_get(m, key);
    return v > 0 ? (int)(v - 1) : -1;
}

static void set_index(tt_hashmap_t *m, const char *key, int idx)
{
    tt_hashmap_set(m, key, (void *)(intptr_t)(idx + 1));
}

static int scope_for(flow_ctx_t *fc, const char *scope_id, const char *file)
{
    const char *key = (scope_id && scope_id[0]) ? scope_id : file;
    int idx = get_index(fc->scope_index, key);
    if (idx >= 0)
        return idx;
    if (fc->nscopes == fc->capscopes)
    {
        int nc = fc->capscopes ? fc->capscopes * 2 : 1024;
        fscope_t *ns = realloc(fc->scopes, (size_t)nc * sizeof(fscope_t));
        if (!ns)
            return -1;
        fc->scopes = ns;
        fc->capscopes = nc;
    }
    fscope_t *s = &fc->scopes[fc->nscopes];
    memset(s, 0, sizeof(*s));
    s->key = tt_arena_strdup(fc->arena, key);
    s->file = tt_arena_strdup(fc->arena, file);
    s->sub_id = (scope_id && scope_id[0]) ? s->key : NULL;
    set_index(fc->scope_index, key, fc->nscopes);
    return fc->nscopes++;
}

static void wrapper_key(char *buf, size_t sz, const char *file, int line, const char *name)
{
    snprintf(buf, sz, "%s\t%d\t%s", file, line, name);
}

/* Parse "[1,0,256]" into masks. */
static int parse_masks(const char *json, uint32_t *out)
{
    int n = 0;
    const char *p = json;
    if (!p || *p != '[')
        return 0;
    p++;
    while (*p && *p != ']' && n < TT_TAINT_MAX_PARAMS)
    {
        while (*p == ' ' || *p == ',')
            p++;
        if (!isdigit((unsigned char)*p))
            break;
        out[n++] = (uint32_t)strtoul(p, (char **)&p, 10);
    }
    return n;
}

static int flow_load(flow_ctx_t *fc, sqlite3 *db)
{
    sqlite3_stmt *st = NULL;
    tt_arena_t *a = fc->arena;

    /* Files and entry flags */
    if (sqlite3_prepare_v2(db, "SELECT path, flags FROM files", -1, &st, NULL) == SQLITE_OK)
    {
        while (sqlite3_step(st) == SQLITE_ROW)
            set_index(fc->file_flags, col_text(st, 0), sqlite3_column_int(st, 1));
        sqlite3_finalize(st);
    }

    /* Subroutines (Perl) */
    int cap = 0;
    if (sqlite3_prepare_v2(db,
                           "SELECT id, name, file, qualified_name FROM symbols "
                           "WHERE language = 'perl' AND kind IN ('subroutine', 'method') "
                           "ORDER BY file, line, id",
                           -1, &st, NULL) == SQLITE_OK)
    {
        while (sqlite3_step(st) == SQLITE_ROW)
        {
            if (fc->nsubs == cap)
            {
                cap = cap ? cap * 2 : 4096;
                fsub_t *ns = realloc(fc->subs, (size_t)cap * sizeof(fsub_t));
                if (!ns)
                    break;
                fc->subs = ns;
            }
            fsub_t *s = &fc->subs[fc->nsubs];
            s->id = tt_arena_strdup(a, col_text(st, 0));
            s->name = tt_arena_strdup(a, col_text(st, 1));
            s->file = tt_arena_strdup(a, col_text(st, 2));
            s->qname = tt_arena_strdup(a, col_text(st, 3));
            s->next_same_name = -1;
            int first = get_index(fc->sub_by_name, s->name);
            if (first < 0)
                set_index(fc->sub_by_name, s->name, fc->nsubs);
            else
            {
                int k = first;
                while (fc->subs[k].next_same_name >= 0)
                    k = fc->subs[k].next_same_name;
                fc->subs[k].next_same_name = fc->nsubs;
            }
            char key[1024];
            snprintf(key, sizeof(key), "%s\t%s", s->file, s->name);
            if (!tt_hashmap_has(fc->sub_by_fname, key))
                set_index(fc->sub_by_fname, key, fc->nsubs);
            if (s->qname[0] && !tt_hashmap_has(fc->sub_by_qname, s->qname))
                set_index(fc->sub_by_qname, s->qname, fc->nsubs);
            fc->nsubs++;
        }
        sqlite3_finalize(st);
    }

    /* Resolved import edges, ordered for deterministic closures */
    cap = 0;
    if (sqlite3_prepare_v2(db,
                           "SELECT DISTINCT source_file, resolved_file FROM imports "
                           "WHERE resolved_file != '' AND resolved_file != source_file "
                           "ORDER BY source_file, resolved_file",
                           -1, &st, NULL) == SQLITE_OK)
    {
        while (sqlite3_step(st) == SQLITE_ROW)
        {
            if (fc->nedges == cap)
            {
                cap = cap ? cap * 2 : 4096;
                fedge_t *ne = realloc(fc->edges, (size_t)cap * sizeof(fedge_t));
                if (!ne)
                    break;
                fc->edges = ne;
            }
            fedge_t *e = &fc->edges[fc->nedges];
            e->src = tt_arena_strdup(a, col_text(st, 0));
            e->dst = tt_arena_strdup(a, col_text(st, 1));
            if (!tt_hashmap_has(fc->edge_start, e->src))
                set_index(fc->edge_start, e->src, fc->nedges);
            fc->nedges++;
        }
        sqlite3_finalize(st);
    }

    /* Sinks, sanitizers (wrappers), sources, calls -- ordered by file, line */
    if (sqlite3_prepare_v2(db,
                           "SELECT category, file, line, name, qualifier, scope_id, args, mask, "
                           "arg_masks, trace FROM sites "
                           "WHERE category IN ('sink', 'sanitizer', 'acl', 'source') "
                           "OR (category = 'call' AND arg_masks != '') "
                           "ORDER BY file, line, name, id",
                           -1, &st, NULL) != SQLITE_OK)
        return -1;
    int capk = 0, capc = 0, caps = 0;
    while (sqlite3_step(st) == SQLITE_ROW)
    {
        const char *cat = col_text(st, 0);
        const char *file = col_text(st, 1);
        int line = sqlite3_column_int(st, 2);
        const char *name = col_text(st, 3);
        const char *scope_id = col_text(st, 5);
        int sc = scope_for(fc, scope_id, file);
        if (sc < 0)
            continue;

        if (strcmp(cat, "sink") == 0 || strcmp(cat, "sanitizer") == 0 || strcmp(cat, "acl") == 0)
        {
            char key[1024];
            wrapper_key(key, sizeof(key), file, line, name);
            tt_hashmap_set(fc->wrappers, key, (void *)1);
            if (strcmp(cat, "sink") != 0)
                continue;
            if (fc->nsinks == capk)
            {
                capk = capk ? capk * 2 : 4096;
                fsink_t *nk = realloc(fc->sinks, (size_t)capk * sizeof(fsink_t));
                if (!nk)
                    break;
                fc->sinks = nk;
            }
            fsink_t *k = &fc->sinks[fc->nsinks];
            k->file = fc->scopes[sc].file;
            k->line = line;
            k->name = tt_arena_strdup(a, name);
            k->klass = tt_arena_strdup(a, col_text(st, 4));
            k->args = tt_arena_strdup(a, col_text(st, 6));
            k->mask = (uint32_t)sqlite3_column_int64(st, 7);
            k->trace = tt_arena_strdup(a, col_text(st, 9));
            k->scope = sc;
            ilist_push(&fc->scopes[sc].sinks, fc->nsinks);
            fc->nsinks++;
        }
        else if (strcmp(cat, "source") == 0)
        {
            if (fc->nsrcs == caps)
            {
                caps = caps ? caps * 2 : 8192;
                fsrc_t *ns = realloc(fc->srcs, (size_t)caps * sizeof(fsrc_t));
                if (!ns)
                    break;
                fc->srcs = ns;
            }
            fsrc_t *s = &fc->srcs[fc->nsrcs];
            s->file = fc->scopes[sc].file;
            s->line = line;
            s->name = tt_arena_strdup(a, name);
            s->args = tt_arena_strdup(a, col_text(st, 6));
            ilist_push(&fc->scopes[sc].srcs, fc->nsrcs);
            fc->nsrcs++;
        }
        else
        {
            if (fc->ncalls == capc)
            {
                capc = capc ? capc * 2 : 8192;
                fcall_t *nc = realloc(fc->calls, (size_t)capc * sizeof(fcall_t));
                if (!nc)
                    break;
                fc->calls = nc;
            }
            fcall_t *c = &fc->calls[fc->ncalls];
            memset(c, 0, sizeof(*c));
            c->file = fc->scopes[sc].file;
            c->line = line;
            c->name = tt_arena_strdup(a, name);
            c->qual = tt_arena_strdup(a, col_text(st, 4));
            c->args = tt_arena_strdup(a, col_text(st, 6));
            c->nargs = parse_masks(col_text(st, 8), c->am);
            c->scope = sc;
            c->callee = -1;
            ilist_push(&fc->scopes[sc].calls, fc->ncalls);
            fc->ncalls++;
        }
    }
    sqlite3_finalize(st);

    /* Calls that are also sink/sanitizer/ACL sites are wrappers (terminal) */
    for (int i = 0; i < fc->ncalls; i++)
    {
        char key[1024];
        wrapper_key(key, sizeof(key), fc->calls[i].file, fc->calls[i].line, fc->calls[i].name);
        fc->calls[i].wrapper = tt_hashmap_has(fc->wrappers, key);
    }
    return 0;
}

static int lookup_in_file(flow_ctx_t *fc, const char *file, const char *name)
{
    char key[1024];
    snprintf(key, sizeof(key), "%s\t%s", file, name);
    return get_index(fc->sub_by_fname, key);
}

/* Breadth-first over resolved imports (depth-limited); first file defining name wins. */
static int lookup_in_closure(flow_ctx_t *fc, const char *file, const char *name, int max_depth)
{
    tt_array_t queue;
    tt_array_init(&queue);
    tt_hashmap_t *seen = tt_hashmap_new(64);
    tt_array_push(&queue, (void *)file);
    tt_hashmap_set(seen, file, (void *)1);
    int found = -1;
    size_t head = 0, level_end = 1;
    int depth = 0;
    while (head < queue.len && found < 0)
    {
        const char *f = queue.items[head++];
        if (depth > 0)
            found = lookup_in_file(fc, f, name);
        if (found >= 0)
            break;
        if (depth < max_depth)
        {
            int e = get_index(fc->edge_start, f);
            for (; e >= 0 && e < fc->nedges && strcmp(fc->edges[e].src, f) == 0; e++)
            {
                if (!tt_hashmap_has(seen, fc->edges[e].dst))
                {
                    tt_hashmap_set(seen, fc->edges[e].dst, (void *)1);
                    tt_array_push(&queue, (void *)fc->edges[e].dst);
                }
            }
        }
        if (head == level_end)
        {
            depth++;
            level_end = queue.len;
        }
    }
    tt_hashmap_free(seen);
    tt_array_free(&queue);
    return found;
}

static int unique_by_name(flow_ctx_t *fc, const char *name)
{
    int first = get_index(fc->sub_by_name, name);
    if (first < 0 || fc->subs[first].next_same_name >= 0)
        return -1;
    return first;
}

static int resolve_call(flow_ctx_t *fc, fcall_t *c)
{
    if (c->callee != -1)
        return c->callee;
    int r = -1;
    const char *q = c->qual;
    if (!q[0])
    {
        r = lookup_in_file(fc, c->file, c->name);
        if (r < 0)
            r = lookup_in_closure(fc, c->file, c->name, FLOW_CLOSURE_DEPTH);
        if (r < 0)
            r = unique_by_name(fc, c->name);
    }
    else if (strcmp(q, "->") == 0)
    {
        r = unique_by_name(fc, c->name); /* no receiver types: unique name only */
    }
    else
    {
        char qn[512];
        snprintf(qn, sizeof(qn), "%s::%s", q, c->name);
        r = get_index(fc->sub_by_qname, qn);
        if (r < 0 && !strstr(q, "::"))
        {
            /* Webmin module: <mod>/<mod>-lib.pl (package uses '_' for '-') */
            char mod[256], lib[600];
            snprintf(mod, sizeof(mod), "%s", q);
            for (char *p = mod; *p; p++)
            {
                if (*p == '_')
                    *p = '-';
            }
            snprintf(lib, sizeof(lib), "%s/%s-lib.pl", mod, mod);
            r = lookup_in_file(fc, lib, c->name);
            if (r < 0)
                r = lookup_in_closure(fc, lib, c->name, 2);
        }
    }
    c->callee = r >= 0 ? r : -2;
    return c->callee;
}

static int sub_scope(flow_ctx_t *fc, int sub)
{
    return get_index(fc->scope_index, fc->subs[sub].id);
}

static void record_unknown(flow_ctx_t *fc, int call, int arg, uint32_t bits)
{
    char key[1024];
    wrapper_key(key, sizeof(key), fc->calls[call].file, fc->calls[call].line, fc->calls[call].name);
    if (tt_hashmap_has(fc->unknown_seen, key))
        return;
    tt_hashmap_set(fc->unknown_seen, key, (void *)1);
    unknown_t *u = malloc(sizeof(*u));
    if (!u)
        return;
    u->call = call;
    u->arg = arg;
    u->bits = bits;
    tt_array_push(&fc->unknown, u);
}

static bool sink_uses_param(uint32_t mask, int k)
{
    if (mask & TT_TAINT_ALL_ARGS)
        return true;
    if (k == FLOW_ALL_PARAMS)
        return (mask & TT_TAINT_PARAMS) != 0;
    return (mask & TT_TAINT_PARAM(k)) != 0;
}

/* Callee parameter receiving parameter k through argument j, or -1. */
static int param_through_arg(uint32_t am, int k, int j)
{
    if (am & TT_TAINT_ALL_ARGS)
    {
        if (k == FLOW_ALL_PARAMS)
            return FLOW_ALL_PARAMS;
        return (k + j < TT_TAINT_MAX_PARAMS) ? k + j : FLOW_ALL_PARAMS;
    }
    if (k == FLOW_ALL_PARAMS)
        return (am & TT_TAINT_PARAMS) ? j : -1;
    return (am & TT_TAINT_PARAM(k)) ? j : -1;
}

static int tail_cmp(const void *a, const void *b)
{
    const ftail_t *x = *(const ftail_t *const *)a;
    const ftail_t *y = *(const ftail_t *const *)b;
    if (x->hops != y->hops)
        return x->hops - y->hops;
    return x->sink - y->sink;
}

/*
 * reach -- Sinks reachable from parameter k of scope sc within depth call
 * hops. Memoized; recursion through a scope already being computed yields
 * nothing (cycles).
 */
static tails_t *reach(flow_ctx_t *fc, int sc, int k, int depth)
{
    static tails_t EMPTY = {NULL, 0, 2};
    if (sc < 0)
        return &EMPTY;
    char key[48];
    snprintf(key, sizeof(key), "%d:%d:%d", sc, k, depth);
    tails_t *t = tt_hashmap_get(fc->memo, key);
    if (t)
        return t->state == 1 ? &EMPTY : t;
    t = calloc(1, sizeof(*t));
    if (!t)
        return &EMPTY;
    t->state = 1;
    tt_hashmap_set(fc->memo, key, t);
    tt_array_push(&fc->memo_items, t);

    tt_array_t cand;
    tt_array_init(&cand);
    fscope_t *s = &fc->scopes[sc];
    for (int i = 0; i < s->sinks.n; i++)
    {
        int si = s->sinks.v[i];
        if (!sink_uses_param(fc->sinks[si].mask, k))
            continue;
        ftail_t *n = tt_arena_alloc(fc->arena, sizeof(ftail_t));
        n->call = -1;
        n->arg = -1;
        n->sink = si;
        n->hops = 0;
        n->next = NULL;
        tt_array_push(&cand, n);
    }
    if (depth > 0)
    {
        for (int i = 0; i < s->calls.n; i++)
        {
            int ci = s->calls.v[i];
            fcall_t *c = &fc->calls[ci];
            if (c->wrapper)
                continue;
            for (int j = 0; j < c->nargs; j++)
            {
                int ck = param_through_arg(c->am[j], k, j);
                if (ck < 0)
                    continue;
                int callee = resolve_call(fc, c);
                if (callee < 0)
                {
                    record_unknown(fc, ci, j, 0);
                    continue;
                }
                tails_t *sub = reach(fc, sub_scope(fc, callee), ck, depth - 1);
                for (int x = 0; x < sub->n; x++)
                {
                    ftail_t *n = tt_arena_alloc(fc->arena, sizeof(ftail_t));
                    n->call = ci;
                    n->arg = j;
                    n->sink = sub->v[x]->sink;
                    n->hops = sub->v[x]->hops + 1;
                    n->next = sub->v[x];
                    tt_array_push(&cand, n);
                }
            }
        }
    }

    /* Shortest chain per sink, capped */
    if (cand.len > 1)
        qsort(cand.items, cand.len, sizeof(void *), tail_cmp);
    t->v = calloc(cand.len ? cand.len : 1, sizeof(ftail_t *));
    tt_hashmap_t *seen = tt_hashmap_new(cand.len > 8 ? cand.len * 2 : 16);
    for (size_t i = 0; i < cand.len && t->v; i++)
    {
        ftail_t *n = cand.items[i];
        char sk[16];
        snprintf(sk, sizeof(sk), "%d", n->sink);
        if (tt_hashmap_has(seen, sk))
            continue;
        if (t->n >= FLOW_MAX_TAILS)
        {
            fc->capped = true;
            break;
        }
        tt_hashmap_set(seen, sk, (void *)1);
        t->v[t->n++] = n;
    }
    tt_hashmap_free(seen);
    tt_array_free(&cand);
    t->state = 2;
    return t;
}

typedef struct
{
    int origin_scope;
    const char *origin_file;
    int origin_line;
    int seed_call; /* -1 for hop 0 */
    int seed_arg;
    uint32_t bits;
    const ftail_t *tail; /* NULL for hop 0 */
    int sink;
    int hops;
    bool sanitized;
} flow_t;

static int flow_cmp(const void *a, const void *b)
{
    const flow_t *x = *(const flow_t *const *)a;
    const flow_t *y = *(const flow_t *const *)b;
    int c = strcmp(x->origin_file, y->origin_file);
    if (c)
        return c;
    if (x->origin_line != y->origin_line)
        return x->origin_line - y->origin_line;
    if (x->hops != y->hops)
        return x->hops - y->hops;
    return x->sink - y->sink;
}

static bool chain_sanitized(const flow_ctx_t *fc, const flow_t *f)
{
    if (fc->sinks[f->sink].mask & TT_TAINT_SANITIZED)
        return true;
    if (f->seed_call >= 0 && (fc->calls[f->seed_call].am[f->seed_arg] & TT_TAINT_SANITIZED))
        return true;
    for (const ftail_t *t = f->tail; t; t = t->next)
    {
        if (t->call >= 0 && (fc->calls[t->call].am[t->arg] & TT_TAINT_SANITIZED))
            return true;
    }
    return false;
}

static int file_flags_of(flow_ctx_t *fc, const char *file)
{
    int v = get_index(fc->file_flags, file);
    return v > 0 ? v : 0;
}

/* Text of top-level argument j in a raw argument list (commas inside
 * brackets or quotes do not split). Writes into buf. */
static void arg_text(const char *args, int j, char *buf, size_t sz)
{
    int depth = 0, idx = 0;
    char quote = 0;
    size_t n = 0;
    buf[0] = '\0';
    for (const char *p = args; *p; p++)
    {
        char ch = *p;
        if (quote)
        {
            if (ch == '\\' && p[1])
            {
                if (idx == j && n + 2 < sz)
                {
                    buf[n++] = ch;
                    buf[n++] = p[1];
                }
                p++;
                continue;
            }
            if (ch == quote)
                quote = 0;
        }
        else if (ch == '\'' || ch == '"')
            quote = ch;
        else if (ch == '(' || ch == '[' || ch == '{')
            depth++;
        else if ((ch == ')' || ch == ']' || ch == '}') && depth > 0)
            depth--;
        else if (ch == ',' && depth == 0)
        {
            if (idx == j)
                break;
            idx++;
            continue;
        }
        if (idx == j && n + 1 < sz)
            buf[n++] = ch;
    }
    buf[n] = '\0';
}

/* Source witness for a call seed: a source on the call line whose key appears
 * in argument j, else the last matching source at or before the call line in
 * the same scope, else the first matching one. Returns source idx or -1. */
static int call_witness(const flow_ctx_t *fc, const fcall_t *c, int j, uint32_t bits)
{
    const fscope_t *s = &fc->scopes[c->scope];
    char at[512];
    arg_text(c->args, j, at, sizeof(at));
    int best = -1, first = -1;
    for (int i = 0; i < s->srcs.n; i++)
    {
        const fsrc_t *src = &fc->srcs[s->srcs.v[i]];
        if (!(source_name_bit(src->name) & bits))
            continue;
        if (src->line == c->line && src->args[0] && at[0] && strstr(at, src->args))
            return s->srcs.v[i];
        if (first < 0)
            first = s->srcs.v[i];
        if (src->line <= c->line)
            best = s->srcs.v[i];
    }
    return best >= 0 ? best : first;
}

static void add_step_source(cJSON *chain, bool compact, const char *file, int line, const char *what)
{
    if (compact)
    {
        char buf[600];
        snprintf(buf, sizeof(buf), "%s:%d source %s", file, line, what);
        cJSON_AddItemToArray(chain, cJSON_CreateString(buf));
        return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "step", "source");
    cJSON_AddStringToObject(o, "file", file);
    cJSON_AddNumberToObject(o, "line", line);
    cJSON_AddStringToObject(o, "what", what);
    cJSON_AddItemToArray(chain, o);
}

static void add_step_call(flow_ctx_t *fc, cJSON *chain, bool compact, int ci, int arg)
{
    const fcall_t *c = &fc->calls[ci];
    const char *callee = c->callee >= 0 ? fc->subs[c->callee].id : c->name;
    if (compact)
    {
        char buf[700];
        snprintf(buf, sizeof(buf), "%s:%d call %s arg%d", c->file, c->line, callee, arg);
        cJSON_AddItemToArray(chain, cJSON_CreateString(buf));
        return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "step", "call");
    cJSON_AddStringToObject(o, "file", c->file);
    cJSON_AddNumberToObject(o, "line", c->line);
    cJSON_AddStringToObject(o, "callee", callee);
    cJSON_AddNumberToObject(o, "arg", arg);
    cJSON_AddStringToObject(o, "args", c->args);
    cJSON_AddItemToArray(chain, o);
}

static void add_step_sink(flow_ctx_t *fc, cJSON *chain, bool compact, int si)
{
    const fsink_t *k = &fc->sinks[si];
    if (compact)
    {
        char buf[700];
        snprintf(buf, sizeof(buf), "%s:%d sink %s", k->file, k->line, k->name);
        cJSON_AddItemToArray(chain, cJSON_CreateString(buf));
        return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "step", "sink");
    cJSON_AddStringToObject(o, "file", k->file);
    cJSON_AddNumberToObject(o, "line", k->line);
    cJSON_AddStringToObject(o, "name", k->name);
    if (k->klass[0])
        cJSON_AddStringToObject(o, "class", k->klass);
    cJSON_AddStringToObject(o, "args", k->args);
    if (fc->scopes[k->scope].sub_id)
        cJSON_AddStringToObject(o, "scope", fc->scopes[k->scope].sub_id);
    cJSON_AddItemToArray(chain, o);
}

static cJSON *flow_json(flow_ctx_t *fc, const flow_t *f, bool compact)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, compact ? "t" : "taint", taint_array(f->bits));
    cJSON_AddNumberToObject(o, compact ? "h" : "hops", f->hops);
    if (compact)
    {
        if (f->sanitized)
            cJSON_AddNumberToObject(o, "z", 1);
    }
    else
        cJSON_AddBoolToObject(o, "sanitized", f->sanitized);

    const fscope_t *os = &fc->scopes[f->origin_scope];
    if (!compact)
    {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "file", os->file);
        if (os->sub_id)
            cJSON_AddStringToObject(entry, "scope", os->sub_id);
        add_entry_flags(entry, file_flags_of(fc, os->file), false);
        cJSON_AddItemToObject(o, "entry", entry);
    }
    else
    {
        add_entry_flags(o, file_flags_of(fc, os->file), true);
    }

    cJSON *chain = cJSON_CreateArray();
    if (f->seed_call < 0)
    {
        /* Hop 0: witness from the sink trace ("var@line,...") */
        const fsink_t *k = &fc->sinks[f->sink];
        const char *tr = k->trace;
        const char *pick = NULL;
        size_t pick_len = 0;
        int pick_line = k->line;
        const char *p = tr;
        while (p && *p)
        {
            const char *comma = strchr(p, ',');
            size_t len = comma ? (size_t)(comma - p) : strlen(p);
            const char *at = memchr(p, '@', len);
            if (at)
            {
                size_t vlen = (size_t)(at - p);
                if (!pick || (witness_bit(p, vlen) & f->bits && !(witness_bit(pick, pick_len) & f->bits)))
                {
                    pick = p;
                    pick_len = vlen;
                    pick_line = atoi(at + 1);
                }
            }
            p = comma ? comma + 1 : NULL;
        }
        char what[128];
        if (pick)
            snprintf(what, sizeof(what), "%.*s", (int)(pick_len < 120 ? pick_len : 120), pick);
        else
            snprintf(what, sizeof(what), "argument");
        add_step_source(chain, compact, k->file, pick ? pick_line : k->line, what);
    }
    else
    {
        const fcall_t *c = &fc->calls[f->seed_call];
        int w = call_witness(fc, c, f->seed_arg, f->bits);
        if (w >= 0)
        {
            char what[200];
            const fsrc_t *src = &fc->srcs[w];
            if (src->args[0])
                snprintf(what, sizeof(what), "%s{%.150s}", src->name, src->args);
            else
                snprintf(what, sizeof(what), "%s", src->name);
            add_step_source(chain, compact, src->file, src->line, what);
        }
        else
            add_step_source(chain, compact, c->file, c->line, c->args);
        add_step_call(fc, chain, compact, f->seed_call, f->seed_arg);
        for (const ftail_t *t = f->tail; t; t = t->next)
        {
            if (t->call >= 0)
                add_step_call(fc, chain, compact, t->call, t->arg);
        }
    }
    add_step_sink(fc, chain, compact, f->sink);
    cJSON_AddItemToObject(o, compact ? "c" : "chain", chain);
    return o;
}

static bool flow_keep(flow_ctx_t *fc, const sec_query_t *q, flow_t *f, tt_hashmap_t *best,
                      tt_array_t *flows)
{
    const fsink_t *k = &fc->sinks[f->sink];
    if (!name_selected(q, k->name, k->klass))
        return false;
    f->sanitized = chain_sanitized(fc, f);
    if (q->unsanitized && f->sanitized)
        return false;
    /* One flow per (entry call site, sink): the shortest chain wins */
    char key[64];
    snprintf(key, sizeof(key), "%d:%d:%d", f->origin_scope, f->seed_call, f->sink);
    int prev = get_index(best, key);
    if (prev >= 0)
    {
        flow_t *old = flows->items[prev];
        if (old->hops <= f->hops)
            return false;
        *old = *f;
        return true;
    }
    flow_t *nf = malloc(sizeof(*nf));
    if (!nf)
        return false;
    *nf = *f;
    tt_array_push(flows, nf);
    set_index(best, key, (int)flows->len - 1);
    return true;
}

static void flow_ctx_free(flow_ctx_t *fc)
{
    for (int i = 0; i < fc->nscopes; i++)
    {
        free(fc->scopes[i].sinks.v);
        free(fc->scopes[i].calls.v);
        free(fc->scopes[i].srcs.v);
    }
    free(fc->scopes);
    free(fc->sinks);
    free(fc->calls);
    free(fc->srcs);
    free(fc->subs);
    free(fc->edges);
    for (size_t i = 0; i < fc->memo_items.len; i++)
    {
        tails_t *t = fc->memo_items.items[i];
        free(t->v);
        free(t);
    }
    tt_array_free(&fc->memo_items);
    tt_array_free_items(&fc->unknown);
    tt_hashmap_free(fc->scope_index);
    tt_hashmap_free(fc->sub_by_name);
    tt_hashmap_free(fc->sub_by_fname);
    tt_hashmap_free(fc->sub_by_qname);
    tt_hashmap_free(fc->edge_start);
    tt_hashmap_free(fc->file_flags);
    tt_hashmap_free(fc->wrappers);
    tt_hashmap_free(fc->memo);
    tt_hashmap_free(fc->unknown_seen);
    tt_arena_free(fc->arena);
}

/* Calls are loaded ordered by file, line: the call index is the sort key. */
static int unknown_cmp(const void *a, const void *b)
{
    const unknown_t *x = *(const unknown_t *const *)a;
    const unknown_t *y = *(const unknown_t *const *)b;
    return x->call - y->call;
}

static const char *FLOW_ANALYSIS =
    "taint-lite reachability: per-scope origin masks (flow-insensitive, field-insensitive); "
    "callees resolved by same file > import closure > Webmin module/package > unique name; "
    "calls to sink or sanitizer wrappers are reported at the wrapper; "
    "'sanitized' means a sanitizer call appears in the chain's arguments, not that it neutralizes the value.";

cJSON *tt_cmd_security_flow_exec(tt_cli_opts_t *opts)
{
    tt_timer_start();
    sec_query_t q;
    cJSON *err = sec_query_init(&q, opts, FLOW_DEFAULT_LIMIT);
    if (err)
        return err;
    uint32_t bits_filter = (q.taint & TT_TAINT_SOURCES) ? (q.taint & TT_TAINT_SOURCES) : TT_TAINT_SOURCES;

    char *project_path = NULL;
    tt_database_t db;
    err = open_index(opts, &project_path, &db);
    if (err)
    {
        sec_query_free(&q);
        return err;
    }

    cJSON *result = cJSON_CreateObject();
    if (!index_has_sites(db.db))
    {
        cJSON_AddNumberToObject(result, "count", 0);
        cJSON_AddStringToObject(result, "note", NO_SITES_NOTE);
        if (!q.count)
            cJSON_AddItemToObject(result, "flows", cJSON_CreateArray());
        tt_database_close(&db);
        free(project_path);
        sec_query_free(&q);
        return result;
    }

    flow_ctx_t fc;
    memset(&fc, 0, sizeof(fc));
    fc.arena = tt_arena_new(1 << 20);
    fc.scope_index = tt_hashmap_new(16384);
    fc.sub_by_name = tt_hashmap_new(16384);
    fc.sub_by_fname = tt_hashmap_new(16384);
    fc.sub_by_qname = tt_hashmap_new(16384);
    fc.edge_start = tt_hashmap_new(4096);
    fc.file_flags = tt_hashmap_new(8192);
    fc.wrappers = tt_hashmap_new(16384);
    fc.memo = tt_hashmap_new(16384);
    fc.unknown_seen = tt_hashmap_new(1024);
    tt_array_init(&fc.unknown);
    tt_array_init(&fc.memo_items);

    if (flow_load(&fc, db.db) < 0)
    {
        cJSON_Delete(result);
        result = make_error("storage_error", sqlite3_errmsg(db.db), NULL);
        flow_ctx_free(&fc);
        tt_database_close(&db);
        free(project_path);
        sec_query_free(&q);
        return result;
    }

    tt_array_t flows;
    tt_array_init(&flows);
    tt_hashmap_t *best = tt_hashmap_new(4096);

    /* Hop 0: sinks whose arguments carry sources directly */
    for (int i = 0; i < fc.nsinks; i++)
    {
        const fsink_t *k = &fc.sinks[i];
        uint32_t bits = k->mask & bits_filter;
        if (!bits)
            continue;
        if (!file_selected(&q, k->file, file_flags_of(&fc, k->file)))
            continue;
        flow_t f;
        memset(&f, 0, sizeof(f));
        f.origin_scope = k->scope;
        f.origin_file = k->file;
        f.origin_line = k->line;
        f.seed_call = -1;
        f.seed_arg = -1;
        f.bits = bits;
        f.sink = i;
        f.hops = 0;
        flow_keep(&fc, &q, &f, best, &flows);
    }

    /* Hop >= 1: calls passing sources into user-defined subroutines */
    for (int i = 0; i < fc.ncalls; i++)
    {
        fcall_t *c = &fc.calls[i];
        if (c->wrapper)
            continue;
        if (!file_selected(&q, c->file, file_flags_of(&fc, c->file)))
            continue;
        for (int j = 0; j < c->nargs; j++)
        {
            uint32_t bits = c->am[j] & bits_filter;
            if (!bits)
                continue;
            int callee = resolve_call(&fc, c);
            if (callee < 0)
            {
                record_unknown(&fc, i, j, bits);
                continue;
            }
            tails_t *tl = reach(&fc, sub_scope(&fc, callee), j, q.depth - 1);
            for (int x = 0; x < tl->n; x++)
            {
                flow_t f;
                memset(&f, 0, sizeof(f));
                f.origin_scope = c->scope;
                f.origin_file = c->file;
                f.origin_line = c->line;
                f.seed_call = i;
                f.seed_arg = j;
                f.bits = bits;
                f.tail = tl->v[x];
                f.sink = tl->v[x]->sink;
                f.hops = tl->v[x]->hops + 1;
                flow_keep(&fc, &q, &f, best, &flows);
            }
        }
    }
    tt_hashmap_free(best);

    if (flows.len > 1)
        qsort(flows.items, flows.len, sizeof(void *), flow_cmp);
    if (fc.unknown.len > 1)
        qsort(fc.unknown.items, fc.unknown.len, sizeof(void *), unknown_cmp);

    int total = (int)flows.len;
    cJSON_AddNumberToObject(result, "count", total);
    cJSON_AddNumberToObject(result, "unknown_count", (int)fc.unknown.len);
    if (q.count)
    {
        name_counter_t by_sink;
        counter_init(&by_sink);
        for (size_t i = 0; i < flows.len; i++)
            counter_add(&by_sink, fc.sinks[((flow_t *)flows.items[i])->sink].name);
        cJSON_AddItemToObject(result, "by_sink", counter_finish(&by_sink));
    }
    else
    {
        cJSON *arr = cJSON_CreateArray();
        int emitted = 0, tokens = 0;
        for (size_t i = 0; i < flows.len && emitted < q.limit; i++)
        {
            cJSON *item = flow_json(&fc, flows.items[i], q.compact);
            if (q.token_budget > 0)
            {
                int t = json_tokens(item);
                if (emitted > 0 && tokens + t > q.token_budget)
                {
                    cJSON_Delete(item);
                    break;
                }
                tokens += t;
            }
            cJSON_AddItemToArray(arr, item);
            emitted++;
        }
        cJSON_AddNumberToObject(result, "n", emitted);
        cJSON_AddBoolToObject(result, "truncated", emitted < total);
        cJSON_AddItemToObject(result, "flows", arr);

        cJSON *unk = cJSON_CreateArray();
        for (size_t i = 0; i < fc.unknown.len && (int)i < q.limit; i++)
        {
            const unknown_t *u = fc.unknown.items[i];
            const fcall_t *c = &fc.calls[u->call];
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, q.compact ? "f" : "file", c->file);
            cJSON_AddNumberToObject(o, q.compact ? "l" : "line", c->line);
            cJSON_AddStringToObject(o, q.compact ? "n" : "callee", c->name);
            if (c->qual[0])
                cJSON_AddStringToObject(o, q.compact ? "q" : "qualifier", c->qual);
            cJSON_AddNumberToObject(o, q.compact ? "g" : "arg", u->arg);
            if (u->bits)
                cJSON_AddItemToObject(o, q.compact ? "t" : "taint", taint_array(u->bits));
            else
                cJSON_AddStringToObject(o, q.compact ? "v" : "via", "parameter");
            cJSON_AddItemToArray(unk, o);
        }
        cJSON_AddItemToObject(result, "unknown", unk);
        if (q.token_budget > 0)
        {
            cJSON_AddNumberToObject(result, "token_budget", q.token_budget);
            cJSON_AddNumberToObject(result, "tokens_used", tokens);
        }
    }
    cJSON_AddNumberToObject(result, "depth", q.depth);
    if (fc.capped)
        cJSON_AddBoolToObject(result, "chains_capped", true);
    cJSON_AddStringToObject(result, "analysis", FLOW_ANALYSIS);

    tt_array_free_items(&flows);
    flow_ctx_free(&fc);
    tt_database_close(&db);
    free(project_path);
    sec_query_free(&q);
    return result;
}

/* ---- CLI wrappers ---- */

static int print_result(cJSON *result)
{
    if (!result)
    {
        const char *err = tt_error_get();
        return tt_output_error("internal_error", err && err[0] ? err : "unknown error", NULL);
    }
    if (cJSON_GetObjectItemCaseSensitive(result, "error"))
    {
        tt_json_print(result);
        cJSON_Delete(result);
        return 1;
    }
    tt_json_print(result);
    cJSON_Delete(result);
    return 0;
}

int tt_cmd_security_sinks(tt_cli_opts_t *opts)
{
    return print_result(tt_cmd_security_sinks_exec(opts));
}

int tt_cmd_security_sources(tt_cli_opts_t *opts)
{
    return print_result(tt_cmd_security_sources_exec(opts));
}

int tt_cmd_security_sanitizers(tt_cli_opts_t *opts)
{
    return print_result(tt_cmd_security_sanitizers_exec(opts));
}

int tt_cmd_security_acl_gates(tt_cli_opts_t *opts)
{
    return print_result(tt_cmd_security_acl_gates_exec(opts));
}

int tt_cmd_security_flow(tt_cli_opts_t *opts)
{
    return print_result(tt_cmd_security_flow_exec(opts));
}
