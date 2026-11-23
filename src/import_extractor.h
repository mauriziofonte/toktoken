/*
 * import_extractor.h -- Extract import/require/include statements from source files.
 *
 * Per-language line-by-line extraction of import statements.
 * Produces an array of tt_import_t records for storage in the imports table.
 */

#ifndef TT_IMPORT_EXTRACTOR_H
#define TT_IMPORT_EXTRACTOR_H

#include <stddef.h>

typedef struct {
    char *from_file;      /* [owns] relative path of the importing file */
    char *to_specifier;   /* [owns] raw import specifier (module/path) */
    char *to_file;        /* [owns] resolved relative path, or NULL */
    char *symbol_name;    /* [owns] imported symbol name, or NULL */
    int line;             /* 1-indexed line number */
    char *import_type;    /* [owns] "import", "require", "include", "use", etc. */
    int dynamic;          /* 1 = runtime dispatch edge (e.g. Webmin foreign_require) */
    double confidence;    /* edge confidence in (0,1]; 0 means "static, 1.0" */
} tt_import_t;

/*
 * tt_extract_imports -- Extract import statements from a source file.
 *
 * Reads the file at project_root/file_path, scans for import patterns
 * based on the detected language.
 *
 * Returns 0 on success, -1 on error. On success, *out and *out_count
 * are populated (caller must free with tt_import_array_free).
 */
int tt_extract_imports(const char *project_root, const char *file_path,
                       const char *language, tt_import_t **out, int *out_count);

/*
 * tt_extract_imports_from_lines -- Extract imports from pre-split lines.
 *
 * Like tt_extract_imports but operates on already-loaded content lines,
 * avoiding a redundant file read when content is already in memory.
 * The lines array must be NULL-terminated (lines[nlines] == NULL).
 *
 * *out and *out_count may be pre-populated (appends to existing array).
 * *cap tracks allocated capacity.
 *
 * Returns 0 on success, -1 on error.
 */
int tt_extract_imports_from_lines(const char *file_path, const char *language,
                                  const char **lines, int nlines,
                                  tt_import_t **out, int *out_count, int *cap);

/*
 * tt_import_free -- Free a single import's owned strings.
 */
void tt_import_free(tt_import_t *imp);

/*
 * tt_import_array_free -- Free an array of imports and their strings.
 */
void tt_import_array_free(tt_import_t *imps, int count);

#endif /* TT_IMPORT_EXTRACTOR_H */
