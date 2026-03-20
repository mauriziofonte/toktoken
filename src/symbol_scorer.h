/*
 * symbol_scorer.h -- Symbol relevance scoring for search results.
 *
 * Calculates a relevance score for a symbol given a search query.
 * The scoring cascade matches SymbolScorer.php exactly.
 */

#ifndef TT_SYMBOL_SCORER_H
#define TT_SYMBOL_SCORER_H

/* Scoring weights (from SymbolScorer.php). */
#define TT_WEIGHT_EXACT_NAME 20
#define TT_WEIGHT_NAME_SUBSTRING 10
#define TT_WEIGHT_NAME_WORD 5
#define TT_WEIGHT_SIGNATURE_FULL 8
#define TT_WEIGHT_SIGNATURE_WORD 2
#define TT_WEIGHT_SUMMARY_FULL 5
#define TT_WEIGHT_SUMMARY_WORD 1
#define TT_WEIGHT_KEYWORD 3
#define TT_WEIGHT_DOCSTRING_WORD 1
#define TT_WEIGHT_CENTRALITY 0.3

/* Minimum score threshold for single/double-word queries.
 * For queries with 3+ words, the threshold scales up to require
 * that at least half the query words match in the symbol name.
 * This prevents partial-word noise from multi-word queries. */
#define TT_SCORE_MIN_THRESHOLD 5

/*
 * tt_score_symbol -- Calculate relevance score for a symbol.
 *
 * All comparisons are case-insensitive. The query_lower and query_words
 * must already be lowercased by the caller.
 *
 * The scoring cascade:
 *   Phase 1 (name): exact(+20) OR substring(+10) OR word(+5/word)
 *   Phase 2 (signature): full(+8) OR word(+2/word)
 *   Phase 3 (summary): full(+5) OR word(+1/word)
 *   Phase 4 (keywords): +3 per matching keyword
 *   Phase 5 (docstring): +1 per matching query word
 *
 * Returns 0 if no match at all.
 */
int tt_score_symbol(const char *name, const char *qualified_name,
                    const char *signature, const char *summary,
                    const char *keywords_json, const char *docstring,
                    const char *query_lower, const char **query_words,
                    int query_word_count);

/*
 * Score breakdown for debug mode.
 * Each field records the contribution from that scoring phase.
 */
typedef struct {
    int name_score;       /* Phase 1: name matching */
    int signature_score;  /* Phase 2: signature matching */
    int summary_score;    /* Phase 3: summary matching */
    int keyword_score;    /* Phase 4: keyword matching */
    int docstring_score;  /* Phase 5: docstring matching */
    int total;            /* Sum of all phases */
} tt_score_breakdown_t;

/*
 * tt_score_symbol_debug -- Like tt_score_symbol but returns per-field breakdown.
 */
int tt_score_symbol_debug(const char *name, const char *qualified_name,
                           const char *signature, const char *summary,
                           const char *keywords_json, const char *docstring,
                           const char *query_lower, const char **query_words,
                           int query_word_count,
                           tt_score_breakdown_t *breakdown);

#endif /* TT_SYMBOL_SCORER_H */
