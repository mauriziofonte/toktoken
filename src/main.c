/*
 * main.c -- TokToken CLI entry point and command dispatch.
 */

#include "version.h"
#include "cli.h"
#include "json_output.h"
#include "cmd_index.h"
#include "cmd_search.h"
#include "cmd_inspect.h"
#include "cmd_manage.h"
#include "cmd_github.h"
#include "cmd_serve.h"
#include "cmd_find.h"
#include "cmd_bundle.h"
#include "cmd_update.h"
#include "cmd_suggest.h"
#include "cmd_help.h"
#include "update_check.h"

#include <stdio.h>
#include <string.h>
#include <signal.h>

/* Signal handler for clean interruption.
 *
 * Installed via sigaction() WITHOUT SA_RESTART so that blocking syscalls
 * (read, poll, waitpid, getline, etc.) return -1/EINTR instead of silently
 * restarting. This lets every blocking loop check tt_interrupted and exit
 * promptly. Using signal() would set SA_RESTART on glibc/Linux, making the
 * process appear unkillable with SIGINT/SIGTERM.
 */
static void signal_handler(int sig)
{
    (void)sig;
    tt_interrupted = 1;
}

static void print_usage(void)
{
    tt_update_info_t info = tt_update_check();
    if (info.update_available) {
        fprintf(stderr,
                "TokToken (beta release) %s (update available: %s -- run 'toktoken --self-update')\n",
                TT_VERSION, info.upstream_version);
    } else {
        fprintf(stderr, "TokToken (beta release) %s\n", TT_VERSION);
    }
    tt_update_info_free(&info);

    fprintf(stderr,
            "\n"
            "Usage: toktoken <command> [options]\n"
            "\n"
            "Indexing:\n"
            "  index:create [path]    Create index for a project\n"
            "  index:update [path]    Update existing index\n"
            "  index:file <file>      Reindex a single file\n"
            "  index:github <repo>    Clone and index a GitHub repository\n"
            "\n"
            "Search:\n"
            "  search:symbols <query> Search symbols by name\n"
            "  search:text <query>    Full-text search across files\n"
            "  search:cooccurrence <a>,<b> Find symbols co-occurring in same file\n"
            "  search:similar <id>    Find symbols similar to a given one\n"
            "  find:importers <file>  Find files that import a given file\n"
            "  find:references <id>   Find import references to an identifier\n"
            "  find:callers <id>      Find symbols that call a given function\n"
            "  find:dead             Find unreferenced symbols\n"
            "\n"
            "Inspect:\n"
            "  inspect:outline <file> Show file symbol hierarchy\n"
            "  inspect:symbol <id>    Show symbol source code\n"
            "  inspect:file <file>    Show file content\n"
            "  inspect:bundle <id>    Get symbol context bundle (definition + imports + outline)\n"
            "  inspect:tree           Show indexed file tree\n"
            "  inspect:dependencies <file> Trace transitive import graph\n"
            "  inspect:hierarchy <file>    Show class/function hierarchy\n"
            "  inspect:cycles              Detect circular import cycles\n"
            "  inspect:blast <id>          Symbol blast radius analysis\n"
            "\n"
            "Help:\n"
            "  help [command]         List all tools or show detailed usage\n"
            "\n"
            "Discovery:\n"
            "  suggest                Get suggested queries for exploring a codebase\n"
            "\n"
            "Management:\n"
            "  stats                  Show index statistics\n"
            "  projects:list          List indexed projects\n"
            "  cache:clear            Clear index cache\n"
            "  codebase:detect [path] Detect if directory is a codebase\n"
            "  repos:list             List cloned GitHub repositories\n"
            "  repos:remove <repo>    Remove a cloned repository\n"
            "  repos:clear            Remove all cloned repositories\n"
            "\n"
            "MCP Server:\n"
            "  serve                  Start MCP server on STDIO\n"
            "\n");

    fprintf(stderr,
            "Indexing options:\n"
            "  -m, --max-files <n>    Max files to index (default: 200000)\n"
            "  -f, --full             Disable smart filter, index all file types\n"
            "  -i, --ignore <pattern> Extra ignore pattern (repeatable)\n"
            "      --languages <list> Comma-separated language filter\n"
            "  -X, --diagnostic       Emit JSONL diagnostics to stderr\n"
            "\n"
            "Search options:\n"
            "  -k, --kind <type>      Filter by symbol kind (function,class,chapter,...)\n"
            "  -L, --language <lang>  Filter by language\n"
            "  -n, --count            Show match count only\n"
            "  -g, --group-by file    Group text search results by file\n"
            "  -C, --context <n>      Context lines around matches\n"
            "  -r, --regex            Treat search query as regex\n"
            "  -s, --case-sensitive   Case-sensitive search\n"
            "  -D, --debug            Show scoring breakdown\n"
            "      --no-sig           Omit signatures from results\n"
            "      --no-summary       Omit summaries from results\n"
            "      --detail <level>   Detail: compact, standard (default), full\n"
            "      --token-budget <n> Max token budget for results\n"
            "      --scope-imports-of <file>  Scope to files imported by file\n"
            "      --scope-importers-of <file> Scope to files that import file\n"
            "\n"
            "Find options:\n"
            "      --has-importers    Enrich importers with has_importers flag\n"
            "      --exclude-tests    Exclude test files (find:dead)\n"
            "\n"
            "Inspect options:\n"
            "      --lines <start-end>  Line range for inspect:file\n"
            "  -d, --depth <n>        Tree depth / blast radius depth\n"
            "      --include-callers  Include callers in bundle output\n"
            "      --cross-dir        Show only cross-directory cycles\n"
            "      --min-length <n>   Min cycle length (inspect:cycles)\n"
            "\n"
            "Global options:\n"
            "  -p, --path <dir>       Project path (default: current directory)\n"
            "  -o, --format <fmt>     Output format: json, table (default: json)\n"
            "      --filter <pattern> Include files matching pattern\n"
            "  -e, --exclude <pat>    Exclude files matching pattern\n"
            "  -l, --limit <n>        Max results\n"
            "  -c, --compact          Compact JSON output\n"
            "  -u, --unique           Deduplicate results\n"
            "  -S, --sort <field>     Sort order (default: score)\n"
            "  -t, --truncate <n>     Truncate width (default: 120, min: 20)\n"
            "      --self-update      Update to latest version\n"
            "  -v, --version          Show version\n"
            "  -h, --help             Show this help\n"
            "\n"
            "Short flags can be combined: -cn is equivalent to --compact --count\n"
            "Value flags accept: -l10 or -l 10 or --limit 10\n");
}

/* Command name -> handler dispatch */
typedef int (*cmd_handler_t)(tt_cli_opts_t *opts);

typedef struct
{
    const char *name;
    cmd_handler_t handler;
} cmd_entry_t;

static const cmd_entry_t commands[] = {
    {"index:create", tt_cmd_index_create},
    {"index:update", tt_cmd_index_update},
    {"index:file", tt_cmd_index_file},
    {"search:symbols", tt_cmd_search_symbols},
    {"search:text", tt_cmd_search_text},
    {"inspect:outline", tt_cmd_inspect_outline},
    {"inspect:symbol", tt_cmd_inspect_symbol},
    {"inspect:file", tt_cmd_inspect_file},
    {"inspect:tree", tt_cmd_inspect_tree},
    {"stats", tt_cmd_stats},
    {"projects:list", tt_cmd_projects_list},
    {"cache:clear", tt_cmd_cache_clear},
    {"codebase:detect", tt_cmd_codebase_detect},
    {"index:github", tt_cmd_index_github},
    {"repos:list", tt_cmd_repos_list},
    {"repos:remove", tt_cmd_repos_remove},
    {"repos:clear", tt_cmd_repos_clear},
    {"inspect:bundle", tt_cmd_inspect_bundle},
    {"find:importers", tt_cmd_find_importers},
    {"find:references", tt_cmd_find_references},
    {"find:callers", tt_cmd_find_callers},
    {"find:dead", tt_cmd_find_dead},
    {"search:cooccurrence", tt_cmd_search_cooccurrence},
    {"search:similar", tt_cmd_search_similar},
    {"inspect:dependencies", tt_cmd_inspect_dependencies},
    {"inspect:hierarchy", tt_cmd_inspect_hierarchy},
    {"inspect:cycles", tt_cmd_inspect_cycles},
    {"inspect:blast", tt_cmd_inspect_blast},
    {"suggest", tt_cmd_suggest},
    {"serve", tt_cmd_serve},
    {"help", tt_cmd_help},
    {NULL, NULL}};

int main(int argc, char *argv[])
{
    /* Install signal handlers for clean interruption. */
#ifdef TT_PLATFORM_WINDOWS
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#else
    /* Use sigaction WITHOUT SA_RESTART so that blocking syscalls
     * (read, poll, waitpid, getline, etc.) return -1/EINTR instead of
     * silently restarting. This lets every blocking loop check
     * tt_interrupted and exit promptly. */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = signal_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0; /* no SA_RESTART */
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
    }
#endif

    /* No arguments -> help */
    if (argc < 2)
    {
        print_usage();
        return 0;
    }

    /* Check for --version / --help / --self-update before command dispatch */
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--version") == 0)
        {
            tt_update_info_t info = tt_update_check();
            if (info.update_available) {
                printf("TokToken (beta release) %s (update available: %s)\n",
                       TT_VERSION, info.upstream_version);
            } else {
                printf("TokToken (beta release) %s\n", TT_VERSION);
            }
            tt_update_info_free(&info);
            return 0;
        }
        if (strcmp(argv[i], "--self-update") == 0 ||
            strcmp(argv[i], "--self-upgrade") == 0)
        {
            tt_cli_opts_t opts;
            memset(&opts, 0, sizeof(opts));
            return tt_cmd_self_update(&opts);
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            print_usage();
            return 0;
        }
    }

    /* First positional argument is the command name */
    const char *cmd_name = argv[1];

    /* Find command handler */
    cmd_handler_t handler = NULL;
    for (int i = 0; commands[i].name != NULL; i++)
    {
        if (strcmp(cmd_name, commands[i].name) == 0)
        {
            handler = commands[i].handler;
            break;
        }
    }

    if (!handler)
    {
        /* Unknown command */
        fprintf(stderr, "Unknown command: %s\n\n", cmd_name);
        fprintf(stderr, "Run 'toktoken --help' for available commands.\n");
        return 1;
    }

    /* Parse remaining arguments */
    tt_cli_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.truncate_width = 120;

    if (tt_cli_parse(&opts, argc, argv) < 0)
    {
        tt_cli_opts_free(&opts);
        return 1;
    }

    /* Dispatch */
    int exit_code = handler(&opts);

    tt_cli_opts_free(&opts);
    return exit_code;
}
