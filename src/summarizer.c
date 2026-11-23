/*
 * summarizer.c -- Symbol summary generation (Tier 1 + Tier 3).
 */

#include "summarizer.h"
#include "index_store.h"
#include "str_util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

char *tt_summarize_docstring(const char *docstring)
{
    if (!docstring)
        return tt_strdup("");

    /* 1. Trim. If empty, return "" */
    char *trimmed = tt_strdup(docstring);
    if (!trimmed)
        return tt_strdup("");
    tt_str_trim(trimmed);

    if (!trimmed[0])
    {
        free(trimmed);
        return tt_strdup("");
    }

    /* 2. Split by "\n" */
    int line_count = 0;
    char **lines = tt_str_split(trimmed, '\n', &line_count);
    free(trimmed);

    if (!lines || line_count == 0)
    {
        tt_str_split_free(lines);
        return tt_strdup("");
    }

    /* 3. Collect lines until blank line or @tag */
    tt_strbuf_t cleaned;
    tt_strbuf_init(&cleaned);
    bool has_content = false;

    for (int i = 0; i < line_count; i++)
    {
        char *line = lines[i];
        tt_str_trim(line);

        /* Stop at blank line if we already have content */
        if (line[0] == '\0' && has_content)
            break;

        /* Stop at @tag (e.g. @param, @return) */
        if (line[0] == '@' && line[1] && isalpha((unsigned char)line[1]))
            break;

        /* Skip empty lines at the beginning */
        if (line[0] == '\0')
            continue;

        if (has_content)
        {
            tt_strbuf_append_char(&cleaned, ' ');
        }
        tt_strbuf_append_str(&cleaned, line);
        has_content = true;
    }

    tt_str_split_free(lines);

    char *text = tt_strbuf_detach(&cleaned);
    if (!text || !text[0])
    {
        free(text);
        return tt_strdup("");
    }

    tt_str_trim(text);
    if (!text[0])
    {
        free(text);
        return tt_strdup("");
    }

    /* 5. Extract first sentence: match /^(.+?\.)\s/
     * PHP appends " " to text before matching, so we check for ". " or ".\n" etc. */
    char *result = NULL;
    const char *p = text;
    while (*p)
    {
        if (*p == '.' && *(p + 1))
        {
            char next = *(p + 1);
            if (next == ' ' || next == '\t' || next == '\n' || next == '\r')
            {
                size_t sentence_len = (size_t)(p - text) + 1;
                result = tt_strndup(text, sentence_len);
                break;
            }
        }
        p++;
    }

    /* Also handle period at end of text (PHP appends " " so "text." becomes "text. ") */
    if (!result)
    {
        size_t tlen = strlen(text);
        if (tlen > 0 && text[tlen - 1] == '.')
        {
            result = tt_strdup(text);
        }
    }

    if (!result)
    {
        result = text;
        text = NULL;
    }
    free(text);

    /* 6. Truncate: if mb_strlen > 120 → mb_substr(0, 117) + "..." */
    size_t char_count = tt_utf8_strlen(result);
    if (char_count > 120)
    {
        /* Find byte position of 117th UTF-8 codepoint */
        size_t cp = 0;
        size_t byte_pos = 0;
        size_t rlen = strlen(result);
        while (byte_pos < rlen && cp < 117)
        {
            unsigned char c = (unsigned char)result[byte_pos];
            if (c < 0x80)
                byte_pos++;
            else if (c < 0xE0)
                byte_pos += 2;
            else if (c < 0xF0)
                byte_pos += 3;
            else
                byte_pos += 4;
            if (byte_pos > rlen)
                byte_pos = rlen;
            cp++;
        }

        char *truncated = malloc(byte_pos + 4);
        if (truncated)
        {
            memcpy(truncated, result, byte_pos);
            memcpy(truncated + byte_pos, "...", 3);
            truncated[byte_pos + 3] = '\0';
            free(result);
            result = truncated;
        }
    }

    return result;
}

char *tt_summarize_signature(tt_symbol_kind_e kind, const char *name)
{
    const char *label = tt_kind_label(kind);
    if (!name)
        name = "";

    size_t len = strlen(label) + 1 + strlen(name) + 1;
    char *result = malloc(len);
    if (!result)
        return tt_strdup("");

    snprintf(result, len, "%s %s", label, name);
    return result;
}

bool tt_is_tier3_fallback(const char *summary, tt_symbol_kind_e kind, const char *name)
{
    /* Empty summary is a fallback */
    if (!summary || !summary[0])
        return true;

    /* Check if summary == "{KindLabel} {name}" */
    char *expected = tt_summarize_signature(kind, name);
    if (!expected)
        return false;

    bool is_fallback = (strcmp(summary, expected) == 0);
    free(expected);
    return is_fallback;
}

int tt_apply_sync_summaries(tt_database_t *db)
{
    tt_index_store_t store;
    if (tt_store_init(&store, db) < 0)
        return -1;

    tt_database_begin(db);

    /*
     * Phase 1: Tier 3 — set fallback summaries in pure SQL.
     *
     * For symbols without docstrings, the summary is just "Label name"
     * (e.g. "Function foo", "Class Bar"). This single UPDATE handles
     * ~95% of symbols without any C↔SQLite round trips.
     */
    {
        char *errmsg = NULL;
        int rc = sqlite3_exec(db->db,
            "UPDATE symbols SET summary = "
            "CASE kind "
            "WHEN 'class'     THEN 'Class ' || name "
            "WHEN 'interface' THEN 'Interface ' || name "
            "WHEN 'trait'     THEN 'Trait ' || name "
            "WHEN 'enum'      THEN 'Enum ' || name "
            "WHEN 'function'  THEN 'Function ' || name "
            "WHEN 'subroutine' THEN 'Subroutine ' || name "
            "WHEN 'method'    THEN 'Method ' || name "
            "WHEN 'constant'  THEN 'Constant ' || name "
            "WHEN 'property'  THEN 'Property ' || name "
            "WHEN 'variable'  THEN 'Variable ' || name "
            "WHEN 'namespace' THEN 'Namespace ' || name "
            "WHEN 'type'      THEN 'Type definition ' || name "
            "WHEN 'directive' THEN 'Directive ' || name "
            "ELSE kind || ' ' || name "
            "END "
            "WHERE summary = '' AND (docstring = '' OR docstring IS NULL)",
            NULL, NULL, &errmsg);
        if (rc != SQLITE_OK)
        {
            sqlite3_free(errmsg);
            tt_database_commit(db);
            tt_store_close(&store);
            return -1;
        }
    }

    /*
     * Phase 2: Tier 1 — process symbols that have docstrings.
     *
     * Fetch only (id, docstring, kind, name) — lightweight query.
     * Parse docstrings in C, then update via prepared statement.
     * Typically ~5% of symbols have real docstrings.
     */
    {
        tt_summary_input_t *items = NULL;
        int count = 0;

        if (tt_store_get_docstring_symbols(&store, &items, &count) < 0)
        {
            tt_database_commit(db);
            tt_store_close(&store);
            return -1;
        }

        for (int i = 0; i < count; i++)
        {
            char *summary = tt_summarize_docstring(items[i].docstring);

            /* If docstring parsing produces empty result, keep the Tier 3
             * fallback that was already set in Phase 1. */
            if (summary && summary[0])
            {
                tt_store_update_symbol_summary(&store, items[i].id, summary);
            }
            free(summary);
        }

        tt_summary_input_free(items, count);
    }

    tt_database_commit(db);
    tt_store_close(&store);
    return 0;
}
