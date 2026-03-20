/*
 * index_store.c -- CRUD operations on the TokToken SQLite database.
 */

#include "index_store.h"
#include "arena.h"
#include "symbol_kind.h"
#include "symbol_scorer.h"
#include "platform.h"
#include "fast_hash.h"
#include "error.h"
#include "str_util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/stat.h>
#include <math.h>
#include <stdint.h>

/* Forward declarations */
static int compare_by_score_desc(const void *a, const void *b);

/* ---- Helper: prepare a statement ---- */

static int prepare(sqlite3 *db, const char *sql, sqlite3_stmt **out)
{
    int rc = sqlite3_prepare_v2(db, sql, -1, out, NULL);
    if (rc != SQLITE_OK)
    {
        tt_error_set("index_store: prepare failed: %s (SQL: %.80s)",
                     sqlite3_errmsg(db), sql);
        return -1;
    }
    return 0;
}

/* ---- Helper: finalize if non-NULL ---- */

static void finalize(sqlite3_stmt **stmt)
{
    if (*stmt)
    {
        sqlite3_finalize(*stmt);
        *stmt = NULL;
    }
}

/* ---- Helper: duplicate a column text, empty string if NULL ---- */

static char *col_strdup(sqlite3_stmt *stmt, int col)
{
    const char *val = (const char *)sqlite3_column_text(stmt, col);
    return tt_strdup(val ? val : "");
}

/* Nullable version: returns NULL if column is SQL NULL */
static char *col_strdup_nullable(sqlite3_stmt *stmt, int col)
{
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL)
        return NULL;
    const char *val = (const char *)sqlite3_column_text(stmt, col);
    return val ? tt_strdup(val) : NULL;
}

/* ---- Arena-aware column helpers (strings allocated from arena) ---- */

static char *col_astrdup(tt_arena_t *a, sqlite3_stmt *stmt, int col)
{
    const char *val = (const char *)sqlite3_column_text(stmt, col);
    return tt_arena_strdup(a, val ? val : "");
}

static char *col_astrdup_nullable(tt_arena_t *a, sqlite3_stmt *stmt, int col)
{
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL)
        return NULL;
    const char *val = (const char *)sqlite3_column_text(stmt, col);
    return val ? tt_arena_strdup(a, val) : NULL;
}

/* Read a full symbol row with all strings allocated from arena. */
static void read_symbol_row_arena(tt_arena_t *a, sqlite3_stmt *stmt, tt_symbol_t *sym)
{
    sym->id = col_astrdup(a, stmt, 0);
    sym->file = col_astrdup(a, stmt, 1);
    sym->name = col_astrdup(a, stmt, 2);
    sym->qualified_name = col_astrdup(a, stmt, 3);
    {
        const char *kind_str = (const char *)sqlite3_column_text(stmt, 4);
        sym->kind = tt_kind_from_ctags(kind_str ? kind_str : "");
    }
    sym->language = col_astrdup(a, stmt, 5);
    sym->signature = col_astrdup(a, stmt, 6);
    sym->docstring = col_astrdup(a, stmt, 7);
    sym->summary = col_astrdup(a, stmt, 8);
    sym->decorators = col_astrdup(a, stmt, 9);
    sym->keywords = col_astrdup(a, stmt, 10);
    sym->parent_id = col_astrdup_nullable(a, stmt, 11);
    sym->line = sqlite3_column_int(stmt, 12);
    sym->end_line = sqlite3_column_int(stmt, 13);
    sym->byte_offset = sqlite3_column_int(stmt, 14);
    sym->byte_length = sqlite3_column_int(stmt, 15);
    sym->content_hash = col_astrdup(a, stmt, 16);
}

/*
 * Deep-copy all string fields of a symbol from arena memory to heap memory.
 * After this call, the symbol owns heap-allocated strings and can be freed
 * normally with tt_symbol_free(). The old arena pointers are NOT freed
 * (the arena will reclaim them in bulk).
 */
static void symbol_to_heap(tt_symbol_t *sym)
{
    sym->id = tt_strdup(sym->id);
    sym->file = tt_strdup(sym->file);
    sym->name = tt_strdup(sym->name);
    sym->qualified_name = tt_strdup(sym->qualified_name);
    sym->language = tt_strdup(sym->language);
    sym->signature = tt_strdup(sym->signature);
    sym->docstring = tt_strdup(sym->docstring);
    sym->summary = tt_strdup(sym->summary);
    sym->decorators = tt_strdup(sym->decorators);
    sym->keywords = tt_strdup(sym->keywords);
    sym->parent_id = sym->parent_id ? tt_strdup(sym->parent_id) : NULL;
    sym->content_hash = tt_strdup(sym->content_hash);
}

/* ---- Free functions ---- */

void tt_symbol_result_free(tt_symbol_result_t *r)
{
    if (!r)
        return;
    tt_symbol_free(&r->sym);
}

void tt_file_record_free(tt_file_record_t *r)
{
    if (!r)
        return;
    free(r->path);
    free(r->hash);
    free(r->language);
    free(r->summary);
    free(r->indexed_at);
    memset(r, 0, sizeof(*r));
}

void tt_search_results_free(tt_search_results_t *r)
{
    if (!r)
        return;
    for (int i = 0; i < r->count; i++)
    {
        tt_symbol_result_free(&r->results[i]);
    }
    free(r->results);
    r->results = NULL;
    r->count = 0;
}

void tt_stats_free(tt_stats_t *s)
{
    if (!s)
        return;
    for (int i = 0; i < s->language_count; i++)
        free(s->languages[i].name);
    free(s->languages);
    for (int i = 0; i < s->kind_count; i++)
        free(s->kinds[i].name);
    free(s->kinds);
    for (int i = 0; i < s->dir_count; i++)
        free(s->dirs[i].name);
    free(s->dirs);
    memset(s, 0, sizeof(*s));
}

static const char *SQL_INSERT_FILE =
    "INSERT OR REPLACE INTO files (path, hash, language, summary, size_bytes, mtime_sec, mtime_nsec, indexed_at) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?)";

static const char *SQL_INSERT_SYM =
    "INSERT OR REPLACE INTO symbols "
    "(id, file, name, qualified_name, kind, language, signature, docstring, "
    "summary, decorators, keywords, parent_id, line, end_line, byte_offset, "
    "byte_length, content_hash) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

static const char *SQL_DELETE_FILE =
    "DELETE FROM files WHERE path = ?";

static const char *SQL_DELETE_SYMS =
    "DELETE FROM symbols WHERE file = ?";

static const char *SQL_GET_FILE =
    "SELECT path, hash, language, summary, size_bytes, indexed_at FROM files WHERE path = ?";

static const char *SQL_GET_SYM =
    "SELECT id, file, name, qualified_name, kind, language, signature, docstring, "
    "summary, decorators, keywords, parent_id, line, end_line, byte_offset, "
    "byte_length, content_hash FROM symbols WHERE id = ?";

static const char *SQL_GET_SYMS_FILE =
    "SELECT id, file, name, qualified_name, kind, language, signature, docstring, "
    "summary, decorators, keywords, parent_id, line, end_line, byte_offset, "
    "byte_length, content_hash FROM symbols WHERE file = ? ORDER BY line";

static const char *SQL_SET_META =
    "INSERT OR REPLACE INTO metadata (key, value) VALUES (?, ?)";

static const char *SQL_GET_META =
    "SELECT value FROM metadata WHERE key = ?";

static const char *SQL_UPDATE_SYM_SUMMARY =
    "UPDATE symbols SET summary = ? WHERE id = ?";

static const char *SQL_UPDATE_FILE_SUMMARY =
    "UPDATE files SET summary = ? WHERE path = ?";

int tt_store_init(tt_index_store_t *store, tt_database_t *db)
{
    memset(store, 0, sizeof(*store));
    store->db = db;

    sqlite3 *sdb = db->db;

    if (prepare(sdb, SQL_INSERT_FILE, &store->insert_file) < 0)
        goto fail;
    if (prepare(sdb, SQL_INSERT_SYM, &store->insert_sym) < 0)
        goto fail;
    if (prepare(sdb, SQL_DELETE_FILE, &store->delete_file) < 0)
        goto fail;
    if (prepare(sdb, SQL_DELETE_SYMS, &store->delete_syms) < 0)
        goto fail;
    if (prepare(sdb, SQL_GET_FILE, &store->get_file) < 0)
        goto fail;
    if (prepare(sdb, SQL_GET_SYM, &store->get_sym) < 0)
        goto fail;
    if (prepare(sdb, SQL_GET_SYMS_FILE, &store->get_syms_file) < 0)
        goto fail;
    if (prepare(sdb, SQL_SET_META, &store->set_meta) < 0)
        goto fail;
    if (prepare(sdb, SQL_GET_META, &store->get_meta) < 0)
        goto fail;
    if (prepare(sdb, SQL_UPDATE_SYM_SUMMARY, &store->update_sym_summary) < 0)
        goto fail;
    if (prepare(sdb, SQL_UPDATE_FILE_SUMMARY, &store->update_file_summary) < 0)
        goto fail;

    return 0;

fail:
    tt_store_close(store);
    return -1;
}

void tt_store_close(tt_index_store_t *store)
{
    if (!store)
        return;
    finalize(&store->insert_file);
    finalize(&store->insert_sym);
    finalize(&store->delete_file);
    finalize(&store->delete_syms);
    finalize(&store->get_file);
    finalize(&store->get_sym);
    finalize(&store->get_syms_file);
    finalize(&store->set_meta);
    finalize(&store->get_meta);
    finalize(&store->update_sym_summary);
    finalize(&store->update_file_summary);
    store->db = NULL;
}

int tt_store_insert_file(tt_index_store_t *store, const char *path, const char *hash,
                         const char *language, int64_t size_bytes,
                         int64_t mtime_sec, int64_t mtime_nsec,
                         const char *summary)
{
    sqlite3_stmt *stmt = store->insert_file;
    const char *ts = tt_now_rfc3339();

    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, hash, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, language, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, summary ? summary : "", -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 5, size_bytes);
    sqlite3_bind_int64(stmt, 6, mtime_sec);
    sqlite3_bind_int64(stmt, 7, mtime_nsec);
    sqlite3_bind_text(stmt, 8, ts, -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_reset(stmt);

    if (rc != SQLITE_DONE)
    {
        tt_error_set("index_store: insert_file failed: %s",
                     sqlite3_errmsg(store->db->db));
        return -1;
    }
    return 0;
}

int tt_store_insert_symbol(tt_index_store_t *store, const tt_symbol_t *sym)
{
    sqlite3_stmt *stmt = store->insert_sym;

    /* SQLITE_STATIC is safe: symbol data lives until sqlite3_reset below. */
    sqlite3_bind_text(stmt, 1, sym->id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, sym->file, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, sym->name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, sym->qualified_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, tt_kind_to_str(sym->kind), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, sym->language, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 7, sym->signature ? sym->signature : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 8, sym->docstring ? sym->docstring : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 9, sym->summary ? sym->summary : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 10, sym->decorators ? sym->decorators : "[]", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 11, sym->keywords ? sym->keywords : "[]", -1, SQLITE_STATIC);

    if (sym->parent_id)
    {
        sqlite3_bind_text(stmt, 12, sym->parent_id, -1, SQLITE_STATIC);
    }
    else
    {
        sqlite3_bind_null(stmt, 12);
    }

    sqlite3_bind_int(stmt, 13, sym->line);
    sqlite3_bind_int(stmt, 14, sym->end_line);
    sqlite3_bind_int(stmt, 15, sym->byte_offset);
    sqlite3_bind_int(stmt, 16, sym->byte_length);
    sqlite3_bind_text(stmt, 17, sym->content_hash ? sym->content_hash : "", -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_reset(stmt);

    if (rc != SQLITE_DONE)
    {
        tt_error_set("index_store: insert_symbol failed: %s",
                     sqlite3_errmsg(store->db->db));
        return -1;
    }
    return 0;
}

int tt_store_insert_symbols(tt_index_store_t *store, const tt_symbol_t *syms, int count)
{
    for (int i = 0; i < count; i++)
    {
        if (tt_store_insert_symbol(store, &syms[i]) < 0)
            return -1;
    }
    return 0;
}

/* Read a full symbol row from a stepped statement (17 columns). */
static void read_symbol_row(sqlite3_stmt *stmt, tt_symbol_t *sym)
{
    sym->id = col_strdup(stmt, 0);
    sym->file = col_strdup(stmt, 1);
    sym->name = col_strdup(stmt, 2);
    sym->qualified_name = col_strdup(stmt, 3);
    {
        const char *kind_str = (const char *)sqlite3_column_text(stmt, 4);
        sym->kind = tt_kind_from_ctags(kind_str ? kind_str : "");
    }
    sym->language = col_strdup(stmt, 5);
    sym->signature = col_strdup(stmt, 6);
    sym->docstring = col_strdup(stmt, 7);
    sym->summary = col_strdup(stmt, 8);
    sym->decorators = col_strdup(stmt, 9);
    sym->keywords = col_strdup(stmt, 10);
    sym->parent_id = col_strdup_nullable(stmt, 11);
    sym->line = sqlite3_column_int(stmt, 12);
    sym->end_line = sqlite3_column_int(stmt, 13);
    sym->byte_offset = sqlite3_column_int(stmt, 14);
    sym->byte_length = sqlite3_column_int(stmt, 15);
    sym->content_hash = col_strdup(stmt, 16);
}

/* Build an FTS5 query string from a user query.
 * Input: "auth service"
 * Output: "auth"* OR "service"*
 * [caller-frees] Returns NULL on error.
 */
static char *build_fts5_query(const char *query)
{
    int word_count = 0;
    char **words = tt_str_split_words(query, &word_count);
    if (!words || word_count == 0)
    {
        tt_str_split_free(words);
        return NULL;
    }

    tt_strbuf_t sb;
    tt_strbuf_init(&sb);

    int valid = 0;
    for (int i = 0; i < word_count; i++)
    {
        /* Skip words that are entirely non-alphanumeric (e.g. "*", "??") */
        bool has_alnum = false;
        for (const char *p = words[i]; *p; p++)
        {
            if (isalnum((unsigned char)*p))
            {
                has_alnum = true;
                break;
            }
        }
        if (!has_alnum)
            continue;

        if (valid > 0)
            tt_strbuf_append_str(&sb, " OR ");

        /* Escape double quotes in word */
        tt_strbuf_append_char(&sb, '"');
        for (const char *p = words[i]; *p; p++)
        {
            if (*p == '"')
                tt_strbuf_append_char(&sb, '"');
            tt_strbuf_append_char(&sb, *p);
        }
        tt_strbuf_append_str(&sb, "\"*");
        valid++;
    }

    tt_str_split_free(words);

    if (valid == 0)
    {
        tt_strbuf_free(&sb);
        return NULL;
    }

    return tt_strbuf_detach(&sb);
}

/* Build a WHERE clause for kind/language filters.
 * Appends to sb. Caller should start with "SELECT ... FROM symbols".
 * Returns the number of bind parameters added.
 */
static int build_filter_clause(tt_strbuf_t *sb, const char **kinds, int kind_count,
                               const char *language, bool has_where)
{
    int bind_count = 0;

    if (kind_count > 0)
    {
        tt_strbuf_append_str(sb, has_where ? " AND " : " WHERE ");
        has_where = true;
        tt_strbuf_append_str(sb, "kind IN (");
        for (int i = 0; i < kind_count; i++)
        {
            if (i > 0)
                tt_strbuf_append_char(sb, ',');
            tt_strbuf_append_char(sb, '?');
            bind_count++;
        }
        tt_strbuf_append_char(sb, ')');
    }

    if (language && language[0] != '\0')
    {
        tt_strbuf_append_str(sb, has_where ? " AND " : " WHERE ");
        tt_strbuf_append_str(sb, "language = ?");
        bind_count++;
    }

    return bind_count;
}

/* Simple case-insensitive substring match (for regex fallback). */
static bool simple_match(const char *text, const char *pattern_lower)
{
    if (!text || !pattern_lower)
        return false;
    char *text_lower = tt_str_tolower(text);
    if (!text_lower)
        return false;
    bool found = (strstr(text_lower, pattern_lower) != NULL);
    free(text_lower);
    return found;
}

int tt_store_search_symbols(tt_index_store_t *store, const char *query,
                            const char **kinds, int kind_count,
                            const char *language, const char *file_pattern,
                            int max_results, bool regex,
                            tt_search_results_t *out)
{
    memset(out, 0, sizeof(*out));

    char *query_lower = query ? tt_str_tolower(query) : tt_strdup("");
    int word_count = 0;
    char **query_words = tt_str_split_words(query_lower, &word_count);
    bool match_all = (!query_lower[0]);

    /* Load centrality map for search ranking bonus */
    tt_hashmap_t *centrality_map = tt_store_load_centrality(store);

    /* Phase 1: Get candidates */
    sqlite3_stmt *candidates = NULL;
    bool use_fts = (!regex && !match_all);
    char *fts_query = NULL;

    if (use_fts)
    {
        fts_query = build_fts5_query(query);
        if (!fts_query)
        {
            use_fts = false;
            match_all = true; /* No valid search terms -> return all */
        }
    }

    tt_strbuf_t sql;
    tt_strbuf_init_cap(&sql, 512);

    if (use_fts)
    {
        tt_strbuf_append_str(&sql,
                             "SELECT s.id, s.file, s.name, s.qualified_name, s.kind, s.language, "
                             "s.signature, s.docstring, s.summary, s.decorators, s.keywords, "
                             "s.parent_id, s.line, s.end_line, s.byte_offset, s.byte_length, "
                             "s.content_hash FROM symbols s "
                             "INNER JOIN symbols_fts f ON s.rowid = f.rowid "
                             "WHERE symbols_fts MATCH ?");

        int extra = build_filter_clause(&sql, kinds, kind_count, language, true);
        (void)extra;

        if (prepare(store->db->db, sql.data, &candidates) < 0)
        {
            /* FTS5 failed, fallback to full scan */
            use_fts = false;
            tt_strbuf_reset(&sql);
        }
        else
        {
            int bind_idx = 1;
            sqlite3_bind_text(candidates, bind_idx++, fts_query, -1, SQLITE_TRANSIENT);
            for (int i = 0; i < kind_count; i++)
            {
                sqlite3_bind_text(candidates, bind_idx++, kinds[i], -1, SQLITE_TRANSIENT);
            }
            if (language && language[0])
            {
                sqlite3_bind_text(candidates, bind_idx++, language, -1, SQLITE_TRANSIENT);
            }
        }
    }

    if (!use_fts)
    {
        tt_strbuf_reset(&sql);
        tt_strbuf_append_str(&sql,
                             "SELECT id, file, name, qualified_name, kind, language, "
                             "signature, docstring, summary, decorators, keywords, "
                             "parent_id, line, end_line, byte_offset, byte_length, "
                             "content_hash FROM symbols");

        build_filter_clause(&sql, kinds, kind_count, language, false);

        if (prepare(store->db->db, sql.data, &candidates) < 0)
        {
            tt_strbuf_free(&sql);
            free(fts_query);
            free(query_lower);
            tt_str_split_free(query_words);
            return -1;
        }

        int bind_idx = 1;
        for (int i = 0; i < kind_count; i++)
        {
            sqlite3_bind_text(candidates, bind_idx++, kinds[i], -1, SQLITE_TRANSIENT);
        }
        if (language && language[0])
        {
            sqlite3_bind_text(candidates, bind_idx++, language, -1, SQLITE_TRANSIENT);
        }
    }

    tt_strbuf_free(&sql);

    /* Phase 2: Filter + score.
     *
     * Arena strategy: all symbol strings from SQLite go into the arena.
     * Discarded symbols (filtered out, score too low, over limit) are
     * simply abandoned — the arena reclaims them in bulk.
     * Surviving symbols get deep-copied to heap before the arena is freed,
     * so callers can free them normally with tt_symbol_free(). */
    tt_arena_t *arena = tt_arena_new(0);

    tt_array_t results;
    tt_array_init(&results);

    /* For regex mode, we use simple case-insensitive substring as fallback
     * (PCRE2 support will be added later; for now, substring match). */
    bool regex_mode = (regex && query_lower[0]);

    while (sqlite3_step(candidates) == SQLITE_ROW)
    {
        tt_symbol_t sym;
        memset(&sym, 0, sizeof(sym));
        read_symbol_row_arena(arena, candidates, &sym);

        /* File pattern filter */
        if (file_pattern && file_pattern[0])
        {
            if (!tt_fnmatch(file_pattern, sym.file, true))
                continue; /* arena reclaims strings */
        }

        double score = 1.0;

        if (regex_mode)
        {
            /* Simple substring match on name, qualified_name, signature */
            if (!simple_match(sym.name, query_lower) &&
                !simple_match(sym.qualified_name, query_lower) &&
                !simple_match(sym.signature, query_lower))
                continue; /* arena reclaims strings */
        }
        else if (!match_all)
        {
            /* Use the full scoring cascade from symbol_scorer */
            score = (double)tt_score_symbol(
                sym.name, sym.qualified_name, sym.signature,
                sym.summary, sym.keywords, sym.docstring,
                query_lower, (const char **)query_words, word_count);

            /* Dynamic threshold: for multi-word queries, require that
             * at least half the query words contribute to the score.
             * 1-2 words: min 5 (one name word match)
             * 3-4 words: min 10 (two name word matches, or substring)
             * 5+  words: min 15, etc. */
            int min_score = TT_SCORE_MIN_THRESHOLD;
            if (word_count > 2)
                min_score = TT_WEIGHT_NAME_WORD * ((word_count + 1) / 2);

            if (score < min_score)
                continue; /* arena reclaims strings */
        }

        /* Apply centrality bonus: log(1+importers) * 0.3 */
        if (centrality_map && sym.file) {
            void *val = tt_hashmap_get(centrality_map, sym.file);
            if (val) {
                double cent = (double)(intptr_t)val / 1000.0;
                score += cent * TT_WEIGHT_CENTRALITY;
            }
        }

        tt_symbol_result_t *r = malloc(sizeof(tt_symbol_result_t));
        if (!r)
            break;
        r->sym = sym;
        r->score = score;
        r->dbg_name = r->dbg_sig = r->dbg_summary = r->dbg_keyword = r->dbg_docstring = 0;
        tt_array_push(&results, r);
    }

    sqlite3_finalize(candidates);
    free(fts_query);

    /* Phase 3: Sort by score DESC (unless matchAll or regex) */
    if (!match_all && !regex)
    {
        tt_array_sort(&results, compare_by_score_desc);
    }

    /* Phase 4: Limit to max_results.
     * Over-limit results are abandoned — arena owns their strings,
     * we only free the malloc'd container struct. */
    int final_count = (int)results.len;
    if (max_results > 0 && final_count > max_results)
    {
        for (int i = max_results; i < final_count; i++)
            free(results.items[i]); /* free container, arena owns strings */
        final_count = max_results;
    }

    /* Phase 5: Copy survivors to output, deep-copying strings to heap.
     * After this, the arena can be safely freed. */
    out->results = malloc(sizeof(tt_symbol_result_t) * (final_count > 0 ? final_count : 1));
    out->count = final_count;

    if (out->results)
    {
        for (int i = 0; i < final_count; i++)
        {
            tt_symbol_result_t *r = results.items[i];
            out->results[i] = *r;
            symbol_to_heap(&out->results[i].sym);
            free(r);
        }
    }

    tt_arena_free(arena);
    tt_array_free(&results);
    tt_hashmap_free(centrality_map);
    free(query_lower);
    tt_str_split_free(query_words);
    return 0;
}

/* Comparator for sorting by score DESC. */
static int compare_by_score_desc(const void *a, const void *b)
{
    const tt_symbol_result_t *ra = *(const tt_symbol_result_t *const *)a;
    const tt_symbol_result_t *rb = *(const tt_symbol_result_t *const *)b;
    if (ra->score > rb->score)
        return -1;
    if (ra->score < rb->score)
        return 1;
    return 0;
}

int tt_store_get_symbol(tt_index_store_t *store, const char *id, tt_symbol_result_t *out)
{
    memset(out, 0, sizeof(*out));

    sqlite3_stmt *stmt = store->get_sym;
    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
    {
        read_symbol_row(stmt, &out->sym);
        out->score = 1.0;
        sqlite3_reset(stmt);
        return 0;
    }

    sqlite3_reset(stmt);
    if (rc == SQLITE_DONE)
    {
        tt_error_set("symbol not found: %s", id);
        return -1;
    }

    tt_error_set("index_store: get_symbol failed: %s", sqlite3_errmsg(store->db->db));
    return -1;
}

int tt_store_get_symbols_by_ids(tt_index_store_t *store, const char **ids, int count,
                                tt_symbol_result_t **out, int *out_count,
                                char ***errors, int *error_count)
{
    tt_array_t found;
    tt_array_init(&found);
    tt_array_t errs;
    tt_array_init(&errs);

    for (int i = 0; i < count; i++)
    {
        tt_symbol_result_t *r = malloc(sizeof(tt_symbol_result_t));
        if (!r)
            continue;

        if (tt_store_get_symbol(store, ids[i], r) == 0)
        {
            tt_array_push(&found, r);
        }
        else
        {
            free(r);
            char *msg = tt_strdup(ids[i]);
            if (msg)
                tt_array_push(&errs, msg);
        }
    }

    *out_count = (int)found.len;
    *out = malloc(sizeof(tt_symbol_result_t) * (found.len > 0 ? found.len : 1));
    if (*out)
    {
        for (size_t i = 0; i < found.len; i++)
        {
            tt_symbol_result_t *r = found.items[i];
            (*out)[i] = *r;
            free(r);
        }
    }
    tt_array_free(&found);

    *error_count = (int)errs.len;
    *errors = malloc(sizeof(char *) * (errs.len > 0 ? errs.len : 1));
    if (*errors)
    {
        for (size_t i = 0; i < errs.len; i++)
        {
            (*errors)[i] = errs.items[i];
        }
    }
    tt_array_free(&errs);

    return 0;
}

int tt_store_get_symbols_by_file(tt_index_store_t *store, const char *file_path,
                                 tt_symbol_result_t **out, int *out_count)
{
    tt_array_t results;
    tt_array_init(&results);

    sqlite3_stmt *stmt = store->get_syms_file;
    sqlite3_bind_text(stmt, 1, file_path, -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        tt_symbol_result_t *r = malloc(sizeof(tt_symbol_result_t));
        if (!r)
            break;
        memset(r, 0, sizeof(*r));
        read_symbol_row(stmt, &r->sym);
        r->score = 1.0;
        tt_array_push(&results, r);
    }
    sqlite3_reset(stmt);

    *out_count = (int)results.len;
    *out = malloc(sizeof(tt_symbol_result_t) * (results.len > 0 ? results.len : 1));
    if (*out)
    {
        for (size_t i = 0; i < results.len; i++)
        {
            tt_symbol_result_t *r = results.items[i];
            (*out)[i] = *r;
            free(r);
        }
    }
    tt_array_free(&results);
    return 0;
}

int tt_store_get_file(tt_index_store_t *store, const char *path, tt_file_record_t *out)
{
    memset(out, 0, sizeof(*out));

    sqlite3_stmt *stmt = store->get_file;
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
    {
        out->path = col_strdup(stmt, 0);
        out->hash = col_strdup(stmt, 1);
        out->language = col_strdup(stmt, 2);
        out->summary = col_strdup(stmt, 3);
        out->size_bytes = sqlite3_column_int64(stmt, 4);
        out->indexed_at = col_strdup(stmt, 5);
        sqlite3_reset(stmt);
        return 0;
    }

    sqlite3_reset(stmt);
    if (rc == SQLITE_DONE)
    {
        tt_error_set("file not found: %s", path);
        return -1;
    }

    tt_error_set("index_store: get_file failed: %s", sqlite3_errmsg(store->db->db));
    return -1;
}

int tt_store_get_file_hashes(tt_index_store_t *store, tt_hashmap_t **out)
{
    *out = tt_hashmap_new(256);
    if (!*out)
    {
        tt_error_set("index_store: hashmap alloc failed");
        return -1;
    }

    sqlite3_stmt *stmt = NULL;
    if (prepare(store->db->db, "SELECT path, hash FROM files", &stmt) < 0)
    {
        tt_hashmap_free(*out);
        *out = NULL;
        return -1;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char *path = (const char *)sqlite3_column_text(stmt, 0);
        const char *hash = (const char *)sqlite3_column_text(stmt, 1);
        if (path && hash)
        {
            char *hash_dup = tt_strdup(hash);
            void *old = tt_hashmap_set(*out, path, hash_dup);
            free(old);
        }
    }

    sqlite3_finalize(stmt);
    return 0;
}

int tt_store_get_all_files(tt_index_store_t *store, tt_file_record_t **out, int *count)
{
    tt_array_t results;
    tt_array_init(&results);

    sqlite3_stmt *stmt = NULL;
    if (prepare(store->db->db,
                "SELECT path, hash, language, summary, size_bytes, indexed_at FROM files ORDER BY path",
                &stmt) < 0)
    {
        return -1;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        tt_file_record_t *r = malloc(sizeof(tt_file_record_t));
        if (!r)
            break;
        memset(r, 0, sizeof(*r));
        r->path = col_strdup(stmt, 0);
        r->hash = col_strdup(stmt, 1);
        r->language = col_strdup(stmt, 2);
        r->summary = col_strdup(stmt, 3);
        r->size_bytes = sqlite3_column_int64(stmt, 4);
        r->indexed_at = col_strdup(stmt, 5);
        tt_array_push(&results, r);
    }
    sqlite3_finalize(stmt);

    *count = (int)results.len;
    *out = malloc(sizeof(tt_file_record_t) * (results.len > 0 ? results.len : 1));
    if (*out)
    {
        for (size_t i = 0; i < results.len; i++)
        {
            tt_file_record_t *r = results.items[i];
            (*out)[i] = *r;
            free(r);
        }
    }
    tt_array_free(&results);
    return 0;
}

/* Helper: query a name-count pair list */
static int query_name_count(sqlite3 *db, const char *sql,
                            tt_name_count_t **out, int *out_count)
{
    sqlite3_stmt *stmt = NULL;
    if (prepare(db, sql, &stmt) < 0)
        return -1;

    tt_array_t results;
    tt_array_init(&results);

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        tt_name_count_t *p = malloc(sizeof(tt_name_count_t));
        if (!p)
            break;
        p->name = col_strdup(stmt, 0);
        p->count = sqlite3_column_int(stmt, 1);
        tt_array_push(&results, p);
    }
    sqlite3_finalize(stmt);

    *out_count = (int)results.len;
    *out = malloc(sizeof(tt_name_count_t) * (results.len > 0 ? results.len : 1));
    if (*out)
    {
        for (size_t i = 0; i < results.len; i++)
        {
            tt_name_count_t *p = results.items[i];
            (*out)[i] = *p;
            free(p);
        }
    }
    tt_array_free(&results);
    return 0;
}

int tt_store_get_stats(tt_index_store_t *store, tt_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    sqlite3 *db = store->db->db;

    /* File count */
    sqlite3_stmt *stmt = NULL;
    if (prepare(db, "SELECT COUNT(*) FROM files", &stmt) == 0)
    {
        if (sqlite3_step(stmt) == SQLITE_ROW)
            out->files = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }

    /* Symbol count */
    if (prepare(db, "SELECT COUNT(*) FROM symbols", &stmt) == 0)
    {
        if (sqlite3_step(stmt) == SQLITE_ROW)
            out->symbols = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }

    /* Languages */
    query_name_count(db,
                     "SELECT language, COUNT(*) as cnt FROM symbols GROUP BY language ORDER BY cnt DESC",
                     &out->languages, &out->language_count);

    /* Kinds */
    query_name_count(db,
                     "SELECT kind, COUNT(*) as cnt FROM symbols GROUP BY kind ORDER BY cnt DESC",
                     &out->kinds, &out->kind_count);

    /* Directories */
    query_name_count(db,
                     "SELECT CASE "
                     "    WHEN INSTR(path, '/') > 0 THEN SUBSTR(path, 1, INSTR(path, '/')) "
                     "    ELSE path "
                     "END as dir, COUNT(*) as cnt "
                     "FROM files GROUP BY dir ORDER BY cnt DESC",
                     &out->dirs, &out->dir_count);

    return 0;
}

int tt_store_delete_symbols_by_file(tt_index_store_t *store, const char *file_path)
{
    sqlite3_stmt *stmt = store->delete_syms;
    sqlite3_bind_text(stmt, 1, file_path, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_reset(stmt);

    if (rc != SQLITE_DONE)
    {
        tt_error_set("index_store: delete_symbols_by_file failed: %s",
                     sqlite3_errmsg(store->db->db));
        return -1;
    }
    return 0;
}

int tt_store_delete_file(tt_index_store_t *store, const char *file_path)
{
    sqlite3_stmt *stmt = store->delete_file;
    sqlite3_bind_text(stmt, 1, file_path, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_reset(stmt);

    if (rc != SQLITE_DONE)
    {
        tt_error_set("index_store: delete_file failed: %s",
                     sqlite3_errmsg(store->db->db));
        return -1;
    }
    return 0;
}

int tt_store_truncate(tt_index_store_t *store)
{
    char *errmsg = NULL;
    int rc = sqlite3_exec(store->db->db,
                          "DELETE FROM symbols; DELETE FROM files;", NULL, NULL, &errmsg);
    if (rc != SQLITE_OK)
    {
        tt_error_set("index_store: truncate failed: %s", errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
        return -1;
    }
    return 0;
}

int tt_store_set_metadata(tt_index_store_t *store, const char *key, const char *value)
{
    sqlite3_stmt *stmt = store->set_meta;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_reset(stmt);

    if (rc != SQLITE_DONE)
    {
        tt_error_set("index_store: set_metadata failed: %s",
                     sqlite3_errmsg(store->db->db));
        return -1;
    }
    return 0;
}

char *tt_store_get_metadata(tt_index_store_t *store, const char *key)
{
    sqlite3_stmt *stmt = store->get_meta;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);

    char *result = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char *val = (const char *)sqlite3_column_text(stmt, 0);
        if (val)
            result = tt_strdup(val);
    }
    sqlite3_reset(stmt);
    return result;
}

int tt_store_update_symbol_summary(tt_index_store_t *store, const char *id,
                                   const char *summary)
{
    sqlite3_stmt *stmt = store->update_sym_summary;
    sqlite3_bind_text(stmt, 1, summary, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, id, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_reset(stmt);

    if (rc != SQLITE_DONE)
    {
        tt_error_set("index_store: update_symbol_summary failed: %s",
                     sqlite3_errmsg(store->db->db));
        return -1;
    }
    return 0;
}

int tt_store_update_file_summary(tt_index_store_t *store, const char *path,
                                 const char *summary)
{
    sqlite3_stmt *stmt = store->update_file_summary;
    sqlite3_bind_text(stmt, 1, summary, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, path, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_reset(stmt);

    if (rc != SQLITE_DONE)
    {
        tt_error_set("index_store: update_file_summary failed: %s",
                     sqlite3_errmsg(store->db->db));
        return -1;
    }
    return 0;
}

int tt_store_get_symbols_without_summary(tt_index_store_t *store, int limit,
                                         tt_symbol_result_t **out, int *count)
{
    tt_array_t results;
    tt_array_init(&results);

    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT id, file, name, qualified_name, kind, language, signature, docstring, "
             "summary, decorators, keywords, parent_id, line, end_line, byte_offset, "
             "byte_length, content_hash FROM symbols WHERE summary = '' LIMIT %d",
             limit);

    sqlite3_stmt *stmt = NULL;
    if (prepare(store->db->db, sql, &stmt) < 0)
        return -1;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        tt_symbol_result_t *r = malloc(sizeof(tt_symbol_result_t));
        if (!r)
            break;
        memset(r, 0, sizeof(*r));
        read_symbol_row(stmt, &r->sym);
        r->score = 1.0;
        tt_array_push(&results, r);
    }
    sqlite3_finalize(stmt);

    *count = (int)results.len;
    *out = malloc(sizeof(tt_symbol_result_t) * (results.len > 0 ? results.len : 1));
    if (*out)
    {
        for (size_t i = 0; i < results.len; i++)
        {
            tt_symbol_result_t *r = results.items[i];
            (*out)[i] = *r;
            free(r);
        }
    }
    tt_array_free(&results);
    return 0;
}

int tt_store_get_docstring_symbols(tt_index_store_t *store,
                                   tt_summary_input_t **out, int *count)
{
    *out = NULL;
    *count = 0;

    sqlite3_stmt *stmt = NULL;
    if (prepare(store->db->db,
                "SELECT id, docstring, kind, name FROM symbols "
                "WHERE summary = '' AND docstring != ''",
                &stmt) < 0)
        return -1;

    /* Count pass not needed — collect into dynamic array */
    tt_array_t results;
    tt_array_init(&results);

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        tt_summary_input_t *item = malloc(sizeof(tt_summary_input_t));
        if (!item)
            break;
        item->id = tt_strdup((const char *)sqlite3_column_text(stmt, 0));
        item->docstring = tt_strdup((const char *)sqlite3_column_text(stmt, 1));
        item->kind = tt_strdup((const char *)sqlite3_column_text(stmt, 2));
        item->name = tt_strdup((const char *)sqlite3_column_text(stmt, 3));
        tt_array_push(&results, item);
    }
    sqlite3_finalize(stmt);

    *count = (int)results.len;
    if (results.len > 0)
    {
        *out = malloc(sizeof(tt_summary_input_t) * results.len);
        if (*out)
        {
            for (size_t i = 0; i < results.len; i++)
            {
                tt_summary_input_t *item = results.items[i];
                (*out)[i] = *item;
                free(item);
            }
        }
    }
    tt_array_free(&results);
    return 0;
}

void tt_summary_input_free(tt_summary_input_t *items, int count)
{
    if (!items)
        return;
    for (int i = 0; i < count; i++)
    {
        free(items[i].id);
        free(items[i].docstring);
        free(items[i].kind);
        free(items[i].name);
    }
    free(items);
}

void tt_changes_fast_free(tt_changes_fast_t *c)
{
    if (!c)
        return;
    for (int i = 0; i < c->changed_count; i++)
        free(c->changed[i]);
    free(c->changed);
    for (int i = 0; i < c->added_count; i++)
        free(c->added[i]);
    free(c->added);
    for (int i = 0; i < c->deleted_count; i++)
        free(c->deleted[i]);
    free(c->deleted);
    for (int i = 0; i < c->metadata_changed_count; i++)
        free(c->metadata_changed[i]);
    free(c->metadata_changed);
    memset(c, 0, sizeof(*c));
}

int tt_store_detect_changes_fast(tt_index_store_t *store,
                                 const char *project_root,
                                 const char **discovered_paths,
                                 int path_count,
                                 tt_changes_fast_t *out)
{
    memset(out, 0, sizeof(*out));

    /* Load all stored file records: {path -> (hash, mtime_sec, mtime_nsec, size_bytes)} */
    sqlite3_stmt *stmt = NULL;
    if (prepare(store->db->db,
                "SELECT path, hash, mtime_sec, mtime_nsec, size_bytes FROM files",
                &stmt) < 0)
    {
        return -1;
    }

    /* Build hashmap of DB records keyed by path */
    tt_hashmap_t *db_files = tt_hashmap_new(4096);
    if (!db_files)
    {
        sqlite3_finalize(stmt);
        return -1;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char *path = (const char *)sqlite3_column_text(stmt, 0);
        const char *hash = (const char *)sqlite3_column_text(stmt, 1);
        int64_t mt_sec = sqlite3_column_int64(stmt, 2);
        int64_t mt_nsec = sqlite3_column_int64(stmt, 3);
        int64_t sz = sqlite3_column_int64(stmt, 4);
        if (!path)
            continue;

        /* Pack metadata into a single string: "hash|mtime_sec|mtime_nsec|size_bytes" */
        char meta[512];
        snprintf(meta, sizeof(meta), "%s|%lld|%lld|%lld",
                 hash ? hash : "",
                 (long long)mt_sec, (long long)mt_nsec, (long long)sz);
        tt_hashmap_set(db_files, path, tt_strdup(meta));
    }
    sqlite3_finalize(stmt);

    tt_array_t changed, added, metadata_changed;
    tt_array_init(&changed);
    tt_array_init(&added);
    tt_array_init(&metadata_changed);

    /* Build a set of discovered paths for deleted detection */
    tt_hashmap_t *disc_set = tt_hashmap_new(path_count > 0 ? (size_t)path_count * 2 : 64);

    for (int i = 0; i < path_count; i++)
    {
        const char *relpath = discovered_paths[i];
        tt_hashmap_set(disc_set, relpath, (void *)(intptr_t)1);

        const char *meta_str = (const char *)tt_hashmap_get(db_files, relpath);
        if (!meta_str)
        {
            /* Not in DB: added */
            tt_array_push(&added, tt_strdup(relpath));
            continue;
        }

        /* Parse stored metadata */
        char stored_hash[256] = {0};
        long long stored_mt_sec = 0, stored_mt_nsec = 0, stored_sz = 0;
        sscanf(meta_str, "%255[^|]|%lld|%lld|%lld",
               stored_hash, &stored_mt_sec, &stored_mt_nsec, &stored_sz);

        /* stat() the file */
        char *full = tt_path_join(project_root, relpath);
        if (!full)
        {
            tt_array_push(&added, tt_strdup(relpath));
            continue;
        }

        struct stat st;
        if (stat(full, &st) < 0)
        {
            free(full);
            tt_array_push(&added, tt_strdup(relpath));
            continue;
        }

        int64_t cur_mt_sec = TT_ST_MTIME_SEC(st);
        int64_t cur_mt_nsec = TT_ST_MTIME_NSEC(st);
        int64_t cur_sz = (int64_t)st.st_size;

        /* Fast path: metadata matches -> unchanged */
        if (cur_mt_sec == stored_mt_sec &&
            cur_mt_nsec == stored_mt_nsec &&
            cur_sz == stored_sz)
        {
            free(full);
            continue;
        }

        /* Metadata differs: read file and compute hash */
        char *cur_hash = tt_fast_hash_file(full);
        free(full);

        if (!cur_hash)
        {
            tt_array_push(&changed, tt_strdup(relpath));
            continue;
        }

        if (strcmp(cur_hash, stored_hash) == 0)
        {
            /* Same content, just metadata changed */
            tt_array_push(&metadata_changed, tt_strdup(relpath));
        }
        else
        {
            /* Real content change */
            tt_array_push(&changed, tt_strdup(relpath));
        }
        free(cur_hash);
    }

    /* Find deleted files: in DB but not in discovered set */
    tt_array_t deleted;
    tt_array_init(&deleted);

    sqlite3_stmt *all_stmt = NULL;
    if (prepare(store->db->db, "SELECT path FROM files", &all_stmt) == 0)
    {
        while (sqlite3_step(all_stmt) == SQLITE_ROW)
        {
            const char *db_path = (const char *)sqlite3_column_text(all_stmt, 0);
            if (db_path && !tt_hashmap_get(disc_set, db_path))
            {
                tt_array_push(&deleted, tt_strdup(db_path));
            }
        }
        sqlite3_finalize(all_stmt);
    }

    /* Transfer results */
    out->changed_count = (int)changed.len;
    out->changed = (char **)changed.items;
    changed.items = NULL;

    out->added_count = (int)added.len;
    out->added = (char **)added.items;
    added.items = NULL;

    out->deleted_count = (int)deleted.len;
    out->deleted = (char **)deleted.items;
    deleted.items = NULL;

    out->metadata_changed_count = (int)metadata_changed.len;
    out->metadata_changed = (char **)metadata_changed.items;
    metadata_changed.items = NULL;

    tt_hashmap_free_with_values(db_files);
    tt_hashmap_free(disc_set);
    return 0;
}

int tt_store_update_file_metadata(tt_index_store_t *store,
                                  const char *path,
                                  int64_t mtime_sec, int64_t mtime_nsec,
                                  int64_t size_bytes)
{
    sqlite3_stmt *stmt = NULL;
    if (prepare(store->db->db,
                "UPDATE files SET mtime_sec = ?, mtime_nsec = ?, size_bytes = ? WHERE path = ?",
                &stmt) < 0)
    {
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, mtime_sec);
    sqlite3_bind_int64(stmt, 2, mtime_nsec);
    sqlite3_bind_int64(stmt, 3, size_bytes);
    sqlite3_bind_text(stmt, 4, path, -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE)
    {
        tt_error_set("index_store: update_file_metadata failed: %s",
                     sqlite3_errmsg(store->db->db));
        return -1;
    }
    return 0;
}

int tt_store_generate_file_summaries(tt_index_store_t *store)
{
    sqlite3_stmt *stmt = NULL;
    if (prepare(store->db->db,
                "SELECT file, kind, COUNT(*) as cnt FROM symbols "
                "GROUP BY file, kind ORDER BY file, cnt DESC",
                &stmt) < 0)
    {
        return -1;
    }

    tt_database_begin(store->db);

    char *current_file = NULL;
    tt_strbuf_t summary;
    tt_strbuf_init(&summary);
    int part_count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char *file = (const char *)sqlite3_column_text(stmt, 0);
        const char *kind = (const char *)sqlite3_column_text(stmt, 1);
        int cnt = sqlite3_column_int(stmt, 2);

        if (!file || !kind)
            continue;

        /* New file: flush previous */
        if (!current_file || strcmp(current_file, file) != 0)
        {
            if (current_file && part_count > 0)
            {
                tt_store_update_file_summary(store, current_file, summary.data);
            }

            free(current_file);
            current_file = tt_strdup(file);
            tt_strbuf_reset(&summary);
            part_count = 0;
        }

        /* Append "N kind[s]" */
        if (part_count > 0)
        {
            tt_strbuf_append_str(&summary, ", ");
        }

        /* Pluralization: count == 1 → singular, count > 1 → add 's' */
        if (cnt == 1)
        {
            tt_strbuf_appendf(&summary, "1 %s", kind);
        }
        else
        {
            /* Special pluralization for "class" → "classes" */
            size_t klen = strlen(kind);
            if (klen > 0 && kind[klen - 1] == 's')
            {
                tt_strbuf_appendf(&summary, "%d %ses", cnt, kind);
            }
            else
            {
                tt_strbuf_appendf(&summary, "%d %ss", cnt, kind);
            }
        }
        part_count++;
    }

    /* Flush last file */
    if (current_file && part_count > 0)
    {
        tt_store_update_file_summary(store, current_file, summary.data);
    }

    free(current_file);
    tt_strbuf_free(&summary);
    sqlite3_finalize(stmt);

    tt_database_commit(store->db);
    return 0;
}

/* ==== Import graph CRUD ==== */

int tt_store_insert_imports(tt_index_store_t *store, const tt_import_t *imps, int count)
{
    if (count <= 0)
        return 0;

    sqlite3_stmt *stmt = NULL;
    if (prepare(store->db->db,
                "INSERT INTO imports (source_file, target_name, kind, import_type) "
                "VALUES (?, ?, ?, ?)",
                &stmt) < 0)
        return -1;

    /* Use SAVEPOINT so we don't clobber an outer transaction */
    bool own_txn = !store->db->in_transaction;
    if (own_txn)
        sqlite3_exec(store->db->db, "BEGIN", NULL, NULL, NULL);

    for (int i = 0; i < count; i++)
    {
        sqlite3_bind_text(stmt, 1, imps[i].from_file ? imps[i].from_file : "", -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, imps[i].to_specifier ? imps[i].to_specifier : "", -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, imps[i].symbol_name ? imps[i].symbol_name : "module", -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, imps[i].import_type ? imps[i].import_type : "import", -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }

    if (own_txn)
        sqlite3_exec(store->db->db, "COMMIT", NULL, NULL, NULL);
    sqlite3_finalize(stmt);
    return 0;
}

int tt_store_delete_imports_for_file(tt_index_store_t *store, const char *file_path)
{
    sqlite3_stmt *stmt = NULL;
    if (prepare(store->db->db,
                "DELETE FROM imports WHERE source_file = ?", &stmt) < 0)
        return -1;

    sqlite3_bind_text(stmt, 1, file_path, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE)
    {
        tt_error_set("index_store: delete_imports_for_file failed: %s",
                     sqlite3_errmsg(store->db->db));
        return -1;
    }
    return 0;
}

/* ==== Import resolution ==== */

/* Common file extensions to try when resolving import specifiers.
 * Grouped by language family. Order matters: try most likely first. */
static const char *RESOLVE_EXTENSIONS[] = {
    ".php",
    ".js", ".jsx", ".ts", ".tsx", ".mjs", ".cjs", ".mts", ".vue",
    ".py",
    ".java", ".kt", ".scala",
    ".rb",
    ".go",
    ".rs",
    ".cs",
    ".swift",
    ".hs",
    ".c", ".h", ".cpp", ".hpp", ".cc",
    NULL
};

/* Normalize an import specifier by converting language-specific separators
 * to forward slashes. Writes result into buf (must be >= len+1).
 * Returns the length written (excluding NUL). */
static size_t normalize_specifier(const char *spec, char *buf, size_t bufsize)
{
    size_t i = 0;
    bool has_slash = (strchr(spec, '/') != NULL);

    for (; spec[i] && i < bufsize - 1; i++)
    {
        char c = spec[i];
        if (c == '\\')
            buf[i] = '/'; /* PHP namespaces */
        else if (c == ':' && spec[i + 1] == ':')
        {
            buf[i] = '/'; /* Rust :: */
            i++;
            /* skip second colon */
            continue;
        }
        else if (c == '.' && !has_slash && i > 0 && spec[i - 1] != '.' &&
                 spec[i + 1] && spec[i + 1] != '.' && spec[i + 1] != '/')
        {
            /* Python/Java dot separator — only if no slashes exist in spec
             * and not a file extension (preceded by path component).
             * Skip if it looks like a file extension (e.g., "app.types"). */
            const char *rest = spec + i + 1;
            bool looks_like_ext = true;
            for (const char *r = rest; *r; r++)
            {
                if (*r == '.')
                {
                    looks_like_ext = false;
                    break;
                }
            }
            /* If there are more dots ahead, treat this as a module separator */
            if (!looks_like_ext)
                buf[i] = '/';
            else
                buf[i] = '/'; /* treat as separator anyway for resolution */
        }
        else
        {
            buf[i] = c;
        }
    }
    buf[i] = '\0';
    return i;
}

/* Resolve a relative path (starting with ./ or ../) against a source file.
 * Returns a heap-allocated resolved path, or NULL on failure. */
static char *resolve_relative(const char *source_file, const char *target)
{
    /* Find directory of source_file */
    const char *last_slash = strrchr(source_file, '/');
    size_t dir_len = last_slash ? (size_t)(last_slash - source_file) : 0;

    /* Build initial path: dir/target */
    size_t tlen = strlen(target);
    size_t bufsize = dir_len + 1 + tlen + 1;
    char *result = malloc(bufsize);
    if (!result) return NULL;

    if (dir_len > 0)
    {
        memcpy(result, source_file, dir_len);
        result[dir_len] = '/';
        memcpy(result + dir_len + 1, target, tlen + 1);
    }
    else
    {
        memcpy(result, target, tlen + 1);
    }

    /* Simplify: resolve . and .. components in-place */
    int nparts = 0;
    int part_cap = 32;
    char **parts = malloc((size_t)part_cap * sizeof(char *));
    if (!parts) { free(result); return NULL; }

    char *p = result;
    char *tok = strtok(p, "/");
    while (tok)
    {
        if (strcmp(tok, ".") == 0)
        {
            /* skip */
        }
        else if (strcmp(tok, "..") == 0)
        {
            if (nparts > 0) nparts--;
        }
        else
        {
            if (nparts >= part_cap)
            {
                part_cap *= 2;
                char **tmp = realloc(parts, (size_t)part_cap * sizeof(char *));
                if (!tmp) { free(parts); free(result); return NULL; }
                parts = tmp;
            }
            parts[nparts++] = tok;
        }
        tok = strtok(NULL, "/");
    }

    /* Reconstruct path */
    size_t total = 0;
    for (int i = 0; i < nparts; i++)
        total += strlen(parts[i]) + 1;

    char *resolved = malloc(total + 1);
    if (!resolved) { free(parts); free(result); return NULL; }

    size_t pos = 0;
    for (int i = 0; i < nparts; i++)
    {
        if (i > 0) resolved[pos++] = '/';
        size_t slen = strlen(parts[i]);
        memcpy(resolved + pos, parts[i], slen);
        pos += slen;
    }
    resolved[pos] = '\0';

    free(parts);
    free(result);
    return resolved;
}

int tt_store_resolve_imports(tt_index_store_t *store)
{
    if (!store || !store->db) return -1;
    sqlite3 *db = store->db->db;

    /* Step 1: Load all file paths into two hashmaps:
     * - exact_map: path → path (exact lookup)
     * - suffix_map: lowercased suffix → path (suffix matching)
     *   If collision, value is set to "" to mark ambiguous. */
    sqlite3_stmt *file_stmt = NULL;
    if (prepare(db, "SELECT path FROM files", &file_stmt) < 0)
        return -1;

    int file_count = 0, file_cap = 1024;
    char **file_paths = malloc((size_t)file_cap * sizeof(char *));
    if (!file_paths) { sqlite3_finalize(file_stmt); return -1; }

    while (sqlite3_step(file_stmt) == SQLITE_ROW)
    {
        const char *p = (const char *)sqlite3_column_text(file_stmt, 0);
        if (!p) continue;
        if (file_count >= file_cap)
        {
            file_cap *= 2;
            char **tmp = realloc(file_paths, (size_t)file_cap * sizeof(char *));
            if (!tmp) break;
            file_paths = tmp;
        }
        file_paths[file_count++] = tt_strdup(p);
    }
    sqlite3_finalize(file_stmt);

    /* Build exact path lookup */
    tt_hashmap_t *exact_map = tt_hashmap_new(
        file_count > 16 ? (size_t)file_count * 2 : 32);

    /* Build suffix map: for each file, generate suffixes (both with and
     * without extension) in lowercase. Value = original path, or "" if ambiguous. */
    tt_hashmap_t *suffix_map = tt_hashmap_new(
        file_count > 16 ? (size_t)file_count * 8 : 64);

    /* Build dir_map: directory suffix (lowered) → first file in that dir.
     * Used for Go module-path and C# namespace resolution. */
    tt_hashmap_t *dir_map = tt_hashmap_new(
        file_count > 16 ? (size_t)file_count * 4 : 32);

    for (int i = 0; i < file_count; i++)
    {
        const char *path = file_paths[i];
        tt_hashmap_set(exact_map, path, file_paths[i]);

        size_t plen = strlen(path);

        /* Find extension offset */
        const char *ext = strrchr(path, '.');
        const char *last_slash = strrchr(path, '/');
        if (ext && last_slash && ext < last_slash) ext = NULL; /* dot in dir name */
        size_t stem_len = ext ? (size_t)(ext - path) : plen;

        /* Generate suffixes by stripping leading path components */
        for (size_t s = 0; s < plen; s++)
        {
            if (s > 0 && path[s - 1] != '/') continue;
            const char *suffix = path + s;
            size_t slen = plen - s;

            /* Lowercase suffix for case-insensitive matching */
            char *lower = malloc(slen + 1);
            if (!lower) continue;
            for (size_t j = 0; j < slen; j++)
                lower[j] = (char)tolower((unsigned char)suffix[j]);
            lower[slen] = '\0';

            /* Store with extension */
            if (tt_hashmap_has(suffix_map, lower))
            {
                /* Ambiguous: mark with empty string */
                tt_hashmap_set(suffix_map, lower, (void *)"");
            }
            else
            {
                tt_hashmap_set(suffix_map, lower, file_paths[i]);
            }

            /* Store without extension (if applicable) */
            if (ext && s < stem_len)
            {
                size_t no_ext_len = stem_len - s;
                char *no_ext = malloc(no_ext_len + 1);
                if (no_ext)
                {
                    for (size_t j = 0; j < no_ext_len; j++)
                        no_ext[j] = (char)tolower((unsigned char)suffix[j]);
                    no_ext[no_ext_len] = '\0';

                    if (tt_hashmap_has(suffix_map, no_ext))
                        tt_hashmap_set(suffix_map, no_ext, (void *)"");
                    else
                        tt_hashmap_set(suffix_map, no_ext, file_paths[i]);
                    free(no_ext);
                }
            }

            free(lower);
        }

        /* Populate dir_map: directory suffixes for this file */
        const char *last_sl = strrchr(path, '/');
        if (last_sl)
        {
            size_t dir_len = (size_t)(last_sl - path);
            for (size_t s = 0; s <= dir_len; s++)
            {
                if (s > 0 && path[s - 1] != '/') continue;
                size_t dslen = dir_len - s;
                if (dslen == 0) continue;
                char *lower_dir = malloc(dslen + 1);
                if (!lower_dir) continue;
                for (size_t j = 0; j < dslen; j++)
                    lower_dir[j] = (char)tolower((unsigned char)path[s + j]);
                lower_dir[dslen] = '\0';

                if (tt_hashmap_has(dir_map, lower_dir))
                    tt_hashmap_set(dir_map, lower_dir, (void *)"");
                else
                    tt_hashmap_set(dir_map, lower_dir, file_paths[i]);
                free(lower_dir);
            }
        }
    }

    /* Step 2: Read all unresolved imports */
    sqlite3_stmt *imp_stmt = NULL;
    if (prepare(db,
                "SELECT id, source_file, target_name FROM imports "
                "WHERE resolved_file = ''",
                &imp_stmt) < 0)
    {
        tt_hashmap_free(exact_map);
        tt_hashmap_free(suffix_map);
        tt_hashmap_free(dir_map);
        for (int i = 0; i < file_count; i++) free(file_paths[i]);
        free(file_paths);
        return -1;
    }

    /* Prepare update statement */
    sqlite3_stmt *upd_stmt = NULL;
    if (prepare(db,
                "UPDATE imports SET resolved_file = ? WHERE id = ?",
                &upd_stmt) < 0)
    {
        sqlite3_finalize(imp_stmt);
        tt_hashmap_free(exact_map);
        tt_hashmap_free(suffix_map);
        tt_hashmap_free(dir_map);
        for (int i = 0; i < file_count; i++) free(file_paths[i]);
        free(file_paths);
        return -1;
    }

    sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);

    int resolved_count = 0;
    while (sqlite3_step(imp_stmt) == SQLITE_ROW)
    {
        int imp_id = sqlite3_column_int(imp_stmt, 0);
        const char *source = (const char *)sqlite3_column_text(imp_stmt, 1);
        const char *target = (const char *)sqlite3_column_text(imp_stmt, 2);
        if (!target || !target[0]) continue;

        const char *resolved = NULL;

        /* Strategy 1: Direct exact match (C/C++ includes, etc.) */
        if (tt_hashmap_has(exact_map, target))
        {
            resolved = (const char *)tt_hashmap_get(exact_map, target);
        }

        /* Strategy 2: Relative path resolution (JS/TS/Ruby ./ ../) */
        char *rel_resolved = NULL;
        if (!resolved && source && (target[0] == '.' && (target[1] == '/' ||
            (target[1] == '.' && target[2] == '/'))))
        {
            rel_resolved = resolve_relative(source, target);
            if (rel_resolved)
            {
                /* Try exact match */
                if (tt_hashmap_has(exact_map, rel_resolved))
                {
                    resolved = (const char *)tt_hashmap_get(exact_map, rel_resolved);
                }
                else
                {
                    /* Try with extensions */
                    size_t rlen = strlen(rel_resolved);
                    char *probe = malloc(rlen + 8);
                    if (probe)
                    {
                        for (int e = 0; RESOLVE_EXTENSIONS[e] && !resolved; e++)
                        {
                            size_t elen = strlen(RESOLVE_EXTENSIONS[e]);
                            memcpy(probe, rel_resolved, rlen);
                            memcpy(probe + rlen, RESOLVE_EXTENSIONS[e], elen + 1);
                            if (tt_hashmap_has(exact_map, probe))
                                resolved = (const char *)tt_hashmap_get(exact_map, probe);
                        }
                        /* Try /index.{ext} */
                        if (!resolved)
                        {
                            char *idx_probe = malloc(rlen + 16);
                            if (idx_probe)
                            {
                                static const char *idx_exts[] = {
                                    "/index.ts", "/index.js", "/index.tsx",
                                    "/index.jsx", "/index.py", NULL
                                };
                                for (int e = 0; idx_exts[e] && !resolved; e++)
                                {
                                    size_t elen = strlen(idx_exts[e]);
                                    memcpy(idx_probe, rel_resolved, rlen);
                                    memcpy(idx_probe + rlen, idx_exts[e], elen + 1);
                                    if (tt_hashmap_has(exact_map, idx_probe))
                                        resolved = (const char *)tt_hashmap_get(exact_map, idx_probe);
                                }
                                free(idx_probe);
                            }
                        }
                        free(probe);
                    }
                }
            }
        }

        /* Strategy 2.5: Same-directory resolution (C/C++ #include "file.h")
         * For targets without path separators, try resolving relative to the
         * source file's directory. */
        if (!resolved && source && !strchr(target, '/') && !strchr(target, '\\'))
        {
            const char *src_slash = strrchr(source, '/');
            if (src_slash)
            {
                size_t dir_len = (size_t)(src_slash - source) + 1;
                size_t tlen = strlen(target);
                char *same_dir = malloc(dir_len + tlen + 1);
                if (same_dir)
                {
                    memcpy(same_dir, source, dir_len);
                    memcpy(same_dir + dir_len, target, tlen + 1);
                    if (tt_hashmap_has(exact_map, same_dir))
                        resolved = (const char *)tt_hashmap_get(exact_map, same_dir);
                    if (!resolved)
                    {
                        /* Try with extensions */
                        char *probe = malloc(dir_len + tlen + 8);
                        if (probe)
                        {
                            for (int e = 0; RESOLVE_EXTENSIONS[e] && !resolved; e++)
                            {
                                size_t elen = strlen(RESOLVE_EXTENSIONS[e]);
                                memcpy(probe, same_dir, dir_len + tlen);
                                memcpy(probe + dir_len + tlen, RESOLVE_EXTENSIONS[e], elen + 1);
                                if (tt_hashmap_has(exact_map, probe))
                                    resolved = (const char *)tt_hashmap_get(exact_map, probe);
                            }
                            free(probe);
                        }
                    }
                    free(same_dir);
                }
            }
        }

        /* Strategy 3: Normalize separators + suffix match */
        if (!resolved)
        {
            char normalized[512];
            normalize_specifier(target, normalized, sizeof(normalized));
            size_t nlen = strlen(normalized);

            /* Lowercase for suffix map lookup */
            char lower[512];
            for (size_t j = 0; j <= nlen && j < sizeof(lower) - 1; j++)
                lower[j] = (char)tolower((unsigned char)normalized[j]);
            lower[nlen < sizeof(lower) - 1 ? nlen : sizeof(lower) - 1] = '\0';

            /* Try suffix map (without extension) */
            if (tt_hashmap_has(suffix_map, lower))
            {
                const char *v = (const char *)tt_hashmap_get(suffix_map, lower);
                if (v && v[0]) resolved = v; /* non-empty = unambiguous */
            }

            /* Try suffix map with extensions */
            if (!resolved)
            {
                char probe[520];
                for (int e = 0; RESOLVE_EXTENSIONS[e] && !resolved; e++)
                {
                    size_t elen = strlen(RESOLVE_EXTENSIONS[e]);
                    if (nlen + elen >= sizeof(probe)) continue;
                    memcpy(probe, lower, nlen);
                    for (size_t j = 0; j <= elen; j++)
                        probe[nlen + j] = (char)tolower((unsigned char)RESOLVE_EXTENSIONS[e][j]);
                    if (tt_hashmap_has(suffix_map, probe))
                    {
                        const char *v = (const char *)tt_hashmap_get(suffix_map, probe);
                        if (v && v[0]) resolved = v;
                    }
                }
            }

            /* Try __init__.py for Python modules */
            if (!resolved && nlen + 13 < sizeof(lower))
            {
                char py_init[530];
                snprintf(py_init, sizeof(py_init), "%s/__init__.py", lower);
                if (tt_hashmap_has(suffix_map, py_init))
                {
                    const char *v = (const char *)tt_hashmap_get(suffix_map, py_init);
                    if (v && v[0]) resolved = v;
                }
            }

            /* Strategy 3b: Progressive prefix stripping.
             * For namespace/package imports (PHP: App\Service\UserService,
             * Java: com.example.service.OrderService), the full normalized
             * path doesn't match but a suffix does. Strip leading components
             * progressively and retry. */
            if (!resolved)
            {
                for (size_t s = 0; s < nlen && !resolved; s++)
                {
                    if (lower[s] != '/') continue;
                    const char *sub = lower + s + 1;
                    if (!*sub) continue;
                    size_t sub_len = nlen - s - 1;

                    /* Try suffix map (without extension) */
                    if (tt_hashmap_has(suffix_map, sub))
                    {
                        const char *v = (const char *)tt_hashmap_get(suffix_map, sub);
                        if (v && v[0]) { resolved = v; break; }
                    }

                    /* Try with extensions */
                    char probe[520];
                    for (int e = 0; RESOLVE_EXTENSIONS[e] && !resolved; e++)
                    {
                        size_t elen = strlen(RESOLVE_EXTENSIONS[e]);
                        if (sub_len + elen >= sizeof(probe)) continue;
                        memcpy(probe, sub, sub_len);
                        for (size_t j = 0; j <= elen; j++)
                            probe[sub_len + j] = (char)tolower(
                                (unsigned char)RESOLVE_EXTENSIONS[e][j]);
                        if (tt_hashmap_has(suffix_map, probe))
                        {
                            const char *v = (const char *)tt_hashmap_get(
                                suffix_map, probe);
                            if (v && v[0]) { resolved = v; break; }
                        }
                    }
                }
            }
        }

        /* Strategy 4: Directory-level matching (Go module paths, C# namespaces).
         * If target resolves to a directory rather than a file, map it to a file
         * in that directory. Tries the raw target first, then normalized, then
         * progressive prefix stripping on both. */
        if (!resolved)
        {
            /* Try raw target lowered */
            size_t tlen = strlen(target);
            char raw_lower[512];
            for (size_t j = 0; j < tlen && j < sizeof(raw_lower) - 1; j++)
                raw_lower[j] = (char)tolower((unsigned char)target[j]);
            raw_lower[tlen < sizeof(raw_lower) - 1 ? tlen : sizeof(raw_lower) - 1] = '\0';

            if (tt_hashmap_has(dir_map, raw_lower))
            {
                const char *v = (const char *)tt_hashmap_get(dir_map, raw_lower);
                if (v && v[0]) resolved = v;
            }

            /* Try normalized target as directory */
            if (!resolved)
            {
                char normalized[512];
                normalize_specifier(target, normalized, sizeof(normalized));
                size_t nlen2 = strlen(normalized);
                char lower2[512];
                for (size_t j = 0; j < nlen2 && j < sizeof(lower2) - 1; j++)
                    lower2[j] = (char)tolower((unsigned char)normalized[j]);
                lower2[nlen2 < sizeof(lower2) - 1 ? nlen2 : sizeof(lower2) - 1] = '\0';

                if (tt_hashmap_has(dir_map, lower2))
                {
                    const char *v = (const char *)tt_hashmap_get(dir_map, lower2);
                    if (v && v[0]) resolved = v;
                }

                /* Progressive prefix stripping on directory map */
                if (!resolved)
                {
                    for (size_t s = 0; s < nlen2 && !resolved; s++)
                    {
                        if (lower2[s] != '/') continue;
                        const char *sub = lower2 + s + 1;
                        if (!*sub) continue;
                        if (tt_hashmap_has(dir_map, sub))
                        {
                            const char *v = (const char *)tt_hashmap_get(dir_map, sub);
                            if (v && v[0]) { resolved = v; break; }
                        }
                    }
                }
            }
        }

        /* Update resolved_file if we found a match */
        if (resolved && resolved[0])
        {
            sqlite3_bind_text(upd_stmt, 1, resolved, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(upd_stmt, 2, imp_id);
            sqlite3_step(upd_stmt);
            sqlite3_reset(upd_stmt);
            resolved_count++;
        }

        free(rel_resolved);
    }

    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);

    sqlite3_finalize(imp_stmt);
    sqlite3_finalize(upd_stmt);
    tt_hashmap_free(exact_map);
    tt_hashmap_free(suffix_map);
    tt_hashmap_free(dir_map);
    for (int i = 0; i < file_count; i++) free(file_paths[i]);
    free(file_paths);

    return 0;
}

static tt_import_t *read_import_rows(sqlite3_stmt *stmt, int *out_count)
{
    tt_array_t results;
    tt_array_init(&results);

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        tt_import_t *imp = calloc(1, sizeof(tt_import_t));
        if (!imp)
            break;
        imp->from_file = col_strdup(stmt, 0);
        imp->to_specifier = col_strdup(stmt, 1);
        imp->symbol_name = col_strdup_nullable(stmt, 2);
        imp->import_type = col_strdup(stmt, 3);
        tt_array_push(&results, imp);
    }

    *out_count = (int)results.len;
    tt_import_t *arr = calloc(results.len > 0 ? results.len : 1, sizeof(tt_import_t));
    if (arr)
    {
        for (size_t i = 0; i < results.len; i++)
        {
            tt_import_t *imp = results.items[i];
            arr[i] = *imp;
            free(imp);
        }
    }
    tt_array_free(&results);
    return arr;
}

int tt_store_get_importers(tt_index_store_t *store, const char *file_path,
                           tt_import_t **out, int *out_count)
{
    sqlite3_stmt *stmt = NULL;
    if (prepare(store->db->db,
                "SELECT source_file, target_name, kind, import_type "
                "FROM imports WHERE resolved_file = ?",
                &stmt) < 0)
        return -1;

    sqlite3_bind_text(stmt, 1, file_path, -1, SQLITE_TRANSIENT);

    *out = read_import_rows(stmt, out_count);
    sqlite3_finalize(stmt);
    return 0;
}

int tt_store_count_importers(tt_index_store_t *store, const char *file_path)
{
    if (!store || !store->db || !file_path || !file_path[0]) return 0;

    sqlite3_stmt *stmt = NULL;
    if (prepare(store->db->db,
                "SELECT COUNT(*) FROM imports WHERE resolved_file = ?",
                &stmt) < 0)
        return 0;

    sqlite3_bind_text(stmt, 1, file_path, -1, SQLITE_TRANSIENT);

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

int tt_store_get_imports_of(tt_index_store_t *store, const char *file_path,
                             char ***out_files, int *out_count)
{
    *out_files = NULL;
    *out_count = 0;
    if (!store || !store->db || !file_path || !file_path[0]) return 0;

    sqlite3_stmt *stmt = NULL;
    if (prepare(store->db->db,
                "SELECT DISTINCT resolved_file FROM imports "
                "WHERE source_file = ? AND resolved_file != ''",
                &stmt) < 0)
        return -1;

    sqlite3_bind_text(stmt, 1, file_path, -1, SQLITE_TRANSIENT);

    int cap = 32, n = 0;
    char **files = malloc((size_t)cap * sizeof(char *));
    if (!files) { sqlite3_finalize(stmt); return -1; }

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char *t = (const char *)sqlite3_column_text(stmt, 0);
        if (!t) continue;
        if (n >= cap)
        {
            cap *= 2;
            char **tmp = realloc(files, (size_t)cap * sizeof(char *));
            if (!tmp) break;
            files = tmp;
        }
        files[n++] = tt_strdup(t);
    }
    sqlite3_finalize(stmt);

    *out_files = files;
    *out_count = n;
    return 0;
}

int tt_store_find_references(tt_index_store_t *store, const char *identifier,
                             tt_import_t **out, int *out_count)
{
    sqlite3_stmt *stmt = NULL;
    if (prepare(store->db->db,
                "SELECT source_file, target_name, kind, import_type "
                "FROM imports WHERE kind LIKE ? OR target_name LIKE ?",
                &stmt) < 0)
        return -1;

    char like_pattern[512];
    snprintf(like_pattern, sizeof(like_pattern), "%%%s%%", identifier);
    sqlite3_bind_text(stmt, 1, like_pattern, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, like_pattern, -1, SQLITE_TRANSIENT);

    *out = read_import_rows(stmt, out_count);
    sqlite3_finalize(stmt);
    return 0;
}

/* ---- Hierarchy (inspect:hierarchy) ---- */

void tt_hierarchy_node_free(tt_hierarchy_node_t *items, int count)
{
    if (!items)
        return;
    for (int i = 0; i < count; i++)
    {
        free(items[i].id);
        free(items[i].file);
        free(items[i].name);
        free(items[i].qualified_name);
        free(items[i].kind);
        free(items[i].parent_id);
    }
    free(items);
}

int tt_store_get_hierarchy(tt_index_store_t *store,
                           const char *file_path, const char *language,
                           int limit,
                           tt_hierarchy_node_t **out, int *out_count)
{
    *out = NULL;
    *out_count = 0;

    tt_strbuf_t sql;
    tt_strbuf_init_cap(&sql, 256);

    bool file_mode = file_path && file_path[0] &&
                     (strchr(file_path, '/') || strchr(file_path, '.'));

    if (file_mode)
    {
        tt_strbuf_append_str(&sql,
                             "SELECT id, file, name, qualified_name, kind, parent_id, line, end_line "
                             "FROM symbols WHERE file = ? ORDER BY line");
    }
    else
    {
        tt_strbuf_append_str(&sql,
                             "SELECT id, file, name, qualified_name, kind, parent_id, line, end_line "
                             "FROM symbols WHERE kind IN ('class','interface','trait','enum')");
        if (language && language[0])
            tt_strbuf_append_str(&sql, " AND language = ?");
        tt_strbuf_append_str(&sql, " ORDER BY file, line");
        if (limit > 0)
        {
            char buf[32];
            snprintf(buf, sizeof(buf), " LIMIT %d", limit);
            tt_strbuf_append_str(&sql, buf);
        }
    }

    sqlite3_stmt *stmt = NULL;
    if (prepare(store->db->db, sql.data, &stmt) < 0)
    {
        tt_strbuf_free(&sql);
        return -1;
    }
    tt_strbuf_free(&sql);

    int bind_idx = 1;
    if (file_mode)
        sqlite3_bind_text(stmt, bind_idx++, file_path, -1, SQLITE_TRANSIENT);
    else if (language && language[0])
        sqlite3_bind_text(stmt, bind_idx++, language, -1, SQLITE_TRANSIENT);

    tt_array_t results;
    tt_array_init(&results);

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        tt_hierarchy_node_t *node = calloc(1, sizeof(*node));
        if (!node)
            break;
        node->id = col_strdup(stmt, 0);
        node->file = col_strdup(stmt, 1);
        node->name = col_strdup(stmt, 2);
        node->qualified_name = col_strdup(stmt, 3);
        node->kind = col_strdup(stmt, 4);
        node->parent_id = col_strdup_nullable(stmt, 5);
        node->line = sqlite3_column_int(stmt, 6);
        node->end_line = sqlite3_column_int(stmt, 7);
        tt_array_push(&results, node);
    }
    sqlite3_finalize(stmt);

    *out_count = (int)results.len;
    *out = calloc((size_t)results.len, sizeof(tt_hierarchy_node_t));
    if (*out)
    {
        for (size_t i = 0; i < results.len; i++)
        {
            tt_hierarchy_node_t *n = results.items[i];
            (*out)[i] = *n;
            free(n);
        }
    }
    tt_array_free(&results);
    return 0;
}

/* ---- Co-occurrence (search:cooccurrence) ---- */

void tt_cooccurrence_free(tt_cooccurrence_t *items, int count)
{
    if (!items)
        return;
    for (int i = 0; i < count; i++)
    {
        free(items[i].file);
        free(items[i].name_a);
        free(items[i].kind_a);
        free(items[i].name_b);
        free(items[i].kind_b);
    }
    free(items);
}

int tt_store_search_cooccurrence(tt_index_store_t *store,
                                 const char *name_a, const char *name_b,
                                 const char *language, int limit,
                                 tt_cooccurrence_t **out, int *out_count)
{
    *out = NULL;
    *out_count = 0;

    if (!name_a || !name_a[0] || !name_b || !name_b[0])
        return 0;

    if (limit <= 0)
        limit = 50;

    tt_strbuf_t sql;
    tt_strbuf_init_cap(&sql, 512);
    tt_strbuf_append_str(&sql,
                         "SELECT s1.file, s1.name, s1.kind, s1.line, s2.name, s2.kind, s2.line "
                         "FROM symbols s1 "
                         "JOIN symbols s2 ON s1.file = s2.file AND s1.rowid < s2.rowid "
                         "WHERE s1.name LIKE ? AND s2.name LIKE ?");
    if (language && language[0])
        tt_strbuf_append_str(&sql, " AND s1.language = ?");

    char lim_buf[32];
    snprintf(lim_buf, sizeof(lim_buf), " LIMIT %d", limit);
    tt_strbuf_append_str(&sql, lim_buf);

    sqlite3_stmt *stmt = NULL;
    if (prepare(store->db->db, sql.data, &stmt) < 0)
    {
        tt_strbuf_free(&sql);
        return -1;
    }
    tt_strbuf_free(&sql);

    char pat_a[512], pat_b[512];
    snprintf(pat_a, sizeof(pat_a), "%%%s%%", name_a);
    snprintf(pat_b, sizeof(pat_b), "%%%s%%", name_b);

    int bind_idx = 1;
    sqlite3_bind_text(stmt, bind_idx++, pat_a, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, bind_idx++, pat_b, -1, SQLITE_TRANSIENT);
    if (language && language[0])
        sqlite3_bind_text(stmt, bind_idx++, language, -1, SQLITE_TRANSIENT);

    tt_array_t results;
    tt_array_init(&results);

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        tt_cooccurrence_t *r = calloc(1, sizeof(*r));
        if (!r)
            break;
        r->file = col_strdup(stmt, 0);
        r->name_a = col_strdup(stmt, 1);
        r->kind_a = col_strdup(stmt, 2);
        r->line_a = sqlite3_column_int(stmt, 3);
        r->name_b = col_strdup(stmt, 4);
        r->kind_b = col_strdup(stmt, 5);
        r->line_b = sqlite3_column_int(stmt, 6);
        tt_array_push(&results, r);
    }
    sqlite3_finalize(stmt);

    *out_count = (int)results.len;
    *out = calloc((size_t)results.len, sizeof(tt_cooccurrence_t));
    if (*out)
    {
        for (size_t i = 0; i < results.len; i++)
        {
            tt_cooccurrence_t *r = results.items[i];
            (*out)[i] = *r;
            free(r);
        }
    }
    tt_array_free(&results);
    return 0;
}

/* ---- Dependencies (inspect:dependencies) ---- */

void tt_dependency_free(tt_dependency_t *items, int count)
{
    if (!items)
        return;
    for (int i = 0; i < count; i++)
        free(items[i].file);
    free(items);
}

int tt_store_get_dependencies(tt_index_store_t *store,
                              const char *file_path, int max_depth,
                              tt_dependency_t **out, int *out_count)
{
    *out = NULL;
    *out_count = 0;

    if (!file_path || !file_path[0])
        return 0;

    if (max_depth <= 0)
        max_depth = 3;
    if (max_depth > 10)
        max_depth = 10;

    static const char *SQL_DEPS =
        "WITH RECURSIVE dep_graph(file, depth) AS ("
        "    SELECT source_file, 0 FROM imports"
        "    WHERE resolved_file = ?1"
        "    UNION"
        "    SELECT i.source_file, d.depth + 1"
        "    FROM imports i JOIN dep_graph d"
        "    ON i.resolved_file = d.file"
        "    WHERE d.depth < ?2"
        ")"
        "SELECT DISTINCT file, MIN(depth) as depth"
        " FROM dep_graph GROUP BY file ORDER BY depth, file";

    sqlite3_stmt *stmt = NULL;
    if (prepare(store->db->db, SQL_DEPS, &stmt) < 0)
        return -1;

    sqlite3_bind_text(stmt, 1, file_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, max_depth);

    tt_array_t results;
    tt_array_init(&results);

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        tt_dependency_t *d = calloc(1, sizeof(*d));
        if (!d)
            break;
        d->file = col_strdup(stmt, 0);
        d->depth = sqlite3_column_int(stmt, 1);
        tt_array_push(&results, d);
    }
    sqlite3_finalize(stmt);

    *out_count = (int)results.len;
    *out = calloc((size_t)results.len, sizeof(tt_dependency_t));
    if (*out)
    {
        for (size_t i = 0; i < results.len; i++)
        {
            tt_dependency_t *d = results.items[i];
            (*out)[i] = *d;
            free(d);
        }
    }
    tt_array_free(&results);
    return 0;
}

/* ---- Callers (find:callers) ---- */

void tt_caller_free(tt_caller_t *items, int count)
{
    if (!items)
        return;
    for (int i = 0; i < count; i++)
    {
        free(items[i].id);
        free(items[i].file);
        free(items[i].name);
        free(items[i].kind);
        free(items[i].signature);
    }
    free(items);
}

int tt_store_find_callers(tt_index_store_t *store,
                          const char *symbol_id, int limit,
                          tt_caller_t **out, int *out_count)
{
    *out = NULL;
    *out_count = 0;

    if (!symbol_id || !symbol_id[0])
        return 0;

    if (limit <= 0)
        limit = 50;

    /* Step 1: Get the target symbol's file and name */
    tt_symbol_result_t target;
    memset(&target, 0, sizeof(target));
    if (tt_store_get_symbol(store, symbol_id, &target) < 0 || !target.sym.file)
    {
        tt_symbol_result_free(&target);
        return 0;
    }

    const char *target_file = target.sym.file;
    const char *target_name = target.sym.name;

    /* Step 2: Find files that import the target's file */
    tt_import_t *importers = NULL;
    int imp_count = 0;
    tt_store_get_importers(store, target_file, &importers, &imp_count);

    if (imp_count == 0)
    {
        tt_import_array_free(importers, imp_count);
        tt_symbol_result_free(&target);
        return 0;
    }

    /* Step 3: In those files, find symbols whose signature references target_name */
    tt_strbuf_t sql;
    tt_strbuf_init_cap(&sql, 512);
    tt_strbuf_append_str(&sql,
                         "SELECT id, file, name, kind, line, signature "
                         "FROM symbols WHERE file IN (");

    /* Deduplicate importing files */
    tt_array_t unique_files;
    tt_array_init(&unique_files);
    for (int i = 0; i < imp_count; i++)
    {
        if (!importers[i].from_file)
            continue;
        bool dup = false;
        for (size_t j = 0; j < unique_files.len; j++)
        {
            if (strcmp(unique_files.items[j], importers[i].from_file) == 0)
            {
                dup = true;
                break;
            }
        }
        if (!dup)
            tt_array_push(&unique_files, importers[i].from_file);
    }

    if (unique_files.len == 0)
    {
        tt_strbuf_free(&sql);
        tt_array_free(&unique_files);
        tt_import_array_free(importers, imp_count);
        tt_symbol_result_free(&target);
        return 0;
    }

    for (size_t i = 0; i < unique_files.len; i++)
    {
        if (i > 0)
            tt_strbuf_append_char(&sql, ',');
        tt_strbuf_append_char(&sql, '?');
    }
    tt_strbuf_append_str(&sql,
                         ") AND signature LIKE ? ORDER BY file, line");

    char lim_buf[32];
    snprintf(lim_buf, sizeof(lim_buf), " LIMIT %d", limit);
    tt_strbuf_append_str(&sql, lim_buf);

    sqlite3_stmt *stmt = NULL;
    if (prepare(store->db->db, sql.data, &stmt) < 0)
    {
        tt_strbuf_free(&sql);
        tt_array_free(&unique_files);
        tt_import_array_free(importers, imp_count);
        tt_symbol_result_free(&target);
        return -1;
    }
    tt_strbuf_free(&sql);

    int bind_idx = 1;
    for (size_t i = 0; i < unique_files.len; i++)
        sqlite3_bind_text(stmt, bind_idx++, unique_files.items[i], -1, SQLITE_TRANSIENT);

    char sig_pattern[512];
    snprintf(sig_pattern, sizeof(sig_pattern), "%%%s%%", target_name);
    sqlite3_bind_text(stmt, bind_idx++, sig_pattern, -1, SQLITE_TRANSIENT);

    tt_array_t results;
    tt_array_init(&results);

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        tt_caller_t *c = calloc(1, sizeof(*c));
        if (!c)
            break;
        c->id = col_strdup(stmt, 0);
        c->file = col_strdup(stmt, 1);
        c->name = col_strdup(stmt, 2);
        c->kind = col_strdup(stmt, 3);
        c->line = sqlite3_column_int(stmt, 4);
        c->signature = col_strdup(stmt, 5);
        tt_array_push(&results, c);
    }
    sqlite3_finalize(stmt);
    tt_array_free(&unique_files);
    tt_import_array_free(importers, imp_count);
    tt_symbol_result_free(&target);

    *out_count = (int)results.len;
    *out = calloc((size_t)results.len, sizeof(tt_caller_t));
    if (*out)
    {
        for (size_t i = 0; i < results.len; i++)
        {
            tt_caller_t *c = results.items[i];
            (*out)[i] = *c;
            free(c);
        }
    }
    tt_array_free(&results);
    return 0;
}

/* ---- Similar symbols (search:similar) ---- */

int tt_store_search_similar(tt_index_store_t *store,
                            const char *symbol_id, int limit,
                            tt_search_results_t *out)
{
    memset(out, 0, sizeof(*out));

    if (!symbol_id || !symbol_id[0])
        return 0;

    if (limit <= 0)
        limit = 20;

    /* Step 1: Fetch the target symbol */
    tt_symbol_result_t target;
    memset(&target, 0, sizeof(target));
    if (tt_store_get_symbol(store, symbol_id, &target) < 0 || !target.sym.name)
    {
        tt_symbol_result_free(&target);
        return 0;
    }

    /* Step 2: Extract keywords from name and summary */
    tt_strbuf_t query_buf;
    tt_strbuf_init_cap(&query_buf, 128);

    /* Add name tokens (split on _ and camelCase) */
    const char *name = target.sym.name;
    int start = 0;
    int token_count = 0;
    for (int i = 0; name[i] && token_count < 5; i++)
    {
        bool is_boundary = (name[i] == '_') ||
                           (i > 0 && isupper((unsigned char)name[i]) &&
                            islower((unsigned char)name[i - 1]));
        bool is_end = (name[i + 1] == '\0');

        if (is_boundary || is_end)
        {
            int end = is_end ? i + 1 : i;
            if (name[start] == '_')
                start++;
            int len = end - start;
            if (len >= 3)
            {
                if (query_buf.len > 0)
                    tt_strbuf_append_str(&query_buf, " OR ");
                for (int j = start; j < end; j++)
                    tt_strbuf_append_char(&query_buf, (char)tolower((unsigned char)name[j]));
                token_count++;
            }
            if (is_boundary)
                start = (name[i] == '_') ? i + 1 : i;
        }
    }

    /* Add a few summary words if available */
    const char *summary = target.sym.summary;
    if (summary && summary[0] && token_count < 5)
    {
        /* Skip the first word (usually "Function", "Method", etc.) */
        const char *p = summary;
        while (*p && !isspace((unsigned char)*p))
            p++;
        while (*p && isspace((unsigned char)*p))
            p++;

        while (*p && token_count < 5)
        {
            while (*p && isspace((unsigned char)*p))
                p++;
            const char *word_start = p;
            while (*p && !isspace((unsigned char)*p))
                p++;
            int wlen = (int)(p - word_start);
            if (wlen >= 4)
            {
                if (query_buf.len > 0)
                    tt_strbuf_append_str(&query_buf, " OR ");
                for (int j = 0; j < wlen; j++)
                    tt_strbuf_append_char(&query_buf, (char)tolower((unsigned char)word_start[j]));
                token_count++;
            }
        }
    }

    if (query_buf.len == 0)
    {
        tt_strbuf_free(&query_buf);
        tt_symbol_result_free(&target);
        return 0;
    }

    /* Step 3: Search using FTS5 with extracted keywords */
    const char *kind = tt_kind_to_str(target.sym.kind);
    const char *kinds[1] = {kind};
    int kind_count = (kind && kind[0]) ? 1 : 0;

    tt_search_results_t raw;
    int rc = tt_store_search_symbols(store, query_buf.data,
                                     kinds, kind_count,
                                     NULL, NULL,
                                     limit + 1, false, &raw);
    tt_strbuf_free(&query_buf);
    tt_symbol_result_free(&target);

    if (rc < 0)
        return rc;

    /* Step 4: Filter out the input symbol itself, move results */
    out->results = calloc((size_t)raw.count, sizeof(tt_symbol_result_t));
    out->count = 0;
    if (out->results)
    {
        for (int i = 0; i < raw.count; i++)
        {
            if (strcmp(raw.results[i].sym.id, symbol_id) == 0 || out->count >= limit)
            {
                tt_symbol_result_free(&raw.results[i]);
                continue;
            }
            out->results[out->count++] = raw.results[i];
        }
    }
    else
    {
        for (int i = 0; i < raw.count; i++)
            tt_symbol_result_free(&raw.results[i]);
    }
    free(raw.results);
    return 0;
}

/* ---- Centrality ---- */

int tt_store_compute_centrality(tt_index_store_t *store)
{
    sqlite3 *db = store->db->db;

    /* Clear existing scores */
    char *errmsg = NULL;
    int rc = sqlite3_exec(db, "DELETE FROM file_centrality;", NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        sqlite3_free(errmsg);
        return -1;
    }

    /* Count distinct importers per resolved target file */
    const char *sql =
        "SELECT resolved_file, COUNT(DISTINCT source_file) "
        "FROM imports WHERE resolved_file != '' GROUP BY resolved_file";
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return -1;

    /* Insert centrality scores */
    const char *insert_sql =
        "INSERT OR REPLACE INTO file_centrality (file, score) VALUES (?, ?)";
    sqlite3_stmt *ins = NULL;
    rc = sqlite3_prepare_v2(db, insert_sql, -1, &ins, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return -1;
    }

    sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *target = (const char *)sqlite3_column_text(stmt, 0);
        int count = sqlite3_column_int(stmt, 1);
        double score = log(1.0 + count);

        sqlite3_bind_text(ins, 1, target, -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(ins, 2, score);
        sqlite3_step(ins);
        sqlite3_reset(ins);
    }
    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);

    sqlite3_finalize(stmt);
    sqlite3_finalize(ins);
    return 0;
}

tt_hashmap_t *tt_store_load_centrality(tt_index_store_t *store)
{
    sqlite3 *db = store->db->db;
    tt_hashmap_t *map = tt_hashmap_new(256);
    if (!map) return NULL;

    const char *sql = "SELECT file, score FROM file_centrality";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return map;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *file = (const char *)sqlite3_column_text(stmt, 0);
        double score = sqlite3_column_double(stmt, 1);
        /* Encode score * 1000 as intptr_t to store in hashmap */
        intptr_t encoded = (intptr_t)(score * 1000.0);
        tt_hashmap_set(map, file, (void *)encoded);
    }
    sqlite3_finalize(stmt);
    return map;
}
