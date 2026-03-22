/*
 * language_detector.c -- Detect programming language from file path.
 */

#include "language_detector.h"
#include "platform.h"
#include "str_util.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

typedef struct
{
    const char *ext;
    const char *language;
} ext_map_entry_t;

/* Extension-to-language mapping */
static const ext_map_entry_t EXT_MAP[] = {
    {"php", "php"},
    {"phtml", "php"},
    {"inc", "php"},
    {"js", "javascript"},
    {"jsx", "javascript"},
    {"mjs", "javascript"},
    {"cjs", "javascript"},
    {"ts", "typescript"},
    {"tsx", "typescript"},
    {"mts", "typescript"},
    {"vue", "vue"},
    {"py", "python"},
    {"pyw", "python"},
    {"go", "go"},
    {"rs", "rust"},
    {"java", "java"},
    {"c", "c"},
    {"h", "cpp"},
    {"cpp", "cpp"},
    {"cxx", "cpp"},
    {"cc", "cpp"},
    {"hpp", "cpp"},
    {"hxx", "cpp"},
    {"hh", "cpp"},
    {"cs", "csharp"},
    {"rb", "ruby"},
    {"kt", "kotlin"},
    {"kts", "kotlin"},
    {"swift", "swift"},
    {"dart", "dart"},
    {"lua", "lua"},
    {"pl", "perl"},
    {"pm", "perl"},
    {"sh", "bash"},
    {"bash", "bash"},
    {"zsh", "bash"},
    {"sql", "sql"},
    {"r", "r"},
    {"scala", "scala"},
    {"ex", "elixir"},
    {"exs", "elixir"},
    {"erl", "erlang"},
    {"hrl", "erlang"},
    {"f90", "fortran"},
    {"f95", "fortran"},
    {"f03", "fortran"},
    {"f08", "fortran"},
    {"f", "fortran"},
    {"for", "fortran"},
    {"fpp", "fortran"},
    {"hs", "haskell"},
    {"ml", "ocaml"},
    {"mli", "ocaml"},
    {"vim", "vim"},
    {"el", "elisp"},
    {"clj", "clojure"},
    {"cljs", "clojure"},
    {"cljc", "clojure"},
    {"groovy", "groovy"},
    {"v", "verilog"},
    {"sv", "verilog"},
    {"nix", "nix"},
    {"gleam", "gleam"},
    {"ejs", "ejs"},
    {"m", "objc"},
    {"mm", "objcpp"},
    {"proto", "protobuf"},
    {"sc", "scala"},
    {"lhs", "haskell"},
    {"gradle", "groovy"},
    {"css", "css"},
    {"toml", "toml"},
    {"tf", "hcl"},
    {"hcl", "hcl"},
    {"tfvars", "hcl"},
    {"graphql", "graphql"},
    {"gql", "graphql"},
    {"jl", "julia"},
    {"gd", "gdscript"},
    {"verse", "verse"},
    {"html", "html"},
    {"htm", "html"},
    {"xml", "xml"},
    {"xul", "xml"},
    {"ahk", "autohotkey"},
    {"yaml", "yaml"},
    {"yml", "yaml"},
    {"asm", "asm"},
    {"s", "asm"},
    {"md", "markdown"},
    {"markdown", "markdown"},
    {"mdx", "markdown"},
};

#define EXT_MAP_SIZE (sizeof(EXT_MAP) / sizeof(EXT_MAP[0]))

/* Runtime extra extensions (borrowed from config, valid for config lifetime) */
static const char **s_extra_ext_keys = NULL;
static const char **s_extra_ext_langs = NULL;
static int s_extra_ext_count = 0;

void tt_lang_set_extra_extensions(const char **ext_keys, const char **languages, int count)
{
    s_extra_ext_keys = ext_keys;
    s_extra_ext_langs = languages;
    s_extra_ext_count = count;
}

void tt_lang_clear_extra_extensions(void)
{
    s_extra_ext_keys = NULL;
    s_extra_ext_langs = NULL;
    s_extra_ext_count = 0;
}

const char *tt_language_from_extension(const char *ext)
{
    if (!ext || !ext[0])
        return NULL;

    /* Check extra extensions first (runtime overrides) */
    for (int i = 0; i < s_extra_ext_count; i++)
    {
        if (tt_strcasecmp(s_extra_ext_keys[i], ext) == 0)
            return s_extra_ext_langs[i];
    }

    for (size_t i = 0; i < EXT_MAP_SIZE; i++)
    {
        if (tt_strcasecmp(EXT_MAP[i].ext, ext) == 0)
            return EXT_MAP[i].language;
    }
    return NULL;
}

const char *tt_detect_language(const char *file_path)
{
    if (!file_path)
        return "unknown";

    /* Special case: .blade.php checked before extension. */
    if (tt_str_ends_with(file_path, ".blade.php"))
        return "blade";

    /* Special case: OpenAPI/Swagger well-known basenames and compound extensions */
    if (tt_str_ends_with(file_path, ".openapi.yaml") ||
        tt_str_ends_with(file_path, ".openapi.yml") ||
        tt_str_ends_with(file_path, ".openapi.json") ||
        tt_str_ends_with(file_path, ".swagger.yaml") ||
        tt_str_ends_with(file_path, ".swagger.yml") ||
        tt_str_ends_with(file_path, ".swagger.json")) {
        return "openapi";
    }
    {
        const char *base = strrchr(file_path, '/');
        base = base ? base + 1 : file_path;
        if (strcasecmp(base, "openapi.yaml") == 0 ||
            strcasecmp(base, "openapi.yml") == 0 ||
            strcasecmp(base, "openapi.json") == 0 ||
            strcasecmp(base, "swagger.yaml") == 0 ||
            strcasecmp(base, "swagger.yml") == 0 ||
            strcasecmp(base, "swagger.json") == 0) {
            return "openapi";
        }
    }

    /* Get extension (includes dot, e.g. ".php"). */
    const char *ext_with_dot = tt_path_extension(file_path);
    if (!ext_with_dot || !ext_with_dot[0])
        return "unknown";

    /* Skip the leading dot. */
    const char *ext = ext_with_dot + 1;

    const char *lang = tt_language_from_extension(ext);
    if (lang)
        return lang;

    /* Unknown: return lowercased extension.
     * Use a thread-local buffer for the static return. */
    static _Thread_local char buf[64];
    size_t len = strlen(ext);
    if (len >= sizeof(buf))
        len = sizeof(buf) - 1;
    for (size_t i = 0; i < len; i++)
        buf[i] = (char)tolower((unsigned char)ext[i]);
    buf[len] = '\0';
    return buf;
}
