/*
 * index_pipeline.c -- Parallel indexing pipeline (POSIX).
 *
 * Architecture: K worker threads + 1 writer thread.
 *
 * Workers:
 *   - Write chunk file paths to temp file
 *   - Start ctags streaming process
 *   - Parse JSON lines with yyjson
 *   - Group entries by file, build symbols, extract imports
 *   - Push batches to MPSC queue
 *
 * Writer:
 *   - Polls MPSC queue for batches
 *   - Inserts files, symbols, imports into SQLite
 *   - Periodic commits for WAL performance
 *
 * Only the writer thread touches SQLite. Workers never call any SQLite function.
 *
 * Windows stub: index_pipeline_win.c
 */

#include "platform.h"

#ifndef TT_PLATFORM_WINDOWS

#include "index_pipeline.h"
#include "ctags_stream.h"
#include "source_analyzer.h"
#include "line_offsets.h"
#include "language_detector.h"
#include "fast_hash.h"
#include "symbol.h"
#include "symbol_kind.h"
#include "normalizer.h"
#include "platform.h"
#include "str_util.h"
#include "hashmap.h"
#include "arena.h"
#include "error.h"
#include "diagnostic.h"
#include "thread_pool.h"
#include "schema.h"
#include "parser_blade.h"
#include "parser_vue.h"
#include "parser_ejs.h"
#include "parser_nix.h"
#include "parser_gleam.h"
#include "parser_hcl.h"
#include "parser_asm.h"
#include "parser_graphql.h"
#include "parser_julia.h"
#include "parser_gdscript.h"
#include "parser_verse.h"
#include "parser_xml.h"
#include "parser_autohotkey.h"
#include "parser_openapi.h"

#include <yyjson.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#include <signal.h>

#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>

extern volatile sig_atomic_t tt_interrupted;

/* ===== Constants ===== */

#define BATCH_SYMBOL_THRESHOLD 1024
#define BACKPRESSURE_THRESHOLD 100000
#define TRANSACTION_BATCH_SIZE 50000

/* ===== Internal handle structure ===== */

typedef struct
{
    const char *project_root;
    const char *ctags_path;
    const char **file_paths; /* borrowed: slice of config file_paths */
    int file_count;
    int worker_id;
    int ctags_timeout_sec;
    int max_file_size_bytes;
    struct mpsc_queue *queue;
    _Atomic(int64_t) *pending_count;
    _Atomic(int) *error_flag;
    tt_progress_t *progress;
} worker_ctx_t;

typedef struct
{
    tt_database_t *db;
    tt_index_store_t *store;
    struct mpsc_queue *queue;
    _Atomic(int64_t) *pending_count;
    _Atomic(int) *error_flag;
    int num_workers;
    /* Output: error messages collected */
    char **errors;
    int error_count;
    int error_cap;
    /* Output: counters */
    int files_written;
    int symbols_written;
    int imports_written;
    /* Signaled when writer thread exits */
    _Atomic(int) *finished;
} writer_ctx_t;

struct tt_pipeline_handle
{
    pthread_t *worker_threads;
    pthread_t writer_thread;
    worker_ctx_t *worker_ctxs;
    writer_ctx_t writer_ctx;
    int num_workers;
    struct mpsc_queue queue;
    _Atomic(int64_t) pending_count;
    _Atomic(int) error_flag;
    _Atomic(int) finished;
    tt_progress_t *progress;
    const char **balanced_paths; /* [owns] reordered paths array, NULL if not balanced */
};

/* ===== Batch allocation/free ===== */

static tt_symbol_batch_t *batch_alloc(void)
{
    tt_symbol_batch_t *b = calloc(1, sizeof(tt_symbol_batch_t));
    return b;
}

static void batch_free(tt_symbol_batch_t *b)
{
    if (!b)
        return;
    if (b->symbols)
        tt_symbol_array_free(b->symbols, b->symbol_count);
    if (b->imports)
        tt_import_array_free(b->imports, b->import_count);
    for (int i = 0; i < b->file_count; i++)
    {
        free(b->file_paths[i]);
        free(b->file_hashes[i]);
        free(b->file_languages[i]);
    }
    free(b->file_paths);
    free(b->file_hashes);
    free(b->file_languages);
    free(b->file_sizes);
    free(b->file_mtimes_sec);
    free(b->file_mtimes_nsec);
    free(b->error_msg);
    free(b);
}

/* ===== Local batch accumulator for workers ===== */

typedef struct
{
    tt_symbol_t *symbols;
    int sym_count;
    int sym_cap;
    tt_import_t *imports;
    int imp_count;
    int imp_cap;
    char **file_paths;
    char **file_hashes;
    char **file_languages;
    int64_t *file_sizes;
    int64_t *file_mtimes_sec;
    int64_t *file_mtimes_nsec;
    int file_count;
    int file_cap;
} local_batch_t;

static int local_batch_init(local_batch_t *lb)
{
    memset(lb, 0, sizeof(*lb));
    lb->sym_cap = 256;
    lb->symbols = malloc((size_t)lb->sym_cap * sizeof(tt_symbol_t));
    lb->imp_cap = 64;
    lb->imports = malloc((size_t)lb->imp_cap * sizeof(tt_import_t));
    lb->file_cap = 64;
    lb->file_paths = malloc((size_t)lb->file_cap * sizeof(char *));
    lb->file_hashes = malloc((size_t)lb->file_cap * sizeof(char *));
    lb->file_languages = malloc((size_t)lb->file_cap * sizeof(char *));
    lb->file_sizes = malloc((size_t)lb->file_cap * sizeof(int64_t));
    lb->file_mtimes_sec = malloc((size_t)lb->file_cap * sizeof(int64_t));
    lb->file_mtimes_nsec = malloc((size_t)lb->file_cap * sizeof(int64_t));
    if (!lb->symbols || !lb->imports || !lb->file_paths || !lb->file_hashes ||
        !lb->file_languages || !lb->file_sizes || !lb->file_mtimes_sec ||
        !lb->file_mtimes_nsec)
    {
        free(lb->symbols);
        free(lb->imports);
        free(lb->file_paths);
        free(lb->file_hashes);
        free(lb->file_languages);
        free(lb->file_sizes);
        free(lb->file_mtimes_sec);
        free(lb->file_mtimes_nsec);
        memset(lb, 0, sizeof(*lb));
        return -1;
    }
    return 0;
}

static void local_batch_add_file(local_batch_t *lb, const char *path,
                                 const char *hash, const char *language,
                                 int64_t size, int64_t mtime_sec,
                                 int64_t mtime_nsec)
{
    if (lb->file_count >= lb->file_cap)
    {
        int new_cap = lb->file_cap * 2;
        size_t nc = (size_t)new_cap;
        char **tp = realloc(lb->file_paths, nc * sizeof(char *));
        if (!tp)
            return;
        lb->file_paths = tp;
        char **th = realloc(lb->file_hashes, nc * sizeof(char *));
        if (!th)
            return;
        lb->file_hashes = th;
        char **tl = realloc(lb->file_languages, nc * sizeof(char *));
        if (!tl)
            return;
        lb->file_languages = tl;
        int64_t *ts = realloc(lb->file_sizes, nc * sizeof(int64_t));
        if (!ts)
            return;
        lb->file_sizes = ts;
        int64_t *tm = realloc(lb->file_mtimes_sec, nc * sizeof(int64_t));
        if (!tm)
            return;
        lb->file_mtimes_sec = tm;
        int64_t *tn = realloc(lb->file_mtimes_nsec, nc * sizeof(int64_t));
        if (!tn)
            return;
        lb->file_mtimes_nsec = tn;
        lb->file_cap = new_cap;
    }
    lb->file_paths[lb->file_count] = tt_strdup(path);
    lb->file_hashes[lb->file_count] = tt_strdup(hash);
    lb->file_languages[lb->file_count] = tt_strdup(language);
    lb->file_sizes[lb->file_count] = size;
    lb->file_mtimes_sec[lb->file_count] = mtime_sec;
    lb->file_mtimes_nsec[lb->file_count] = mtime_nsec;
    lb->file_count++;
}

/* Transfer local batch to a queue-ready batch, resetting local state */
static tt_symbol_batch_t *local_batch_flush(local_batch_t *lb)
{
    tt_symbol_batch_t *b = batch_alloc();
    if (!b)
        return NULL;

    b->symbols = lb->symbols;
    b->symbol_count = lb->sym_count;
    b->imports = lb->imports;
    b->import_count = lb->imp_count;
    b->file_paths = lb->file_paths;
    b->file_hashes = lb->file_hashes;
    b->file_languages = lb->file_languages;
    b->file_sizes = lb->file_sizes;
    b->file_mtimes_sec = lb->file_mtimes_sec;
    b->file_mtimes_nsec = lb->file_mtimes_nsec;
    b->file_count = lb->file_count;

    /* Reset local batch with fresh allocations */
    memset(lb, 0, sizeof(*lb));
    if (local_batch_init(lb) < 0)
    {
        /* Cannot allocate fresh batch. The flushed batch is still valid. */
    }

    return b;
}

static void local_batch_destroy(local_batch_t *lb)
{
    if (lb->symbols)
    {
        if (lb->sym_count > 0)
            tt_symbol_array_free(lb->symbols, lb->sym_count);
        else
            free(lb->symbols);
    }
    if (lb->imports)
    {
        if (lb->imp_count > 0)
            tt_import_array_free(lb->imports, lb->imp_count);
        else
            free(lb->imports);
    }
    for (int i = 0; i < lb->file_count; i++)
    {
        free(lb->file_paths[i]);
        free(lb->file_hashes[i]);
        free(lb->file_languages[i]);
    }
    free(lb->file_paths);
    free(lb->file_hashes);
    free(lb->file_languages);
    free(lb->file_sizes);
    free(lb->file_mtimes_sec);
    free(lb->file_mtimes_nsec);
}

/* ===== Ctags entry (lightweight, for accumulation) ===== */

typedef struct
{
    char *name;
    char *file;     /* relative path */
    char *kind;     /* normalized kind string */
    char *language; /* normalized language */
    char *signature;
    char *qualified_name;
    char *parent_id;
    int line;
    int end_line;
} pipeline_entry_t;

/* pipeline_entry strings are arena-allocated; no individual free needed. */

/* ===== JSON line parsing with yyjson ===== */

static const char *yy_get_str(yyjson_val *obj, const char *key)
{
    yyjson_val *v = yyjson_obj_get(obj, key);
    return v ? yyjson_get_str(v) : NULL;
}

static int yy_get_int(yyjson_val *obj, const char *key)
{
    yyjson_val *v = yyjson_obj_get(obj, key);
    return v ? (int)yyjson_get_int(v) : 0;
}

/* Extract scope name from "kindname:ScopeName" */
static const char *extract_scope_name(const char *scope, size_t slen,
                                      size_t *name_len)
{
    for (size_t i = 0; i < slen; i++)
    {
        if (scope[i] == ':')
        {
            *name_len = slen - i - 1;
            return scope + i + 1;
        }
    }
    *name_len = slen;
    return scope;
}

/* Extract signature from pattern field (ctags pattern) */
static char *sig_from_pattern(const char *pattern)
{
    if (!pattern || !*pattern)
        return tt_strdup("");

    const char *p = pattern;
    const char *pend = pattern + strlen(pattern);

    if (p < pend && *p == '/')
        p++;
    if (p < pend && *p == '^')
        p++;
    while (pend > p && (*(pend - 1) == '/' || *(pend - 1) == '$'))
        pend--;

    size_t len = (size_t)(pend - p);
    char *sig = tt_strndup(p, len);
    if (sig)
        tt_str_trim(sig);
    return sig ? sig : tt_strdup("");
}

/*
 * parse_ctags_json_line -- Parse one ctags JSON line into a pipeline_entry_t.
 *
 * Returns 0 on success, -1 if the line should be skipped.
 */
static int parse_ctags_json_line(const char *line, size_t len,
                                 const char *project_root, size_t root_len,
                                 tt_arena_t *arena, pipeline_entry_t *out)
{
    memset(out, 0, sizeof(*out));

    yyjson_doc *doc = yyjson_read(line, len, 0);
    if (!doc)
        return -1;

    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root))
    {
        yyjson_doc_free(doc);
        return -1;
    }

    /* Must be a "tag" type */
    const char *type = yy_get_str(root, "_type");
    if (!type || strcmp(type, "tag") != 0)
    {
        yyjson_doc_free(doc);
        return -1;
    }

    const char *name = yy_get_str(root, "name");
    const char *path = yy_get_str(root, "path");
    const char *kind = yy_get_str(root, "kind");
    int line_num = yy_get_int(root, "line");

    if (!name || !name[0] || !path || !path[0] || line_num <= 0)
    {
        yyjson_doc_free(doc);
        return -1;
    }

    /* Lowercase kind */
    char kind_buf[64] = {0};
    if (kind)
    {
        size_t klen = strlen(kind);
        if (klen >= sizeof(kind_buf))
            klen = sizeof(kind_buf) - 1;
        for (size_t i = 0; i < klen; i++)
            kind_buf[i] = (char)tolower((unsigned char)kind[i]);
    }

    /* Skip noise kinds */
    if (strcmp(kind_buf, "alias") == 0 || strcmp(kind_buf, "local") == 0)
    {
        yyjson_doc_free(doc);
        return -1;
    }

    /* Skip JS/TS operators */
    size_t name_len = strlen(name);
    if ((name_len == 10 && memcmp(name, "instanceof", 10) == 0) ||
        (name_len == 6 && memcmp(name, "typeof", 6) == 0) ||
        (name_len == 8 && memcmp(name, "debugger", 8) == 0))
    {
        yyjson_doc_free(doc);
        return -1;
    }

    /* Optional fields */
    const char *lang = yy_get_str(root, "language");
    const char *sig = yy_get_str(root, "signature");
    const char *scope = yy_get_str(root, "scope");
    const char *scope_kind = yy_get_str(root, "scopeKind");
    const char *typeref = yy_get_str(root, "typeref");
    const char *pattern = yy_get_str(root, "pattern");
    int end_line = yy_get_int(root, "end");

    /* Make path relative */
    char *rel_path;
    size_t path_len = strlen(path);
    if (path_len > root_len && path[root_len] == '/' &&
        memcmp(path, project_root, root_len) == 0)
    {
        rel_path = tt_strdup(path + root_len + 1);
    }
    else
    {
        rel_path = tt_strdup(path);
    }
    if (rel_path)
        tt_path_normalize_sep(rel_path);

    /* Normalize kind */
    tt_symbol_kind_e kind_enum = tt_kind_from_ctags(kind_buf);
    const char *norm_kind = tt_kind_to_str(kind_enum);

    /* Normalize language */
    const char *norm_lang = tt_normalize_language(lang ? lang : "");

    /* Build qualified name */
    char *qualified_name;
    if (scope && scope[0])
    {
        size_t sname_len;
        size_t slen = strlen(scope);
        const char *sname = extract_scope_name(scope, slen, &sname_len);
        size_t qlen = sname_len + 1 + name_len + 1;
        qualified_name = malloc(qlen);
        if (qualified_name)
            snprintf(qualified_name, qlen, "%.*s.%.*s",
                     (int)sname_len, sname, (int)name_len, name);
    }
    else
    {
        qualified_name = tt_strdup(name);
    }

    /* Build parent_id */
    char *parent_id = NULL;
    if (scope && scope[0] && scope_kind && scope_kind[0])
    {
        char sk_buf[64];
        size_t sk_len = strlen(scope_kind);
        if (sk_len >= sizeof(sk_buf))
            sk_len = sizeof(sk_buf) - 1;
        for (size_t i = 0; i < sk_len; i++)
            sk_buf[i] = (char)tolower((unsigned char)scope_kind[i]);
        sk_buf[sk_len] = '\0';

        tt_symbol_kind_e sk_enum = tt_kind_from_ctags(sk_buf);
        const char *sk_str = tt_kind_to_str(sk_enum);

        size_t sname_len;
        size_t slen = strlen(scope);
        const char *sname = extract_scope_name(scope, slen, &sname_len);
        char *norm_scope = tt_arena_strndup(arena, sname, sname_len);

        if (norm_scope && strcmp(sk_str, "namespace") != 0)
        {
            char *last_bs = strrchr(norm_scope, '\\');
            if (last_bs)
                *last_bs = '.';
        }

        parent_id = tt_symbol_make_id(rel_path,
                                      norm_scope ? norm_scope : "",
                                      sk_str, 0);
    }

    /* Build signature (heap-allocated intermediate) */
    char *signature;
    if ((!sig || !sig[0]) && pattern && pattern[0])
    {
        signature = sig_from_pattern(pattern);
    }
    else if (sig && sig[0])
    {
        signature = tt_strdup(sig);
    }
    else
    {
        signature = tt_strdup("");
    }

    /* Append return type from typeref */
    if (typeref && typeref[0] && signature && !strstr(signature, ":"))
    {
        const char *colon = strchr(typeref, ':');
        const char *ret_type = colon ? colon + 1 : typeref;
        size_t ret_len = strlen(ret_type);
        if (ret_len > 0 && signature[0] != '\0')
        {
            size_t slen = strlen(signature);
            size_t new_len = slen + 2 + ret_len + 1;
            char *new_sig = malloc(new_len);
            if (new_sig)
            {
                memcpy(new_sig, signature, slen);
                new_sig[slen] = ':';
                new_sig[slen + 1] = ' ';
                memcpy(new_sig + slen + 2, ret_type, ret_len);
                new_sig[slen + 2 + ret_len] = '\0';
                free(signature);
                signature = new_sig;
            }
        }
    }

    /* Arena-allocate final strings for pipeline_entry_t.
     * Heap intermediates (rel_path, qualified_name, signature, parent_id)
     * are copied into the arena and freed. */
    out->name = tt_arena_strdup(arena, name);
    out->file = tt_arena_strdup(arena, rel_path);
    out->kind = tt_arena_strdup(arena, norm_kind);
    out->language = tt_arena_strdup(arena, norm_lang);
    out->signature = tt_arena_strdup(arena, signature ? signature : "");
    out->qualified_name = tt_arena_strdup(arena, qualified_name ? qualified_name : "");
    out->parent_id = parent_id ? tt_arena_strdup(arena, parent_id) : NULL;
    out->line = line_num;
    out->end_line = end_line;

    free(rel_path);
    free(qualified_name);
    free(signature);
    free(parent_id);

    yyjson_doc_free(doc);
    return 0;
}

/* ===== Dedup entries by file:line:kind ===== */

static void dedup_entries(pipeline_entry_t *entries, int *count)
{
    int n = *count;
    if (n <= 0)
        return;

    tt_hashmap_t *seen = tt_hashmap_new(n > 0 ? (size_t)n : 16);

    for (int i = 0; i < n; i++)
    {
        pipeline_entry_t *e = &entries[i];
        char key_buf[512];
        snprintf(key_buf, sizeof(key_buf), "%s:%d:%s", e->file, e->line, e->kind);

        /* Use set to check existence: returns old value (non-NULL) if key existed */
        void *old = tt_hashmap_set(seen, key_buf, (void *)(intptr_t)(i + 1));
        if (old)
        {
            int prev_i = (int)((intptr_t)old - 1);
            if (strlen(e->qualified_name) > strlen(entries[prev_i].qualified_name))
            {
                /* Current entry wins: prev is already replaced in hashmap */
                entries[prev_i].file = NULL;
            }
            else
            {
                /* Previous entry wins: restore it in hashmap */
                tt_hashmap_set(seen, key_buf, old);
                e->file = NULL;
            }
        }
    }

    tt_hashmap_free(seen);

    /* Compact: move surviving entries to front, zero moved slots to
     * prevent double-free when caller frees the original array. */
    int final = 0;
    for (int i = 0; i < n; i++)
    {
        if (entries[i].file)
        {
            if (final != i)
            {
                entries[final] = entries[i];
                memset(&entries[i], 0, sizeof(entries[i]));
            }
            final++;
        }
    }
    *count = final;
}

/* ===== Sort entries by line ===== */

static int cmp_entry_line(const void *a, const void *b)
{
    const pipeline_entry_t *ea = (const pipeline_entry_t *)a;
    const pipeline_entry_t *eb = (const pipeline_entry_t *)b;
    return (ea->line > eb->line) - (ea->line < eb->line);
}

/* ===== Split lines in-place ===== */

static const char **split_lines_inplace(char *content, size_t len, int *line_count)
{
    int count = 1;
    for (size_t i = 0; i < len; i++)
    {
        if (content[i] == '\n')
            count++;
    }

    const char **lines = malloc((size_t)(count + 1) * sizeof(char *));
    if (!lines)
        return NULL;

    int idx = 0;
    lines[0] = content;
    for (size_t i = 0; i < len; i++)
    {
        if (content[i] == '\n')
        {
            content[i] = '\0';
            if (idx + 1 < count)
                lines[++idx] = &content[i + 1];
        }
    }
    lines[count] = NULL;

    *line_count = count;
    return lines;
}

/* ===== Disambiguate overloads ===== */

static void disambiguate_overloads(tt_symbol_t *symbols, int count)
{
    if (count <= 0)
        return;

    tt_hashmap_t *id_counts = tt_hashmap_new((size_t)count);

    for (int i = 0; i < count; i++)
    {
        tt_symbol_t *sym = &symbols[i];

        if (!tt_hashmap_has(id_counts, sym->id))
        {
            tt_hashmap_set(id_counts, sym->id, (void *)(intptr_t)0);
        }
        else
        {
            intptr_t cnt = (intptr_t)tt_hashmap_get(id_counts, sym->id);
            cnt++;
            tt_hashmap_set(id_counts, sym->id, (void *)cnt);

            tt_strbuf_t sb;
            tt_strbuf_init(&sb);
            tt_strbuf_appendf(&sb, "%s~%d", sym->id, (int)cnt);
            free(sym->id);
            sym->id = tt_strbuf_detach(&sb);
        }
    }

    tt_hashmap_free(id_counts);
}

/* ===== Build symbols for a file group ===== */

static int build_symbols_for_group(const char *project_root,
                                   const char *rel_path,
                                   pipeline_entry_t *entries, int entry_count,
                                   local_batch_t *lb,
                                   int max_file_size_bytes)
{
    char *full_path = tt_path_join(project_root, rel_path);
    if (!full_path)
        return -1;

    /* stat() the file for size and mtime */
    struct stat st;
    if (stat(full_path, &st) != 0)
    {
        free(full_path);
        return -1;
    }

    /* Read file */
    size_t content_len = 0;
    char *content = tt_read_file(full_path, &content_len);
    free(full_path);
    if (!content)
        return 0;

    /* Compute content hash and detect language early so we can always
     * insert a file record -- even for binary or oversize files. */
    char *file_hash = tt_fast_hash_hex(content, content_len);
    if (!file_hash)
        file_hash = tt_strdup("");

    const char *lang = tt_detect_language(rel_path);

    /* Always insert file record so change detection won't perpetually
     * re-discover this file as "added" on every index:update. */
    local_batch_add_file(lb, rel_path, file_hash,
                         lang ? lang : "",
                         (int64_t)st.st_size,
                         TT_ST_MTIME_SEC(st),
                         TT_ST_MTIME_NSEC(st));

    /* Binary content check: null bytes in first 8KB -- skip symbols */
    {
        size_t check_len = content_len < 8192 ? content_len : 8192;
        for (size_t i = 0; i < check_len; i++)
        {
            if (content[i] == '\0')
            {
                free(content);
                free(file_hash);
                return 0; /* binary file, skip symbol extraction */
            }
        }
    }

    /* Size check: skip symbol extraction for oversized files */
    if (max_file_size_bytes > 0 && st.st_size > max_file_size_bytes)
    {
        free(content);
        free(file_hash);
        return 0;
    }

    /* Dedup entries (compacts, zeros moved slots) */
    dedup_entries(entries, &entry_count);

    if (entry_count <= 0)
    {
        free(content);
        free(file_hash);
        return 0;
    }

    /* Sort entries by line */
    qsort(entries, (size_t)entry_count, sizeof(pipeline_entry_t), cmp_entry_line);

    /* Build line offsets and split lines */
    tt_line_offsets_t lo;
    tt_line_offsets_build(&lo, content, content_len);

    int total_lines = 0;
    const char **lines = split_lines_inplace(content, content_len, &total_lines);
    if (!lines)
    {
        free(content);
        free(file_hash);
        tt_line_offsets_free(&lo);
        return -1;
    }

    /* Extract imports */
    tt_extract_imports_from_lines(rel_path, lang, lines, total_lines,
                                  &lb->imports, &lb->imp_count, &lb->imp_cap);

    /* Build symbols */
    int start_sym = lb->sym_count;

    for (int i = 0; i < entry_count; i++)
    {
        pipeline_entry_t *e = &entries[i];
        int line_num = e->line;
        int end_line = e->end_line;

        tt_symbol_kind_e kind_enum = tt_kind_from_ctags(e->kind);

        if (end_line <= 0 || end_line < line_num)
        {
            end_line = tt_estimate_end_line(lines, total_lines, line_num, kind_enum);
        }
        else if (!tt_kind_is_single_line(kind_enum) && end_line - line_num < 3)
        {
            int estimated = tt_estimate_end_line(lines, total_lines, line_num, kind_enum);
            if (estimated > end_line)
                end_line = estimated;
        }
        if (end_line > total_lines)
            end_line = total_lines;

        int byte_offset = tt_line_offsets_offset_at(&lo, line_num);
        int end_offset;
        if (end_line < total_lines)
        {
            end_offset = tt_line_offsets_offset_at(&lo, end_line + 1);
            if (end_offset == 0 && end_line + 1 > lo.count)
                end_offset = (int)content_len;
        }
        else
        {
            end_offset = (int)content_len;
        }
        int byte_length = end_offset - byte_offset;
        if (byte_length < 0)
            byte_length = 0;

        char *content_hash = NULL;
        if (byte_length > 0 && byte_offset + byte_length <= (int)content_len)
        {
            content_hash = tt_fast_hash_hex(content + byte_offset, (size_t)byte_length);
        }
        if (!content_hash)
            content_hash = tt_strdup("");

        char *docstring = tt_extract_docstring(lines, total_lines, line_num);
        char *keywords = tt_extract_keywords(e->qualified_name);
        char *id = tt_symbol_make_id(e->file, e->qualified_name,
                                     tt_kind_to_str(kind_enum), 0);

        if (lb->sym_count >= lb->sym_cap)
        {
            lb->sym_cap *= 2;
            tt_symbol_t *tmp = realloc(lb->symbols, (size_t)lb->sym_cap * sizeof(tt_symbol_t));
            if (!tmp)
            {
                free(content_hash);
                free(docstring);
                free(keywords);
                free(id);
                continue;
            }
            lb->symbols = tmp;
        }

        tt_symbol_t *sym = &lb->symbols[lb->sym_count];
        sym->id = id;
        sym->file = tt_strdup(e->file);
        sym->name = tt_strdup(e->name);
        sym->qualified_name = tt_strdup(e->qualified_name);
        sym->kind = kind_enum;
        sym->language = tt_strdup(e->language);
        sym->signature = tt_strdup(e->signature);
        sym->docstring = docstring;
        sym->summary = tt_strdup("");
        sym->decorators = tt_strdup("[]");
        sym->keywords = keywords;
        sym->parent_id = e->parent_id ? tt_strdup(e->parent_id) : NULL;
        sym->line = line_num;
        sym->end_line = end_line;
        sym->byte_offset = byte_offset;
        sym->byte_length = byte_length;
        sym->content_hash = content_hash;
        lb->sym_count++;
    }

    /* Disambiguate overloads for this file group's symbols */
    int new_sym_count = lb->sym_count - start_sym;
    if (new_sym_count > 0)
        disambiguate_overloads(&lb->symbols[start_sym], new_sym_count);

    free(lines);
    free(content);
    free(file_hash);
    tt_line_offsets_free(&lo);
    return 0;
}

/* ===== JS/TS const extraction ===== */

static bool is_js_ts_file(const char *rel_path)
{
    const char *dot = strrchr(rel_path, '.');
    if (!dot) return false;
    return (strcasecmp(dot, ".js") == 0 || strcasecmp(dot, ".jsx") == 0 ||
            strcasecmp(dot, ".ts") == 0 || strcasecmp(dot, ".tsx") == 0 ||
            strcasecmp(dot, ".mjs") == 0 || strcasecmp(dot, ".cjs") == 0 ||
            strcasecmp(dot, ".mts") == 0);
}

/*
 * Extract `const NAME = <non-function-value>` as constant symbols.
 * Skips destructuring, arrow functions, function expressions,
 * and names already extracted by ctags.
 */
static void extract_js_constants(const char *project_root,
                                 const char *rel_path, local_batch_t *lb)
{
    char *full = tt_path_join(project_root, rel_path);
    if (!full) return;

    char *content = tt_read_file(full, NULL);
    free(full);
    if (!content) return;

    /* Build set of existing symbol names for dedup */
    tt_hashmap_t *existing = tt_hashmap_new(
        lb->sym_count > 4 ? (size_t)lb->sym_count * 2 : 16);
    for (int i = 0; i < lb->sym_count; i++) {
        if (lb->symbols[i].name && lb->symbols[i].file &&
            strcmp(lb->symbols[i].file, rel_path) == 0)
            tt_hashmap_set(existing, lb->symbols[i].name, (void *)1);
    }

    /* Determine language string for symbols */
    const char *dot = strrchr(rel_path, '.');
    const char *lang = "javascript";
    if (dot && (strcasecmp(dot, ".ts") == 0 || strcasecmp(dot, ".tsx") == 0 ||
                strcasecmp(dot, ".mts") == 0))
        lang = "typescript";

    tt_symbol_t *extras = NULL;
    int ex_count = 0, ex_cap = 0;

    const char *p = content;
    int line_num = 1;

    while (*p) {
        /* Find start of line, skip whitespace */
        while (*p == ' ' || *p == '\t') p++;

        /* Check for optional `export ` prefix */
        const char *after_export = p;
        if (strncmp(p, "export ", 7) == 0)
            after_export = p + 7;

        /* Match `const ` keyword */
        if (strncmp(after_export, "const ", 6) != 0)
            goto next_line;

        const char *name_start = after_export + 6;
        while (*name_start == ' ' || *name_start == '\t') name_start++;

        /* Skip destructuring: { or [ after const */
        if (*name_start == '{' || *name_start == '[')
            goto next_line;

        /* Read identifier */
        const char *name_end = name_start;
        while (*name_end && (isalnum((unsigned char)*name_end) || *name_end == '_' || *name_end == '$'))
            name_end++;

        size_t name_len = (size_t)(name_end - name_start);
        if (name_len == 0 || name_len > 128)
            goto next_line;

        /* Skip to = sign */
        const char *eq = name_end;
        while (*eq == ' ' || *eq == '\t') eq++;
        if (*eq != '=')
            goto next_line;
        eq++;
        while (*eq == ' ' || *eq == '\t') eq++;

        /* Skip function expressions: function, async, arrow functions */
        if (strncmp(eq, "function", 8) == 0 && !isalnum((unsigned char)eq[8]))
            goto next_line;
        if (strncmp(eq, "async", 5) == 0 && !isalnum((unsigned char)eq[5]))
            goto next_line;
        if (*eq == '(') {
            /* Value starts with ( — could be multi-line arrow function.
             * Scan forward across lines (up to ~500 chars) for =>. */
            const char *scan = eq;
            int depth = 0;
            int chars = 0;
            while (*scan && chars < 500) {
                if (*scan == '(') depth++;
                else if (*scan == ')') {
                    depth--;
                    if (depth == 0) {
                        /* After closing ), skip whitespace and check for => */
                        scan++;
                        while (*scan == ' ' || *scan == '\t' || *scan == '\n' || *scan == '\r')
                            scan++;
                        if (scan[0] == '=' && scan[1] == '>')
                            goto next_line;
                        break;
                    }
                }
                scan++;
                chars++;
            }
        }
        /* Check if current line contains => anywhere (inline arrow) */
        {
            const char *scan = eq;
            while (*scan && *scan != '\n') {
                if (scan[0] == '=' && scan[1] == '>') goto next_line;
                scan++;
            }
        }

        /* Extract name */
        {
            char name[130];
            memcpy(name, name_start, name_len);
            name[name_len] = '\0';

            /* Skip if ctags already found it */
            if (tt_hashmap_has(existing, name))
                goto next_line;

            /* Build symbol */
            if (ex_count >= ex_cap) {
                ex_cap = ex_cap ? ex_cap * 2 : 16;
                tt_symbol_t *tmp = realloc(extras, (size_t)ex_cap * sizeof(tt_symbol_t));
                if (!tmp) goto done;
                extras = tmp;
            }

            tt_symbol_t *sym = &extras[ex_count];
            memset(sym, 0, sizeof(*sym));
            sym->file = tt_strdup(rel_path);
            sym->name = tt_strdup(name);
            sym->qualified_name = tt_strdup(name);
            sym->kind = TT_KIND_CONSTANT;
            sym->language = tt_strdup(lang);
            sym->line = line_num;
            sym->end_line = line_num;
            sym->signature = tt_strdup("");
            sym->docstring = tt_strdup("");
            sym->summary = tt_strdup("");
            sym->decorators = tt_strdup("[]");
            sym->keywords = tt_extract_keywords(name);
            sym->parent_id = NULL;
            sym->content_hash = tt_strdup("");
            sym->id = tt_symbol_make_id(rel_path, name, "constant", 0);

            /* Mark as seen to avoid duplicates within the same file */
            tt_hashmap_set(existing, sym->name, (void *)1);
            ex_count++;
        }

next_line:
        /* Advance to next line */
        while (*p && *p != '\n') p++;
        if (*p == '\n') { p++; line_num++; }
    }

done:
    tt_hashmap_free(existing);
    free(content);

    if (ex_count > 0 && extras) {
        int needed = lb->sym_count + ex_count;
        if (needed > lb->sym_cap) {
            lb->sym_cap = needed * 2;
            tt_symbol_t *tmp = realloc(lb->symbols,
                                       (size_t)lb->sym_cap * sizeof(tt_symbol_t));
            if (tmp) lb->symbols = tmp;
        }
        if (lb->sym_count + ex_count <= lb->sym_cap) {
            memcpy(&lb->symbols[lb->sym_count], extras,
                   (size_t)ex_count * sizeof(tt_symbol_t));
            lb->sym_count += ex_count;
            free(extras);
        } else {
            tt_symbol_array_free(extras, ex_count);
        }
    } else if (extras) {
        free(extras);
    }
}

/* ===== Custom parser integration ===== */

static void merge_custom_parser(const char *project_root,
                                const char *rel_path, local_batch_t *lb)
{
    size_t flen = strlen(rel_path);
    const char *paths[1] = {rel_path};

    tt_symbol_t *extra = NULL;
    int extra_count = 0;
    int rc = -1;

    if (flen > 10 && strcasecmp(rel_path + flen - 10, ".blade.php") == 0)
        rc = tt_parse_blade(project_root, paths, 1, &extra, &extra_count);
    else if (flen > 4 && strcasecmp(rel_path + flen - 4, ".vue") == 0)
        rc = tt_parse_vue(project_root, paths, 1, &extra, &extra_count);
    else if (flen > 4 && strcasecmp(rel_path + flen - 4, ".ejs") == 0)
        rc = tt_parse_ejs(project_root, paths, 1, &extra, &extra_count);
    else if (flen > 4 && strcasecmp(rel_path + flen - 4, ".nix") == 0)
        rc = tt_parse_nix(project_root, paths, 1, &extra, &extra_count);
    else if (flen > 6 && strcasecmp(rel_path + flen - 6, ".gleam") == 0)
        rc = tt_parse_gleam(project_root, paths, 1, &extra, &extra_count);
    else if ((flen > 3 && strcasecmp(rel_path + flen - 3, ".tf") == 0) ||
             (flen > 4 && strcasecmp(rel_path + flen - 4, ".hcl") == 0) ||
             (flen > 7 && strcasecmp(rel_path + flen - 7, ".tfvars") == 0))
        rc = tt_parse_hcl(project_root, paths, 1, &extra, &extra_count);
    else if ((flen > 8 && strcasecmp(rel_path + flen - 8, ".graphql") == 0) ||
             (flen > 4 && strcasecmp(rel_path + flen - 4, ".gql") == 0))
        rc = tt_parse_graphql(project_root, paths, 1, &extra, &extra_count);
    else if (flen > 3 && strcasecmp(rel_path + flen - 3, ".jl") == 0)
        rc = tt_parse_julia(project_root, paths, 1, &extra, &extra_count);
    else if (flen > 3 && strcasecmp(rel_path + flen - 3, ".gd") == 0)
        rc = tt_parse_gdscript(project_root, paths, 1, &extra, &extra_count);
    else if (flen > 6 && strcasecmp(rel_path + flen - 6, ".verse") == 0)
        rc = tt_parse_verse(project_root, paths, 1, &extra, &extra_count);
    else if ((flen > 4 && strcasecmp(rel_path + flen - 4, ".xml") == 0) ||
             (flen > 4 && strcasecmp(rel_path + flen - 4, ".xul") == 0))
        rc = tt_parse_xml(project_root, paths, 1, &extra, &extra_count);
    else if (flen > 4 && strcasecmp(rel_path + flen - 4, ".ahk") == 0)
        rc = tt_parse_autohotkey(project_root, paths, 1, &extra, &extra_count);
    else if ((flen > 4 && strcasecmp(rel_path + flen - 4, ".asm") == 0) ||
             (flen > 2 && strcasecmp(rel_path + flen - 2, ".s") == 0))
        rc = tt_parse_asm(project_root, paths, 1, &extra, &extra_count);

    /* OpenAPI check is separate (uses content detection) */
    if (rc != 0 && tt_is_openapi_file(rel_path))
        rc = tt_parse_openapi(project_root, paths, 1, &extra, &extra_count);

    if (rc == 0 && extra_count > 0 && extra)
    {
        int needed = lb->sym_count + extra_count;
        if (needed > lb->sym_cap)
        {
            lb->sym_cap = needed * 2;
            tt_symbol_t *tmp = realloc(lb->symbols,
                                       (size_t)lb->sym_cap * sizeof(tt_symbol_t));
            if (tmp)
                lb->symbols = tmp;
        }
        if (lb->sym_count + extra_count <= lb->sym_cap)
        {
            memcpy(&lb->symbols[lb->sym_count], extra,
                   (size_t)extra_count * sizeof(tt_symbol_t));
            lb->sym_count += extra_count;
            free(extra);
        }
        else
        {
            tt_symbol_array_free(extra, extra_count);
        }
    }
    else if (extra)
    {
        if (extra_count > 0)
            tt_symbol_array_free(extra, extra_count);
        else
            free(extra);
    }

    /* JS/TS const extraction (runs after ctags + custom parsers) */
    if (is_js_ts_file(rel_path))
        extract_js_constants(project_root, rel_path, lb);
}

/* ===== Enqueue a batch ===== */

static bool enqueue_batch(tt_symbol_batch_t *batch, worker_ctx_t *ctx)
{
    mpsc_queue_insert(ctx->queue, &batch->node);
    atomic_fetch_add(ctx->pending_count, 1);

    if (atomic_load(ctx->pending_count) > BACKPRESSURE_THRESHOLD)
    {
        TT_DIAG("worker", "backpressure",
                "\"wid\":%d,\"pending\":%lld",
                ctx->worker_id,
                (long long)atomic_load(ctx->pending_count));
        usleep(1000); /* 1ms backpressure */
        return true;
    }
    return false;
}

/* ===== Worker thread function ===== */

static void *worker_fn(void *arg)
{
    worker_ctx_t *ctx = (worker_ctx_t *)arg;
    local_batch_t lb;
    if (local_batch_init(&lb) < 0)
    {
        atomic_store(ctx->error_flag, 1);
        return NULL;
    }

    /* 1. Write chunk paths to temp file */
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/tt_chunk_%d_%d_XXXXXX",
             (int)getpid(), ctx->worker_id);
    int fd = mkstemp(tmp_path);
    if (fd < 0)
    {
        /* Error: push sentinel with error */
        tt_symbol_batch_t *sentinel = batch_alloc();
        if (sentinel)
        {
            sentinel->is_sentinel = true;
            sentinel->has_error = true;
            sentinel->error_msg = tt_strdup("failed to create temp chunk file");
            enqueue_batch(sentinel, ctx);
        }
        local_batch_destroy(&lb);
        return NULL;
    }

    tt_strbuf_t file_list;
    tt_strbuf_init(&file_list);
    for (int i = 0; i < ctx->file_count; i++)
    {
        if (i > 0)
            tt_strbuf_append_char(&file_list, '\n');
        tt_strbuf_append_str(&file_list, ctx->project_root);
        tt_strbuf_append_char(&file_list, '/');
        tt_strbuf_append_str(&file_list, ctx->file_paths[i]);
    }
    if (write(fd, file_list.data, file_list.len) < 0)
    {
        close(fd);
        tt_strbuf_free(&file_list);
        unlink(tmp_path);
        tt_symbol_batch_t *sentinel = batch_alloc();
        if (sentinel)
        {
            sentinel->is_sentinel = true;
            sentinel->has_error = true;
            sentinel->error_msg = tt_strdup("failed to write file list to temp file");
            enqueue_batch(sentinel, ctx);
        }
        local_batch_destroy(&lb);
        return NULL;
    }
    close(fd);
    tt_strbuf_free(&file_list);

    /* 2. Start ctags streaming */
    tt_ctags_stream_t stream;
    int rc = tt_ctags_stream_start(&stream, ctx->ctags_path, tmp_path,
                                   ctx->ctags_timeout_sec);
    if (rc < 0)
    {
        unlink(tmp_path);
        tt_symbol_batch_t *sentinel = batch_alloc();
        if (sentinel)
        {
            sentinel->is_sentinel = true;
            sentinel->has_error = true;
            sentinel->error_msg = tt_strdup("failed to start ctags stream");
            enqueue_batch(sentinel, ctx);
        }
        local_batch_destroy(&lb);
        return NULL;
    }

    TT_DIAG("worker", "ctags_start",
            "\"wid\":%d,\"files\":%d", ctx->worker_id, ctx->file_count);

    /* Diagnostic counters for periodic reporting */
    int diag_files_done = 0;
    int diag_syms_total = 0;
    int diag_bp_count = 0;
    int diag_batches = 0;
    uint64_t diag_last_report = tt_monotonic_ms();
    uint64_t diag_file_start = 0;

    /* 3. Read and parse ctags lines, group by file */
    size_t root_len = strlen(ctx->project_root);

    /* Track which files produce ctags output so we can insert
     * zero-symbol files (e.g. __init__.py, header-only) into the DB too. */
    bool *file_seen = calloc((size_t)ctx->file_count, sizeof(bool));
    tt_hashmap_t *path_to_idx = tt_hashmap_new(
        ctx->file_count > 0 ? (size_t)ctx->file_count * 2 : 64);
    for (int i = 0; i < ctx->file_count; i++)
        tt_hashmap_set(path_to_idx, ctx->file_paths[i],
                       (void *)(intptr_t)(i + 1)); /* +1 so 0 means absent */

    /* Current file group accumulator.
     * Entry strings are arena-allocated per file group for batch deallocation. */
    pipeline_entry_t *group_entries = NULL;
    int group_count = 0;
    int group_cap = 0;
    char *current_file = NULL;
    tt_arena_t *group_arena = tt_arena_new(0);

    size_t line_len;
    const char *line;
    while ((line = tt_ctags_stream_readline(&stream, &line_len)) != NULL)
    {
        if (tt_interrupted || atomic_load(ctx->error_flag))
            break;

        pipeline_entry_t entry;
        if (parse_ctags_json_line(line, line_len, ctx->project_root,
                                  root_len, group_arena, &entry) < 0)
            continue;

        /* Check if we switched to a new file */
        if (current_file && strcmp(entry.file, current_file) != 0)
        {
            /* Flush previous file group */
            if (group_count > 0)
            {
                /* Mark file as seen (produced ctags output) */
                intptr_t idx_val = (intptr_t)tt_hashmap_get(path_to_idx, current_file);
                if (idx_val > 0 && file_seen)
                    file_seen[idx_val - 1] = true;

                build_symbols_for_group(ctx->project_root, current_file,
                                        group_entries, group_count,
                                        &lb, ctx->max_file_size_bytes);
                merge_custom_parser(ctx->project_root, current_file, &lb);

                if (ctx->progress)
                {
                    atomic_fetch_add(&ctx->progress->files_indexed, 1);
                    atomic_fetch_add(&ctx->progress->symbols_indexed,
                                     lb.sym_count);
                }

                /* Diagnostic: track file completion and detect slow files */
                diag_files_done++;
                diag_syms_total += group_count;
                if (tt_diag_enabled())
                {
                    uint64_t diag_file_elapsed = tt_monotonic_ms() - diag_file_start;
                    /* Slow file alert: > 500ms for a single file */
                    if (diag_file_elapsed > 500)
                    {
                        tt_diag_event("worker", "slow_file",
                                      "\"wid\":%d,\"file\":\"%s\","
                                      "\"ms\":%llu,\"syms\":%d",
                                      ctx->worker_id, current_file,
                                      (unsigned long long)diag_file_elapsed,
                                      group_count);
                    }
                    /* Periodic progress: every 100 files or every 5s */
                    uint64_t now = tt_monotonic_ms();
                    if (diag_files_done % 100 == 0 ||
                        (now - diag_last_report) >= 5000)
                    {
                        tt_diag_event("worker", "progress",
                                      "\"wid\":%d,\"files\":%d,"
                                      "\"total\":%d,\"syms\":%d,"
                                      "\"bp\":%d,\"batches\":%d",
                                      ctx->worker_id, diag_files_done,
                                      ctx->file_count, diag_syms_total,
                                      diag_bp_count, diag_batches);
                        diag_last_report = now;
                    }
                }
            }

            /* Create new arena and migrate the current entry's strings
             * (they live in group_arena which we're about to free). */
            tt_arena_t *new_arena = tt_arena_new(0);
            entry.name = entry.name ? tt_arena_strdup(new_arena, entry.name) : NULL;
            entry.file = entry.file ? tt_arena_strdup(new_arena, entry.file) : NULL;
            entry.kind = entry.kind ? tt_arena_strdup(new_arena, entry.kind) : NULL;
            entry.language = entry.language ? tt_arena_strdup(new_arena, entry.language) : NULL;
            entry.signature = entry.signature ? tt_arena_strdup(new_arena, entry.signature) : NULL;
            entry.qualified_name = entry.qualified_name ? tt_arena_strdup(new_arena, entry.qualified_name) : NULL;
            entry.parent_id = entry.parent_id ? tt_arena_strdup(new_arena, entry.parent_id) : NULL;

            /* Free old group arena and switch to new one */
            tt_arena_free(group_arena);
            group_arena = new_arena;
            group_count = 0;
            free(current_file);
            current_file = NULL;
            diag_file_start = tt_monotonic_ms();

            /* Check if we should flush the batch */
            if (lb.sym_count >= BATCH_SYMBOL_THRESHOLD)
            {
                tt_symbol_batch_t *batch = local_batch_flush(&lb);
                if (batch)
                {
                    if (enqueue_batch(batch, ctx))
                        diag_bp_count++;
                    diag_batches++;
                }
            }
        }

        /* Set current file (heap-allocated, outlives arena) */
        if (!current_file)
        {
            current_file = tt_strdup(entry.file);
            diag_file_start = tt_monotonic_ms();
        }

        /* Add entry to group */
        if (group_count >= group_cap)
        {
            group_cap = group_cap == 0 ? 64 : group_cap * 2;
            pipeline_entry_t *tmp = realloc(group_entries,
                                            (size_t)group_cap * sizeof(pipeline_entry_t));
            if (!tmp)
                continue;
            group_entries = tmp;
        }
        group_entries[group_count++] = entry;
    }

    /* Flush last file group */
    if (group_count > 0 && current_file)
    {
        /* Mark file as seen (produced ctags output) */
        intptr_t idx_val = (intptr_t)tt_hashmap_get(path_to_idx, current_file);
        if (idx_val > 0 && file_seen)
            file_seen[idx_val - 1] = true;

        build_symbols_for_group(ctx->project_root, current_file,
                                group_entries, group_count,
                                &lb, ctx->max_file_size_bytes);
        merge_custom_parser(ctx->project_root, current_file, &lb);

        if (ctx->progress)
        {
            atomic_fetch_add(&ctx->progress->files_indexed, 1);
        }
    }

    /* Free last group arena and entry array */
    tt_arena_free(group_arena);
    free(group_entries);
    free(current_file);

    /* Insert file records for files that produced no ctags output.
     * Without this, files like __init__.py, .blade.php templates, or
     * header-only files would never appear in the DB, causing index:update
     * to perpetually re-discover and re-index them as "added". */
    if (file_seen && !tt_interrupted && !atomic_load(ctx->error_flag))
    {
        for (int i = 0; i < ctx->file_count; i++)
        {
            if (file_seen[i])
                continue;

            const char *rel = ctx->file_paths[i];
            char *full = tt_path_join(ctx->project_root, rel);
            if (!full)
                continue;

            struct stat st;
            if (stat(full, &st) != 0)
            {
                free(full);
                continue;
            }

            /* Read file to compute hash (and verify it's not binary) */
            size_t content_len = 0;
            char *content = tt_read_file(full, &content_len);
            free(full);
            if (!content)
                continue;

            /* Binary check: null bytes in first 8KB */
            bool is_binary = false;
            {
                size_t check_len = content_len < 8192 ? content_len : 8192;
                for (size_t j = 0; j < check_len; j++)
                {
                    if (content[j] == '\0')
                    {
                        is_binary = true;
                        break;
                    }
                }
            }
            if (is_binary)
            {
                free(content);
                continue;
            }

            char *file_hash = tt_fast_hash_hex(content, content_len);
            if (!file_hash)
                file_hash = tt_strdup("");

            const char *lang = tt_detect_language(rel);

            /* Extract imports even for zero-symbol files (skip for oversize) */
            bool oversize = (ctx->max_file_size_bytes > 0 &&
                             st.st_size > ctx->max_file_size_bytes);
            if (!oversize)
            {
                int total_lines = 0;
                const char **lines = split_lines_inplace(content, content_len,
                                                         &total_lines);
                if (lines && lang)
                {
                    tt_extract_imports_from_lines(rel, lang, lines, total_lines,
                                                  &lb.imports, &lb.imp_count,
                                                  &lb.imp_cap);
                }
                free(lines);
            }

            /* Run custom parsers for files ctags doesn't know about
             * (Vue, Gleam, GDScript, HCL, Nix, GraphQL, Julia, etc.) */
            merge_custom_parser(ctx->project_root, rel, &lb);

            /* Always insert file record so change detection won't
             * perpetually re-discover it as "added". */
            local_batch_add_file(&lb, rel, file_hash,
                                 lang ? lang : "",
                                 (int64_t)st.st_size,
                                 TT_ST_MTIME_SEC(st),
                                 TT_ST_MTIME_NSEC(st));
            free(content);
            free(file_hash);

            if (ctx->progress)
                atomic_fetch_add(&ctx->progress->files_indexed, 1);
        }
    }

    free(file_seen);
    tt_hashmap_free(path_to_idx);

    /* Flush remaining symbols */
    if (lb.sym_count > 0 || lb.file_count > 0)
    {
        tt_symbol_batch_t *batch = local_batch_flush(&lb);
        if (batch)
        {
            if (enqueue_batch(batch, ctx))
                diag_bp_count++;
        }
    }

    /* Finish ctags stream */
    char *stderr_out = NULL;
    int exit_code = tt_ctags_stream_finish(&stream, &stderr_out);
    unlink(tmp_path);

    TT_DIAG("worker", "done",
            "\"wid\":%d,\"files\":%d,\"syms\":%d,"
            "\"batches\":%d,\"bp\":%d,\"exit\":%d",
            ctx->worker_id, diag_files_done, diag_syms_total,
            diag_batches, diag_bp_count, exit_code);

    /* Push sentinel */
    tt_symbol_batch_t *sentinel = batch_alloc();
    if (sentinel)
    {
        sentinel->is_sentinel = true;
        if (exit_code != 0)
        {
            sentinel->has_error = true;
            if (stderr_out && stderr_out[0])
            {
                sentinel->error_msg = stderr_out;
                stderr_out = NULL;
            }
            else
            {
                tt_strbuf_t sb;
                tt_strbuf_init(&sb);
                tt_strbuf_appendf(&sb, "ctags worker %d exited with code %d",
                                  ctx->worker_id, exit_code);
                sentinel->error_msg = tt_strbuf_detach(&sb);
            }
        }
        enqueue_batch(sentinel, ctx);
    }

    free(stderr_out);
    local_batch_destroy(&lb);
    return NULL;
}

/* ===== Writer thread function ===== */

static void writer_add_error(writer_ctx_t *ctx, const char *msg)
{
    if (ctx->error_count >= ctx->error_cap)
    {
        int new_cap = ctx->error_cap == 0 ? 16 : ctx->error_cap * 2;
        char **tmp = realloc(ctx->errors, (size_t)new_cap * sizeof(char *));
        if (!tmp)
            return;
        ctx->errors = tmp;
        ctx->error_cap = new_cap;
    }
    ctx->errors[ctx->error_count++] = tt_strdup(msg);
}

static void *writer_fn(void *arg)
{
    writer_ctx_t *ctx = (writer_ctx_t *)arg;
    int sentinels_received = 0;
    int rows_in_transaction = 0;

    /* Diagnostic counters */
    int diag_commits = 0;
    int diag_batches = 0;
    int diag_empty_polls = 0;
    uint64_t diag_starvation_start = 0;
    bool diag_starving = false;

    tt_database_begin(ctx->db);

    while (sentinels_received < ctx->num_workers)
    {
        struct mpsc_queue_node *node;
        enum mpsc_queue_poll_result result = mpsc_queue_poll(ctx->queue, &node);

        if (result == MPSC_QUEUE_EMPTY)
        {
            if (!diag_starving)
            {
                diag_starving = true;
                diag_starvation_start = tt_monotonic_ms();
            }
            diag_empty_polls++;
            usleep(100); /* 100us yield */
            continue;
        }
        if (result == MPSC_QUEUE_RETRY)
            continue;

        /* Emit starvation event on empty->item transition */
        if (diag_starving)
        {
            uint64_t starvation_ms = tt_monotonic_ms() - diag_starvation_start;
            /* Only report if starved for > 10ms (avoid noise from normal polling) */
            if (starvation_ms > 10)
            {
                TT_DIAG("writer", "starvation",
                        "\"empty_polls\":%d,\"ms\":%llu",
                        diag_empty_polls,
                        (unsigned long long)starvation_ms);
            }
            diag_starving = false;
            diag_empty_polls = 0;
        }

        /* MPSC_QUEUE_ITEM */
        tt_symbol_batch_t *batch = tt_container_of(node, tt_symbol_batch_t, node);
        atomic_fetch_sub(ctx->pending_count, 1);

        if (batch->is_sentinel)
        {
            if (batch->has_error && batch->error_msg)
                writer_add_error(ctx, batch->error_msg);
            sentinels_received++;
            batch_free(batch);
            continue;
        }

        diag_batches++;

        /* Insert files */
        for (int i = 0; i < batch->file_count; i++)
        {
            tt_store_insert_file(ctx->store,
                                 batch->file_paths[i],
                                 batch->file_hashes[i],
                                 batch->file_languages[i],
                                 batch->file_sizes[i],
                                 batch->file_mtimes_sec[i],
                                 batch->file_mtimes_nsec[i],
                                 "");
            rows_in_transaction++;
        }
        ctx->files_written += batch->file_count;

        /* Insert symbols */
        if (batch->symbol_count > 0)
        {
            tt_store_insert_symbols(ctx->store, batch->symbols, batch->symbol_count);
            rows_in_transaction += batch->symbol_count;
            ctx->symbols_written += batch->symbol_count;
        }

        /* Insert imports */
        if (batch->import_count > 0)
        {
            tt_store_insert_imports(ctx->store, batch->imports, batch->import_count);
            rows_in_transaction += batch->import_count;
            ctx->imports_written += batch->import_count;
        }

        /* Periodic commit */
        if (rows_in_transaction >= TRANSACTION_BATCH_SIZE)
        {
            uint64_t commit_start = tt_monotonic_ms();
            if (tt_database_commit(ctx->db) < 0)
            {
                atomic_store(ctx->error_flag, 1);
                writer_add_error(ctx, "database commit failed");
                batch_free(batch);
                break;
            }
            uint64_t commit_ms = tt_monotonic_ms() - commit_start;
            diag_commits++;

            TT_DIAG("writer", "commit",
                    "\"rows\":%d,\"ms\":%llu,"
                    "\"q\":%lld,\"files_w\":%d,\"syms_w\":%d,"
                    "\"batches\":%d,\"commit_n\":%d",
                    rows_in_transaction,
                    (unsigned long long)commit_ms,
                    (long long)atomic_load(ctx->pending_count),
                    ctx->files_written, ctx->symbols_written,
                    diag_batches, diag_commits);

            tt_database_begin(ctx->db);
            rows_in_transaction = 0;
        }

        batch_free(batch);
    }

    /* Final commit */
    if (rows_in_transaction > 0)
    {
        uint64_t commit_start = tt_monotonic_ms();
        tt_database_commit(ctx->db);
        uint64_t commit_ms = tt_monotonic_ms() - commit_start;
        diag_commits++;

        TT_DIAG("writer", "commit",
                "\"rows\":%d,\"ms\":%llu,"
                "\"files_w\":%d,\"syms_w\":%d,"
                "\"batches\":%d,\"commit_n\":%d,\"final\":true",
                rows_in_transaction,
                (unsigned long long)commit_ms,
                ctx->files_written, ctx->symbols_written,
                diag_batches, diag_commits);
    }

    TT_DIAG("writer", "done",
            "\"files\":%d,\"syms\":%d,\"imports\":%d,"
            "\"batches\":%d,\"commits\":%d",
            ctx->files_written, ctx->symbols_written,
            ctx->imports_written, diag_batches, diag_commits);

    /* Signal completion so tt_pipeline_finished() returns true */
    if (ctx->finished)
        atomic_store(ctx->finished, 1);

    return NULL;
}

/* ===== Pipeline API ===== */

/* ===== LPT load balancing ===== */

typedef struct
{
    int index;
    int64_t size;
} balanced_entry_t;

static int cmp_size_desc(const void *a, const void *b)
{
    int64_t sa = ((const balanced_entry_t *)a)->size;
    int64_t sb = ((const balanced_entry_t *)b)->size;
    if (sb > sa)
        return 1;
    if (sb < sa)
        return -1;
    return 0;
}

/*
 * balance_files -- Reorder file_paths for balanced worker distribution.
 *
 * Uses LPT (Longest Processing Time first) greedy scheduling:
 * sort files by size descending, assign each to the worker bucket
 * with the smallest cumulative size.
 *
 * Returns a newly allocated array of reordered file paths (caller must free
 * the array but NOT the strings — they are borrowed from the original).
 * chunk_sizes_out must point to an array of K ints.
 *
 * Returns NULL on allocation failure (caller should fall back to equal chunks).
 */
static const char **balance_files(const char **paths, const int64_t *sizes,
                                  int count, int K, int *chunk_sizes_out)
{
    if (count <= 0 || K <= 0)
        return NULL;

    /* Build index array sorted by size descending */
    balanced_entry_t *entries = malloc(sizeof(balanced_entry_t) * (size_t)count);
    if (!entries)
        return NULL;

    for (int i = 0; i < count; i++)
    {
        entries[i].index = i;
        entries[i].size = sizes[i];
    }

    qsort(entries, (size_t)count, sizeof(balanced_entry_t), cmp_size_desc);

    /* Greedy assign: each file goes to the bucket with minimum total size.
     * K is small (≤16), so linear scan is fine — no heap needed. */
    int64_t *bucket_sizes = calloc((size_t)K, sizeof(int64_t));
    int *bucket_counts = calloc((size_t)K, sizeof(int));
    int **bucket_indices = calloc((size_t)K, sizeof(int *));
    int *bucket_caps = calloc((size_t)K, sizeof(int));

    if (!bucket_sizes || !bucket_counts || !bucket_indices || !bucket_caps)
    {
        free(entries);
        free(bucket_sizes);
        free(bucket_counts);
        free(bucket_indices);
        free(bucket_caps);
        return NULL;
    }

    /* Pre-allocate bucket arrays */
    int avg_per_bucket = (count / K) + 16;
    for (int w = 0; w < K; w++)
    {
        bucket_indices[w] = malloc(sizeof(int) * (size_t)avg_per_bucket);
        bucket_caps[w] = avg_per_bucket;
        if (!bucket_indices[w])
        {
            for (int j = 0; j < w; j++)
                free(bucket_indices[j]);
            free(entries);
            free(bucket_sizes);
            free(bucket_counts);
            free(bucket_indices);
            free(bucket_caps);
            return NULL;
        }
    }

    for (int i = 0; i < count; i++)
    {
        /* Find bucket with minimum total size */
        int min_w = 0;
        for (int w = 1; w < K; w++)
        {
            if (bucket_sizes[w] < bucket_sizes[min_w])
                min_w = w;
        }

        /* Add to that bucket */
        if (bucket_counts[min_w] >= bucket_caps[min_w])
        {
            bucket_caps[min_w] *= 2;
            bucket_indices[min_w] = realloc(bucket_indices[min_w],
                                            sizeof(int) * (size_t)bucket_caps[min_w]);
        }
        bucket_indices[min_w][bucket_counts[min_w]] = entries[i].index;
        bucket_counts[min_w]++;
        bucket_sizes[min_w] += entries[i].size;
    }

    /* Emit diagnostic with per-worker byte totals */
    if (tt_diag_enabled())
    {
        char buf[512];
        int pos = 0;
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "\"worker_bytes\":[");
        for (int w = 0; w < K; w++)
        {
            if (w > 0)
                pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, ",");
            pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%lld",
                            (long long)bucket_sizes[w]);
        }
        snprintf(buf + pos, sizeof(buf) - (size_t)pos, "]");
        TT_DIAG("pipeline", "balance", "%s", buf);
    }

    /* Build reordered paths array: bucket 0 files first, then bucket 1, etc. */
    const char **reordered = malloc(sizeof(const char *) * (size_t)count);
    if (!reordered)
    {
        for (int w = 0; w < K; w++)
            free(bucket_indices[w]);
        free(entries);
        free(bucket_sizes);
        free(bucket_counts);
        free(bucket_indices);
        free(bucket_caps);
        return NULL;
    }

    int pos = 0;
    for (int w = 0; w < K; w++)
    {
        chunk_sizes_out[w] = bucket_counts[w];
        for (int j = 0; j < bucket_counts[w]; j++)
        {
            reordered[pos++] = paths[bucket_indices[w][j]];
        }
    }

    for (int w = 0; w < K; w++)
        free(bucket_indices[w]);
    free(entries);
    free(bucket_sizes);
    free(bucket_counts);
    free(bucket_indices);
    free(bucket_caps);

    return reordered;
}

int tt_pipeline_start(const tt_pipeline_config_t *config,
                      tt_progress_t *progress,
                      tt_pipeline_handle_t **handle)
{
    if (!config || !config->db || !config->store || !handle)
        return -1;

    int K = config->num_workers;
    if (K <= 0)
        K = tt_cpu_count();
    if (K > config->file_count)
        K = config->file_count;
    if (K < 1)
        K = 1;

    /* Resolve project root */
    char *resolved_root = tt_realpath(config->project_root);
    if (!resolved_root)
        resolved_root = tt_strdup(config->project_root);
    size_t rlen = strlen(resolved_root);
    while (rlen > 0 && (resolved_root[rlen - 1] == '/' || resolved_root[rlen - 1] == '\\'))
        resolved_root[--rlen] = '\0';

    /* Allocate handle */
    tt_pipeline_handle_t *h = calloc(1, sizeof(tt_pipeline_handle_t));
    if (!h)
    {
        free(resolved_root);
        return -1;
    }

    h->num_workers = K;
    h->progress = progress;
    atomic_store(&h->pending_count, 0);
    atomic_store(&h->error_flag, 0);
    atomic_store(&h->finished, 0);

    mpsc_queue_init(&h->queue);

    /* Drop FTS triggers and secondary indexes for bulk insert performance.
     * In incremental mode (single-file reindex), keep them active so FTS
     * is updated incrementally via triggers — avoids O(total_symbols) rebuild. */
    if (!config->incremental) {
        tt_schema_drop_fts_triggers(config->db->db);
        tt_schema_drop_secondary_indexes(config->db->db);
    }

    /* Set up worker contexts */
    h->worker_ctxs = calloc((size_t)K, sizeof(worker_ctx_t));
    h->worker_threads = calloc((size_t)K, sizeof(pthread_t));
    if (!h->worker_ctxs || !h->worker_threads)
    {
        free(h->worker_ctxs);
        free(h->worker_threads);
        free(h);
        free(resolved_root);
        return -1;
    }

    /* Distribute files to workers.
     * If file sizes are available, use LPT load balancing.
     * Otherwise, fall back to equal-count contiguous chunks. */
    int *chunk_sizes = calloc((size_t)K, sizeof(int));
    const char **work_paths = config->file_paths;
    h->balanced_paths = NULL;

    if (config->file_sizes && K > 1)
    {
        const char **balanced = balance_files(config->file_paths, config->file_sizes,
                                              config->file_count, K, chunk_sizes);
        if (balanced)
        {
            work_paths = balanced;
            h->balanced_paths = balanced;
        }
        else
        {
            /* Fallback: equal chunks */
            int chunk = config->file_count / K;
            int remainder = config->file_count % K;
            for (int w = 0; w < K; w++)
                chunk_sizes[w] = chunk + (w < remainder ? 1 : 0);
        }
    }
    else
    {
        int chunk = config->file_count / K;
        int remainder = config->file_count % K;
        for (int w = 0; w < K; w++)
            chunk_sizes[w] = chunk + (w < remainder ? 1 : 0);
    }

    int offset = 0;
    for (int w = 0; w < K; w++)
    {
        h->worker_ctxs[w].project_root = resolved_root;
        h->worker_ctxs[w].ctags_path = config->ctags_path;
        h->worker_ctxs[w].file_paths = work_paths + offset;
        h->worker_ctxs[w].file_count = chunk_sizes[w];
        h->worker_ctxs[w].worker_id = w;
        h->worker_ctxs[w].ctags_timeout_sec = config->ctags_timeout_sec;
        h->worker_ctxs[w].max_file_size_bytes = config->max_file_size_bytes;
        h->worker_ctxs[w].queue = &h->queue;
        h->worker_ctxs[w].pending_count = &h->pending_count;
        h->worker_ctxs[w].error_flag = &h->error_flag;
        h->worker_ctxs[w].progress = progress;
        offset += chunk_sizes[w];
    }
    free(chunk_sizes);

    /* Set up writer context */
    h->writer_ctx.db = config->db;
    h->writer_ctx.store = config->store;
    h->writer_ctx.queue = &h->queue;
    h->writer_ctx.pending_count = &h->pending_count;
    h->writer_ctx.error_flag = &h->error_flag;
    h->writer_ctx.num_workers = K;
    h->writer_ctx.errors = NULL;
    h->writer_ctx.error_count = 0;
    h->writer_ctx.error_cap = 0;
    h->writer_ctx.files_written = 0;
    h->writer_ctx.symbols_written = 0;
    h->writer_ctx.imports_written = 0;
    h->writer_ctx.finished = &h->finished;

    /* Launch writer thread */
    if (pthread_create(&h->writer_thread, NULL, writer_fn, &h->writer_ctx) != 0)
    {
        free((void *)h->balanced_paths);
        free(h->worker_ctxs);
        free(h->worker_threads);
        free(h);
        free(resolved_root);
        return -1;
    }

    /* Launch worker threads */
    for (int w = 0; w < K; w++)
    {
        if (h->worker_ctxs[w].file_count <= 0)
        {
            /* Push sentinel for empty workers */
            tt_symbol_batch_t *sentinel = batch_alloc();
            if (sentinel)
            {
                sentinel->is_sentinel = true;
                mpsc_queue_insert(&h->queue, &sentinel->node);
                atomic_fetch_add(&h->pending_count, 1);
            }
            continue;
        }
        if (pthread_create(&h->worker_threads[w], NULL, worker_fn,
                           &h->worker_ctxs[w]) != 0)
        {
            /* Fallback: push sentinel for failed thread */
            tt_symbol_batch_t *sentinel = batch_alloc();
            if (sentinel)
            {
                sentinel->is_sentinel = true;
                sentinel->has_error = true;
                sentinel->error_msg = tt_strdup("failed to create worker thread");
                mpsc_queue_insert(&h->queue, &sentinel->node);
                atomic_fetch_add(&h->pending_count, 1);
            }
        }
    }

    /* Store resolved_root pointer in first worker context.
     * All workers share it and it must survive until join.
     * We stash it as a void* in the handle for cleanup. */
    /* We rely on resolved_root being freed in tt_pipeline_join. */
    /* TODO: better ownership -- for now, leaked if not joined. */

    *handle = h;
    return 0;
}

bool tt_pipeline_finished(tt_pipeline_handle_t *handle)
{
    if (!handle)
        return true;
    return atomic_load(&handle->finished) != 0;
}

int tt_pipeline_join(tt_pipeline_handle_t *handle,
                     tt_pipeline_result_t *result)
{
    if (!handle)
        return -1;

    /* Join worker threads */
    for (int w = 0; w < handle->num_workers; w++)
    {
        if (handle->worker_ctxs[w].file_count > 0)
            pthread_join(handle->worker_threads[w], NULL);
    }

    /* Join writer thread */
    pthread_join(handle->writer_thread, NULL);

    /* Populate result */
    if (result)
    {
        memset(result, 0, sizeof(*result));
        result->files_indexed = handle->writer_ctx.files_written;
        result->symbols_indexed = handle->writer_ctx.symbols_written;
        result->imports_indexed = handle->writer_ctx.imports_written;
        result->errors_count = handle->writer_ctx.error_count;
        result->errors = handle->writer_ctx.errors;
        /* Transfer ownership of errors to result */
        handle->writer_ctx.errors = NULL;
        handle->writer_ctx.error_count = 0;
    }
    else
    {
        /* Free errors if no result to receive them */
        for (int i = 0; i < handle->writer_ctx.error_count; i++)
            free(handle->writer_ctx.errors[i]);
        free(handle->writer_ctx.errors);
    }

    /* Free resolved_root (shared across all worker contexts) */
    if (handle->num_workers > 0)
        free((void *)handle->worker_ctxs[0].project_root);

    free((void *)handle->balanced_paths);
    free(handle->worker_ctxs);
    free(handle->worker_threads);
    free(handle);

    return 0;
}

void tt_pipeline_finalize_schema(tt_database_t *db)
{
    if (!db)
        return;
    tt_schema_create_secondary_indexes(db->db);
    tt_schema_rebuild_fts(db->db);
    tt_schema_create_fts_triggers(db->db);
}

int tt_pipeline_run(const tt_pipeline_config_t *config,
                    tt_pipeline_result_t *result)
{
    tt_pipeline_handle_t *handle = NULL;
    int rc = tt_pipeline_start(config, NULL, &handle);
    if (rc < 0)
        return rc;
    rc = tt_pipeline_join(handle, result);
    if (rc == 0 && !config->incremental)
        tt_pipeline_finalize_schema(config->db);
    return rc;
}

void tt_pipeline_result_free(tt_pipeline_result_t *result)
{
    if (!result)
        return;
    for (int i = 0; i < result->errors_count; i++)
        free(result->errors[i]);
    free(result->errors);
    result->errors = NULL;
    result->errors_count = 0;
}

#endif /* !TT_PLATFORM_WINDOWS */
