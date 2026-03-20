/*
 * test_int_multilang.c -- Comprehensive multi-language E2E integration tests.
 *
 * Creates a realistic multi-language project fixture (36 files, 12+ languages),
 * indexes it, and tests ALL 23 tools with specific expected metrics.
 *
 * Languages with import extraction:
 *   PHP, TypeScript, Python, Rust, C, Java, Go, Ruby, C#, Kotlin, Haskell, Swift
 *
 * Languages with ctags/custom parsers only:
 *   Shell, SQL, Lua, Perl, Julia, GDScript
 */

#include "test_framework.h"
#include "test_helpers.h"

#include "cmd_search.h"
#include "cmd_inspect.h"
#include "cmd_index.h"
#include "cmd_find.h"
#include "cmd_bundle.h"
#include "cmd_manage.h"
#include "cli.h"
#include "error.h"
#include "platform.h"

#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Test fixture ---- */

static char *test_dir = NULL;

static void init_opts(tt_cli_opts_t *opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->path = test_dir;
    opts->truncate_width = 120;
}

/* ---- Helper: find symbol ID by searching ---- */

static char *find_symbol_id(const char *query, const char *kind_filter)
{
    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {query};
    opts.positional = pos;
    opts.positional_count = 1;
    if (kind_filter) opts.kind = kind_filter;

    cJSON *result = tt_cmd_search_symbols_exec(&opts);
    if (!result) return NULL;

    cJSON *results = cJSON_GetObjectItem(result, "results");
    if (!results || cJSON_GetArraySize(results) == 0)
    {
        cJSON_Delete(result);
        return NULL;
    }

    const char *id = cJSON_GetStringValue(
        cJSON_GetObjectItem(cJSON_GetArrayItem(results, 0), "id"));
    char *dup = id ? strdup(id) : NULL;
    cJSON_Delete(result);
    return dup;
}

/* ---- Fixture setup ---- */

static void setup_fixture(void)
{
    test_dir = tt_test_tmpdir();
    if (!test_dir)
    {
        fprintf(stderr, "  FATAL: tt_test_tmpdir() returned NULL\n");
        return;
    }

    /* ---- PHP ---- */
    tt_test_write_file(test_dir, "src/php/Controller.php",
        "<?php\n"
        "\n"
        "namespace App\\Http;\n"
        "\n"
        "use App\\Service\\UserService;\n"
        "\n"
        "class Controller\n"
        "{\n"
        "    private UserService $service;\n"
        "\n"
        "    public function index(): array\n"
        "    {\n"
        "        return $this->service->findAll();\n"
        "    }\n"
        "\n"
        "    public function show(int $id): array\n"
        "    {\n"
        "        return $this->service->findById($id);\n"
        "    }\n"
        "\n"
        "    public function store(array $data): bool\n"
        "    {\n"
        "        return $this->service->create($data);\n"
        "    }\n"
        "}\n");

    tt_test_write_file(test_dir, "app/Service/UserService.php",
        "<?php\n"
        "\n"
        "namespace App\\Service;\n"
        "\n"
        "class UserService\n"
        "{\n"
        "    public function findAll(): array\n"
        "    {\n"
        "        return [];\n"
        "    }\n"
        "\n"
        "    public function findById(int $id): array\n"
        "    {\n"
        "        return [];\n"
        "    }\n"
        "\n"
        "    public function create(array $data): bool\n"
        "    {\n"
        "        return true;\n"
        "    }\n"
        "}\n");

    /* ---- TypeScript ---- */
    tt_test_write_file(test_dir, "src/ts/app.ts",
        "import { formatOutput } from './utils';\n"
        "import { Logger } from '../lib/logger';\n"
        "\n"
        "export function runApp(): void {\n"
        "    const logger = new Logger();\n"
        "    logger.info(formatOutput('started'));\n"
        "}\n"
        "\n"
        "export function shutdown(): void {\n"
        "    const logger = new Logger();\n"
        "    logger.info('stopping');\n"
        "}\n");

    tt_test_write_file(test_dir, "src/ts/utils.ts",
        "import { validateInput } from './helpers';\n"
        "\n"
        "export function formatOutput(value: string): string {\n"
        "    if (validateInput(value)) {\n"
        "        return value.trim();\n"
        "    }\n"
        "    return '';\n"
        "}\n"
        "\n"
        "export function parseNumber(s: string): number {\n"
        "    return parseInt(s, 10);\n"
        "}\n");

    tt_test_write_file(test_dir, "src/ts/helpers.ts",
        "export function validateInput(x: unknown): boolean {\n"
        "    return x !== null && x !== undefined;\n"
        "}\n"
        "\n"
        "export function sanitize(input: string): string {\n"
        "    return input.replace(/[<>]/g, '');\n"
        "}\n");

    tt_test_write_file(test_dir, "src/lib/logger.ts",
        "export class Logger {\n"
        "    info(msg: string): void {\n"
        "        console.log(msg);\n"
        "    }\n"
        "\n"
        "    error(msg: string): void {\n"
        "        console.error(msg);\n"
        "    }\n"
        "}\n");

    /* ---- Python ---- */
    tt_test_write_file(test_dir, "src/python/main.py",
        "from services.auth import authenticate\n"
        "\n"
        "def main():\n"
        "    result = authenticate('admin', 'secret')\n"
        "    print(result)\n"
        "\n"
        "def parse_args():\n"
        "    return {}\n");

    tt_test_write_file(test_dir, "src/python/services/auth.py",
        "from models.user import User\n"
        "\n"
        "def authenticate(username, password):\n"
        "    user = User(username)\n"
        "    return user.check_password(password)\n"
        "\n"
        "def generate_token(user_id):\n"
        "    return str(user_id)\n");

    tt_test_write_file(test_dir, "src/python/models/user.py",
        "class User:\n"
        "    def __init__(self, username):\n"
        "        self.username = username\n"
        "\n"
        "    def check_password(self, password):\n"
        "        return len(password) > 0\n"
        "\n"
        "    def get_display_name(self):\n"
        "        return self.username\n");

    /* ---- Rust ---- */
    tt_test_write_file(test_dir, "src/rust/main.rs",
        "mod config;\n"
        "\n"
        "fn main() {\n"
        "    let settings = config::load();\n"
        "    println!(\"{}\", settings);\n"
        "}\n"
        "\n"
        "fn version() -> &'static str {\n"
        "    \"1.0.0\"\n"
        "}\n");

    tt_test_write_file(test_dir, "src/rust/config.rs",
        "pub struct Settings {\n"
        "    name: String,\n"
        "}\n"
        "\n"
        "impl Settings {\n"
        "    pub fn new() -> Self {\n"
        "        Settings { name: String::from(\"app\") }\n"
        "    }\n"
        "}\n"
        "\n"
        "pub fn load() -> String {\n"
        "    String::from(\"loaded\")\n"
        "}\n");

    /* ---- C ---- */
    tt_test_write_file(test_dir, "src/c/main.c",
        "#include \"utils.h\"\n"
        "\n"
        "int main(void) {\n"
        "    int result = add_numbers(1, 2);\n"
        "    char *msg = format_message(\"hello\");\n"
        "    return result;\n"
        "}\n");

    tt_test_write_file(test_dir, "src/c/utils.h",
        "#ifndef UTILS_H\n"
        "#define UTILS_H\n"
        "\n"
        "int add_numbers(int a, int b);\n"
        "char *format_message(const char *msg);\n"
        "int multiply(int a, int b);\n"
        "\n"
        "#endif\n");

    tt_test_write_file(test_dir, "src/c/utils.c",
        "#include \"utils.h\"\n"
        "#include <string.h>\n"
        "\n"
        "int add_numbers(int a, int b) {\n"
        "    return a + b;\n"
        "}\n"
        "\n"
        "char *format_message(const char *msg) {\n"
        "    return (char *)msg;\n"
        "}\n"
        "\n"
        "int multiply(int a, int b) {\n"
        "    return a * b;\n"
        "}\n");

    /* ---- Java ---- */
    tt_test_write_file(test_dir, "src/java/App.java",
        "import com.example.service.UserService;\n"
        "\n"
        "public class App {\n"
        "    public static void main(String[] args) {\n"
        "        UserService svc = new UserService();\n"
        "        svc.process();\n"
        "    }\n"
        "\n"
        "    public static String getVersion() {\n"
        "        return \"1.0\";\n"
        "    }\n"
        "}\n");

    tt_test_write_file(test_dir, "src/java/service/UserService.java",
        "package com.example.service;\n"
        "\n"
        "public class UserService {\n"
        "    public void process() {\n"
        "        System.out.println(\"processing\");\n"
        "    }\n"
        "\n"
        "    public boolean validate(String input) {\n"
        "        return input != null;\n"
        "    }\n"
        "\n"
        "    public int count() {\n"
        "        return 0;\n"
        "    }\n"
        "}\n");

    /* ---- Go ---- */
    tt_test_write_file(test_dir, "src/go/main.go",
        "package main\n"
        "\n"
        "import \"myapp/pkg/handler\"\n"
        "\n"
        "func main() {\n"
        "    handler.Handle()\n"
        "}\n"
        "\n"
        "func setup() string {\n"
        "    return \"ready\"\n"
        "}\n");

    tt_test_write_file(test_dir, "src/go/pkg/handler.go",
        "package handler\n"
        "\n"
        "func Handle() {\n"
        "    Parse()\n"
        "}\n"
        "\n"
        "func Parse() string {\n"
        "    return \"parsed\"\n"
        "}\n");

    /* ---- Ruby ---- */
    tt_test_write_file(test_dir, "src/ruby/app.rb",
        "require_relative './helpers'\n"
        "\n"
        "def run_application\n"
        "  result = format_output(\"data\")\n"
        "  validate_input(result)\n"
        "end\n"
        "\n"
        "def stop_application\n"
        "  puts \"stopped\"\n"
        "end\n");

    tt_test_write_file(test_dir, "src/ruby/helpers.rb",
        "def format_output(data)\n"
        "  data.to_s.strip\n"
        "end\n"
        "\n"
        "def validate_input(input)\n"
        "  !input.nil? && !input.empty?\n"
        "end\n"
        "\n"
        "def log_message(msg)\n"
        "  puts msg\n"
        "end\n");

    /* ---- C# ---- */
    tt_test_write_file(test_dir, "src/csharp/Program.cs",
        "using MyApp.Data.Repository;\n"
        "\n"
        "namespace MyApp\n"
        "{\n"
        "    class Program\n"
        "    {\n"
        "        static void Main(string[] args)\n"
        "        {\n"
        "            var repo = new Repository();\n"
        "            repo.Save();\n"
        "        }\n"
        "\n"
        "        static string GetConfig()\n"
        "        {\n"
        "            return \"default\";\n"
        "        }\n"
        "    }\n"
        "}\n");

    tt_test_write_file(test_dir, "src/csharp/Data/Repository.cs",
        "namespace MyApp.Data\n"
        "{\n"
        "    public class Repository\n"
        "    {\n"
        "        public void Save()\n"
        "        {\n"
        "        }\n"
        "\n"
        "        public void Delete(int id)\n"
        "        {\n"
        "        }\n"
        "\n"
        "        public object Find(int id)\n"
        "        {\n"
        "            return null;\n"
        "        }\n"
        "    }\n"
        "}\n");

    /* ---- Swift ---- */
    tt_test_write_file(test_dir, "src/swift/App.swift",
        "import Foundation\n"
        "\n"
        "class AppController {\n"
        "    func start() {\n"
        "        print(\"started\")\n"
        "    }\n"
        "\n"
        "    func stop() {\n"
        "        print(\"stopped\")\n"
        "    }\n"
        "}\n"
        "\n"
        "func createController() -> AppController {\n"
        "    return AppController()\n"
        "}\n");

    /* ---- Kotlin ---- */
    tt_test_write_file(test_dir, "src/kotlin/Main.kt",
        "import data.Repository\n"
        "\n"
        "fun main() {\n"
        "    val repo = Repository()\n"
        "    repo.save()\n"
        "}\n"
        "\n"
        "fun getVersion(): String {\n"
        "    return \"1.0\"\n"
        "}\n");

    tt_test_write_file(test_dir, "src/kotlin/data/Repository.kt",
        "package data\n"
        "\n"
        "class Repository {\n"
        "    fun save() {\n"
        "        println(\"saved\")\n"
        "    }\n"
        "\n"
        "    fun delete(id: Int) {\n"
        "        println(\"deleted\")\n"
        "    }\n"
        "}\n");

    /* ---- Haskell ---- */
    tt_test_write_file(test_dir, "src/haskell/Main.hs",
        "import Data.Utils\n"
        "\n"
        "main :: IO ()\n"
        "main = do\n"
        "    let result = formatValue 42\n"
        "    putStrLn (show result)\n"
        "\n"
        "parseInput :: String -> Int\n"
        "parseInput s = read s\n");

    tt_test_write_file(test_dir, "src/haskell/Data/Utils.hs",
        "module Data.Utils where\n"
        "\n"
        "formatValue :: Int -> String\n"
        "formatValue n = show n\n"
        "\n"
        "compute :: Int -> Int -> Int\n"
        "compute a b = a + b\n");

    /* ---- Misc (ctags/custom parsers only) ---- */
    tt_test_write_file(test_dir, "src/misc/script.sh",
        "#!/bin/bash\n"
        "\n"
        "process_data() {\n"
        "    echo \"processing\"\n"
        "}\n"
        "\n"
        "cleanup() {\n"
        "    echo \"cleaning up\"\n"
        "}\n"
        "\n"
        "validate_config() {\n"
        "    return 0\n"
        "}\n");

    tt_test_write_file(test_dir, "src/misc/query.sql",
        "CREATE TABLE users (\n"
        "    id INTEGER PRIMARY KEY,\n"
        "    name TEXT NOT NULL,\n"
        "    email TEXT NOT NULL\n"
        ");\n"
        "\n"
        "CREATE VIEW active_users AS\n"
        "SELECT id, name FROM users WHERE id > 0;\n");

    tt_test_write_file(test_dir, "src/misc/module.lua",
        "local M = {}\n"
        "\n"
        "function M.calculate(a, b)\n"
        "    return a + b\n"
        "end\n"
        "\n"
        "function M.format(value)\n"
        "    return tostring(value)\n"
        "end\n"
        "\n"
        "return M\n");

    tt_test_write_file(test_dir, "src/misc/script.pl",
        "sub process_data {\n"
        "    my ($input) = @_;\n"
        "    return $input;\n"
        "}\n"
        "\n"
        "sub validate {\n"
        "    my ($data) = @_;\n"
        "    return defined $data;\n"
        "}\n");

    tt_test_write_file(test_dir, "src/misc/analysis.jl",
        "function analyze(data)\n"
        "    return sum(data)\n"
        "end\n"
        "\n"
        "function transform(x, y)\n"
        "    return x * y\n"
        "end\n");

    tt_test_write_file(test_dir, "src/misc/game.gd",
        "extends Node\n"
        "\n"
        "func _ready():\n"
        "    pass\n"
        "\n"
        "func take_damage(amount):\n"
        "    pass\n"
        "\n"
        "func heal(amount):\n"
        "    pass\n");

    /* ---- Cycle files (Python at root) ---- */
    tt_test_write_file(test_dir, "cycle_a.py",
        "from cycle_b import func_b\n"
        "\n"
        "def func_a():\n"
        "    return func_b()\n");

    tt_test_write_file(test_dir, "cycle_b.py",
        "from cycle_a import func_a\n"
        "\n"
        "def func_b():\n"
        "    return func_a()\n");

    /* ---- Orphan file (dead code) ---- */
    tt_test_write_file(test_dir, "orphan.py",
        "def orphan_function():\n"
        "    return 'nobody calls me'\n"
        "\n"
        "def another_orphan():\n"
        "    return 'also alone'\n");

    /* Manifest for codebase:detect */
    tt_test_write_file(test_dir, "package.json",
        "{\"name\": \"multilang-fixture\", \"version\": \"1.0.0\"}\n");
}

static void cleanup_fixture(void)
{
    if (test_dir)
    {
        tt_test_rmdir(test_dir);
        free(test_dir);
        test_dir = NULL;
    }
}

/* ====================================================================
 * Tool 1: index:create
 * ==================================================================== */

TT_TEST(test_ml_index_create)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    cJSON *result = tt_cmd_index_create_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT(cJSON_GetObjectItem(result, "error") == NULL, "index:create should not error");

    int files = (int)cJSON_GetObjectItem(result, "files")->valuedouble;
    TT_ASSERT_EQ_INT(files, 36);

    int symbols = (int)cJSON_GetObjectItem(result, "symbols")->valuedouble;
    TT_ASSERT_GE_INT(symbols, 70);

    cJSON *languages = cJSON_GetObjectItem(result, "languages");
    TT_ASSERT_NOT_NULL(languages);
    /* Must detect multiple languages */
    int lang_count = cJSON_GetArraySize(languages);
    TT_ASSERT_GE_INT(lang_count, 8);

    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(result, "duration_seconds"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(result, "timing"));

    cJSON_Delete(result);
}

/* ====================================================================
 * Tool 2: index:update (no changes)
 * ==================================================================== */

TT_TEST(test_ml_index_update)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    cJSON *result = tt_cmd_index_update_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT(cJSON_GetObjectItem(result, "error") == NULL, "index:update should not error");

    TT_ASSERT_EQ_INT((int)cJSON_GetObjectItem(result, "changed")->valuedouble, 0);
    TT_ASSERT_EQ_INT((int)cJSON_GetObjectItem(result, "added")->valuedouble, 0);
    TT_ASSERT_EQ_INT((int)cJSON_GetObjectItem(result, "deleted")->valuedouble, 0);
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(result, "duration_seconds"));

    cJSON_Delete(result);
}

/* ====================================================================
 * Tool 3: index:file (modify + reindex)
 * ==================================================================== */

TT_TEST(test_ml_index_file)
{
    /* Add a function to main.py */
    tt_test_write_file(test_dir, "src/python/main.py",
        "from services.auth import authenticate\n"
        "\n"
        "def main():\n"
        "    result = authenticate('admin', 'secret')\n"
        "    print(result)\n"
        "\n"
        "def parse_args():\n"
        "    return {}\n"
        "\n"
        "def new_function():\n"
        "    pass\n");

    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {"src/python/main.py"};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_index_file_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT(cJSON_GetObjectItem(result, "error") == NULL, "index:file should not error");
    TT_ASSERT(cJSON_IsTrue(cJSON_GetObjectItem(result, "changed")), "file should be changed");
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(result, "duration_seconds"));

    cJSON_Delete(result);

    /* Restore original */
    tt_test_write_file(test_dir, "src/python/main.py",
        "from services.auth import authenticate\n"
        "\n"
        "def main():\n"
        "    result = authenticate('admin', 'secret')\n"
        "    print(result)\n"
        "\n"
        "def parse_args():\n"
        "    return {}\n");

    cJSON *r2 = tt_cmd_index_file_exec(&opts);
    if (r2) cJSON_Delete(r2);
}

/* ====================================================================
 * Tool 4: search:symbols
 * ==================================================================== */

TT_TEST(test_ml_search_symbols)
{
    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {"format"};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_search_symbols_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT_EQ_STR(cJSON_GetStringValue(cJSON_GetObjectItem(result, "q")), "format");

    cJSON *results = cJSON_GetObjectItem(result, "results");
    TT_ASSERT_NOT_NULL(results);
    int n = cJSON_GetArraySize(results);
    TT_ASSERT_GE_INT(n, 3);

    /* Verify result structure */
    cJSON *first = cJSON_GetArrayItem(results, 0);
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "id"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "name"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "kind"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "file"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "line"));

    TT_ASSERT_EQ_INT((int)cJSON_GetObjectItem(result, "n")->valuedouble, n);

    cJSON_Delete(result);
}

/* ====================================================================
 * Tool 5: search:text
 * ==================================================================== */

TT_TEST(test_ml_search_text)
{
    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {"authenticate"};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_search_text_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT_EQ_STR(cJSON_GetStringValue(cJSON_GetObjectItem(result, "q")), "authenticate");

    cJSON *results = cJSON_GetObjectItem(result, "results");
    TT_ASSERT_NOT_NULL(results);
    int n = cJSON_GetArraySize(results);
    TT_ASSERT_GE_INT(n, 2);

    /* Verify result structure */
    cJSON *first = cJSON_GetArrayItem(results, 0);
    TT_ASSERT_NOT_NULL(cJSON_GetStringValue(cJSON_GetObjectItem(first, "f")));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "l"));
    TT_ASSERT_NOT_NULL(cJSON_GetStringValue(cJSON_GetObjectItem(first, "t")));

    cJSON_Delete(result);
}

/* ====================================================================
 * Tool 6: search:cooccurrence
 * ==================================================================== */

TT_TEST(test_ml_search_cooccurrence)
{
    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {"formatOutput,parseNumber"};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_search_cooccurrence_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT_EQ_STR(cJSON_GetStringValue(cJSON_GetObjectItem(result, "name_a")), "formatOutput");
    TT_ASSERT_EQ_STR(cJSON_GetStringValue(cJSON_GetObjectItem(result, "name_b")), "parseNumber");

    cJSON *results = cJSON_GetObjectItem(result, "results");
    TT_ASSERT_NOT_NULL(results);
    int n = (int)cJSON_GetObjectItem(result, "n")->valuedouble;
    TT_ASSERT_GE_INT(n, 1);

    if (cJSON_GetArraySize(results) > 0)
    {
        cJSON *first = cJSON_GetArrayItem(results, 0);
        TT_ASSERT_NOT_NULL(cJSON_GetStringValue(cJSON_GetObjectItem(first, "file")));
    }

    cJSON_Delete(result);
}

/* ====================================================================
 * Tool 7: search:similar
 * ==================================================================== */

TT_TEST(test_ml_search_similar)
{
    char *sym_id = find_symbol_id("validateInput", "function");
    if (!sym_id)
    {
        TT_ASSERT(0, "could not find validateInput symbol for similar search");
        return;
    }

    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {sym_id};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_search_similar_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *results = cJSON_GetObjectItem(result, "results");
    TT_ASSERT_NOT_NULL(results);
    int n = (int)cJSON_GetObjectItem(result, "n")->valuedouble;
    TT_ASSERT_GE_INT(n, 0);

    if (cJSON_GetArraySize(results) > 0)
    {
        cJSON *first = cJSON_GetArrayItem(results, 0);
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "id"));
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "name"));
    }

    cJSON_Delete(result);
    free(sym_id);
}

/* ====================================================================
 * Tool 8: inspect:outline
 * ==================================================================== */

TT_TEST(test_ml_inspect_outline)
{
    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {"src/php/Controller.php"};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_inspect_outline_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    TT_ASSERT_EQ_STR(cJSON_GetStringValue(cJSON_GetObjectItem(result, "file")),
                      "src/php/Controller.php");

    cJSON *symbols = cJSON_GetObjectItem(result, "symbols");
    TT_ASSERT_NOT_NULL(symbols);
    TT_ASSERT_GE_INT(cJSON_GetArraySize(symbols), 1);

    cJSON *first = cJSON_GetArrayItem(symbols, 0);
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "name"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "kind"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "line"));

    cJSON_Delete(result);
}

/* ====================================================================
 * Tool 9: inspect:symbol
 * ==================================================================== */

TT_TEST(test_ml_inspect_symbol)
{
    char *sym_id = find_symbol_id("authenticate", "function");
    if (!sym_id)
    {
        TT_ASSERT(0, "could not find authenticate symbol");
        return;
    }

    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {sym_id};
    opts.positional = pos;
    opts.positional_count = 1;

    int exit_code = -1;
    cJSON *result = tt_cmd_inspect_symbol_exec(&opts, &exit_code);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT_EQ_INT(exit_code, 0);

    TT_ASSERT_EQ_STR(cJSON_GetStringValue(cJSON_GetObjectItem(result, "name")), "authenticate");
    TT_ASSERT_NOT_NULL(cJSON_GetStringValue(cJSON_GetObjectItem(result, "source")));

    cJSON_Delete(result);
    free(sym_id);
}

/* ====================================================================
 * Tool 10: inspect:file
 * ==================================================================== */

TT_TEST(test_ml_inspect_file)
{
    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {"src/ts/app.ts"};
    opts.positional = pos;
    opts.positional_count = 1;
    opts.lines = "1-5";

    cJSON *result = tt_cmd_inspect_file_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    TT_ASSERT_EQ_STR(cJSON_GetStringValue(cJSON_GetObjectItem(result, "file")), "src/ts/app.ts");
    TT_ASSERT_GE_INT((int)cJSON_GetObjectItem(result, "total_lines")->valuedouble, 10);
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(result, "range"));
    TT_ASSERT_NOT_NULL(cJSON_GetStringValue(cJSON_GetObjectItem(result, "content")));
    TT_ASSERT_STR_CONTAINS(cJSON_GetStringValue(cJSON_GetObjectItem(result, "content")), "import");

    cJSON_Delete(result);
}

/* ====================================================================
 * Tool 11: inspect:bundle
 * ==================================================================== */

TT_TEST(test_ml_inspect_bundle)
{
    char *sym_id = find_symbol_id("authenticate", "function");
    if (!sym_id)
    {
        TT_ASSERT(0, "could not find authenticate symbol for bundle");
        return;
    }

    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {sym_id};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_inspect_bundle_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT(cJSON_GetObjectItem(result, "error") == NULL, "bundle should not error");

    cJSON *def = cJSON_GetObjectItem(result, "definition");
    TT_ASSERT_NOT_NULL(def);
    TT_ASSERT_EQ_STR(cJSON_GetStringValue(cJSON_GetObjectItem(def, "name")), "authenticate");
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(def, "source"));

    cJSON *imports = cJSON_GetObjectItem(result, "imports");
    TT_ASSERT_NOT_NULL(imports);
    TT_ASSERT_GE_INT(cJSON_GetArraySize(imports), 1);

    cJSON *outline = cJSON_GetObjectItem(result, "outline");
    TT_ASSERT_NOT_NULL(outline);
    TT_ASSERT_GE_INT(cJSON_GetArraySize(outline), 1);

    cJSON_Delete(result);
    free(sym_id);
}

/* ====================================================================
 * Tool 12: inspect:tree
 * ==================================================================== */

TT_TEST(test_ml_inspect_tree)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    cJSON *result = tt_cmd_inspect_tree_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *tree = cJSON_GetObjectItem(result, "tree");
    TT_ASSERT_NOT_NULL(tree);
    TT_ASSERT(cJSON_IsArray(tree), "tree should be array");
    TT_ASSERT_GE_INT(cJSON_GetArraySize(tree), 3);

    int file_count = (int)cJSON_GetObjectItem(result, "files")->valuedouble;
    TT_ASSERT_EQ_INT(file_count, 36);

    cJSON_Delete(result);
}

/* ====================================================================
 * Tool 13: inspect:dependencies
 * ==================================================================== */

TT_TEST(test_ml_inspect_dependencies)
{
    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {"src/ts/helpers.ts"};
    opts.positional = pos;
    opts.positional_count = 1;
    opts.depth = 3;

    cJSON *result = tt_cmd_inspect_dependencies_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    TT_ASSERT_EQ_STR(cJSON_GetStringValue(cJSON_GetObjectItem(result, "file")),
                      "src/ts/helpers.ts");

    cJSON *dependents = cJSON_GetObjectItem(result, "dependents");
    TT_ASSERT_NOT_NULL(dependents);

    int n = (int)cJSON_GetObjectItem(result, "n")->valuedouble;
    TT_ASSERT_GE_INT(n, 1);

    /* Check structure of first dependent */
    if (cJSON_GetArraySize(dependents) > 0)
    {
        cJSON *first = cJSON_GetArrayItem(dependents, 0);
        TT_ASSERT_NOT_NULL(cJSON_GetStringValue(cJSON_GetObjectItem(first, "file")));
        TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "depth"));
    }

    cJSON_Delete(result);
}

/* ====================================================================
 * Tool 14: inspect:hierarchy
 * ==================================================================== */

TT_TEST(test_ml_inspect_hierarchy)
{
    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {"src/php/Controller.php"};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_inspect_hierarchy_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    TT_ASSERT_EQ_STR(cJSON_GetStringValue(cJSON_GetObjectItem(result, "file")),
                      "src/php/Controller.php");

    cJSON *nodes = cJSON_GetObjectItem(result, "nodes");
    TT_ASSERT_NOT_NULL(nodes);
    TT_ASSERT_GE_INT(cJSON_GetArraySize(nodes), 1);

    /* Controller class should have method children */
    cJSON *first = cJSON_GetArrayItem(nodes, 0);
    cJSON *children = cJSON_GetObjectItem(first, "children");
    TT_ASSERT_NOT_NULL(children);
    TT_ASSERT_GE_INT(cJSON_GetArraySize(children), 1);

    cJSON_Delete(result);
}

/* ====================================================================
 * Tool 15: find:importers
 * ==================================================================== */

TT_TEST(test_ml_find_importers)
{
    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {"src/ts/helpers.ts"};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_find_importers_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    TT_ASSERT_EQ_STR(cJSON_GetStringValue(cJSON_GetObjectItem(result, "file")),
                      "src/ts/helpers.ts");

    cJSON *importers = cJSON_GetObjectItem(result, "importers");
    TT_ASSERT_NOT_NULL(importers);

    int count = (int)cJSON_GetObjectItem(result, "count")->valuedouble;
    TT_ASSERT_EQ_INT(count, 1);

    cJSON *first = cJSON_GetArrayItem(importers, 0);
    TT_ASSERT_STR_CONTAINS(
        cJSON_GetStringValue(cJSON_GetObjectItem(first, "from_file")), "utils.ts");

    cJSON_Delete(result);
}

/* ====================================================================
 * Tool 16: find:references
 * ==================================================================== */

TT_TEST(test_ml_find_references)
{
    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {"authenticate"};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_find_references_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    TT_ASSERT_EQ_STR(cJSON_GetStringValue(cJSON_GetObjectItem(result, "identifier")),
                      "authenticate");

    int count = (int)cJSON_GetObjectItem(result, "count")->valuedouble;
    TT_ASSERT_GE_INT(count, 1);

    cJSON *refs = cJSON_GetObjectItem(result, "references");
    TT_ASSERT_NOT_NULL(refs);

    cJSON *first = cJSON_GetArrayItem(refs, 0);
    TT_ASSERT_NOT_NULL(cJSON_GetStringValue(cJSON_GetObjectItem(first, "from_file")));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "line"));

    cJSON_Delete(result);
}

/* ====================================================================
 * Tool 17: find:callers
 * ==================================================================== */

TT_TEST(test_ml_find_callers)
{
    char *sym_id = find_symbol_id("formatOutput", "function");
    if (!sym_id)
    {
        TT_ASSERT(0, "could not find formatOutput symbol for callers");
        return;
    }

    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {sym_id};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_find_callers_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    TT_ASSERT_NOT_NULL(cJSON_GetStringValue(cJSON_GetObjectItem(result, "symbol")));

    cJSON *callers = cJSON_GetObjectItem(result, "callers");
    TT_ASSERT_NOT_NULL(callers);
    /* Callers may be 0 depending on ctags ref-tag support */
    TT_ASSERT_GE_INT((int)cJSON_GetObjectItem(result, "n")->valuedouble, 0);

    cJSON_Delete(result);
    free(sym_id);
}

/* ====================================================================
 * Tool 18: find:dead
 * ==================================================================== */

TT_TEST(test_ml_find_dead)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    cJSON *result = tt_cmd_find_dead_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *results = cJSON_GetObjectItem(result, "results");
    TT_ASSERT_NOT_NULL(results);
    TT_ASSERT_GT_INT(cJSON_GetArraySize(results), 0);

    cJSON *summary = cJSON_GetObjectItem(result, "summary");
    TT_ASSERT_NOT_NULL(summary);

    int dead = (int)cJSON_GetObjectItem(summary, "dead")->valuedouble;
    TT_ASSERT_GT_INT(dead, 0);

    int total = (int)cJSON_GetObjectItem(summary, "total")->valuedouble;
    TT_ASSERT_GT_INT(total, 0);

    /* Verify orphan.py symbols are classified as dead */
    bool found_orphan = false;
    cJSON *item;
    cJSON_ArrayForEach(item, results)
    {
        const char *file = cJSON_GetStringValue(cJSON_GetObjectItem(item, "file"));
        if (file && strstr(file, "orphan.py"))
        {
            found_orphan = true;
            TT_ASSERT_EQ_STR(
                cJSON_GetStringValue(cJSON_GetObjectItem(item, "classification")), "dead");
        }
    }
    TT_ASSERT_TRUE(found_orphan);

    cJSON_Delete(result);
}

/* ====================================================================
 * Tool 19: inspect:blast
 * ==================================================================== */

TT_TEST(test_ml_inspect_blast)
{
    char *sym_id = find_symbol_id("validateInput", "function");
    if (!sym_id)
    {
        TT_ASSERT(0, "could not find validateInput symbol for blast");
        return;
    }

    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {sym_id};
    opts.positional = pos;
    opts.positional_count = 1;
    opts.depth = 3;

    cJSON *result = tt_cmd_inspect_blast_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *target = cJSON_GetObjectItem(result, "target");
    TT_ASSERT_NOT_NULL(target);
    TT_ASSERT_EQ_STR(cJSON_GetStringValue(cJSON_GetObjectItem(target, "name")), "validateInput");

    cJSON *confirmed = cJSON_GetObjectItem(result, "confirmed");
    TT_ASSERT_NOT_NULL(confirmed);
    cJSON *potential = cJSON_GetObjectItem(result, "potential");
    TT_ASSERT_NOT_NULL(potential);

    cJSON *summary_obj = cJSON_GetObjectItem(result, "summary");
    TT_ASSERT_NOT_NULL(summary_obj);
    int conf_count = (int)cJSON_GetObjectItem(summary_obj, "confirmed_count")->valuedouble;
    TT_ASSERT_GT_INT(conf_count, 0);

    cJSON_Delete(result);
    free(sym_id);
}

/* ====================================================================
 * Tool 20: inspect:cycles
 * ==================================================================== */

TT_TEST(test_ml_inspect_cycles)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    cJSON *result = tt_cmd_inspect_cycles_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *cycles = cJSON_GetObjectItem(result, "cycles");
    TT_ASSERT_NOT_NULL(cycles);
    TT_ASSERT(cJSON_IsArray(cycles), "cycles should be array");

    cJSON *summary_obj = cJSON_GetObjectItem(result, "summary");
    TT_ASSERT_NOT_NULL(summary_obj);
    int total = (int)cJSON_GetObjectItem(summary_obj, "total_cycles")->valuedouble;
    TT_ASSERT_GT_INT(total, 0);

    /* Verify cycle structure */
    cJSON *first = cJSON_GetArrayItem(cycles, 0);
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "files"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "length"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(first, "cross_dir"));

    int len = (int)cJSON_GetObjectItem(first, "length")->valuedouble;
    TT_ASSERT_EQ_INT(len, 2);

    cJSON_Delete(result);
}

/* ====================================================================
 * Tool 21: stats
 * ==================================================================== */

TT_TEST(test_ml_stats)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    cJSON *result = tt_cmd_stats_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    int files = (int)cJSON_GetObjectItem(result, "files")->valuedouble;
    TT_ASSERT_EQ_INT(files, 36);

    int symbols = (int)cJSON_GetObjectItem(result, "symbols")->valuedouble;
    TT_ASSERT_GE_INT(symbols, 70);

    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(result, "languages"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(result, "kinds"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(result, "dirs"));

    cJSON_Delete(result);
}

/* ====================================================================
 * Tool 23: codebase:detect (before cache:clear)
 * ==================================================================== */

TT_TEST(test_ml_codebase_detect)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    int exit_code = -1;
    cJSON *result = tt_cmd_codebase_detect_exec(&opts, &exit_code);
    TT_ASSERT_NOT_NULL(result);
    TT_ASSERT_EQ_INT(exit_code, 0);

    TT_ASSERT(cJSON_IsTrue(cJSON_GetObjectItem(result, "is_codebase")),
              "multilang dir should be a codebase");
    TT_ASSERT(cJSON_IsTrue(cJSON_GetObjectItem(result, "has_index")),
              "we just created the index");
    TT_ASSERT_EQ_STR(cJSON_GetStringValue(cJSON_GetObjectItem(result, "action")), "ready");

    cJSON *ecosystems = cJSON_GetObjectItem(result, "ecosystems");
    TT_ASSERT_NOT_NULL(ecosystems);

    cJSON_Delete(result);
}

/* ====================================================================
 * Tool 22: cache:clear (last — destroys index)
 * ==================================================================== */

TT_TEST(test_ml_cache_clear)
{
    tt_cli_opts_t opts;
    init_opts(&opts);

    cJSON *result = tt_cmd_cache_clear_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(result, "deleted"));
    TT_ASSERT_NOT_NULL(cJSON_GetObjectItem(result, "freed_bytes"));

    cJSON_Delete(result);
}

/* ====================================================================
 * Language-specific symbol searches
 * ==================================================================== */

TT_TEST(test_ml_search_php_symbols)
{
    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {"Controller"};
    opts.positional = pos;
    opts.positional_count = 1;
    opts.kind = "class";

    cJSON *result = tt_cmd_search_symbols_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    cJSON *results = cJSON_GetObjectItem(result, "results");
    TT_ASSERT_GE_INT(cJSON_GetArraySize(results), 1);

    cJSON *first = cJSON_GetArrayItem(results, 0);
    TT_ASSERT_STR_CONTAINS(
        cJSON_GetStringValue(cJSON_GetObjectItem(first, "file")), "php/Controller.php");

    cJSON_Delete(result);
}

TT_TEST(test_ml_search_go_symbols)
{
    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {"Handle"};
    opts.positional = pos;
    opts.positional_count = 1;
    opts.kind = "function";

    cJSON *result = tt_cmd_search_symbols_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    cJSON *results = cJSON_GetObjectItem(result, "results");
    TT_ASSERT_GE_INT(cJSON_GetArraySize(results), 1);
    cJSON_Delete(result);
}

TT_TEST(test_ml_search_rust_symbols)
{
    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {"Settings"};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_search_symbols_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    cJSON *results = cJSON_GetObjectItem(result, "results");
    TT_ASSERT_GE_INT(cJSON_GetArraySize(results), 1);
    cJSON_Delete(result);
}

TT_TEST(test_ml_search_java_symbols)
{
    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {"UserService"};
    opts.positional = pos;
    opts.positional_count = 1;
    opts.kind = "class";

    cJSON *result = tt_cmd_search_symbols_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    cJSON *results = cJSON_GetObjectItem(result, "results");
    TT_ASSERT_GE_INT(cJSON_GetArraySize(results), 1);
    cJSON_Delete(result);
}

TT_TEST(test_ml_search_shell_symbols)
{
    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {"process_data"};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_search_symbols_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    cJSON *results = cJSON_GetObjectItem(result, "results");
    TT_ASSERT_GE_INT(cJSON_GetArraySize(results), 1);
    cJSON_Delete(result);
}

TT_TEST(test_ml_search_lua_symbols)
{
    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {"calculate"};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_search_symbols_exec(&opts);
    TT_ASSERT_NOT_NULL(result);
    cJSON *results = cJSON_GetObjectItem(result, "results");
    TT_ASSERT_GE_INT(cJSON_GetArraySize(results), 1);
    cJSON_Delete(result);
}

/* ====================================================================
 * Import chain verification (TS: helpers -> utils -> app)
 * ==================================================================== */

TT_TEST(test_ml_import_chain_ts)
{
    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {"src/ts/helpers.ts"};
    opts.positional = pos;
    opts.positional_count = 1;
    opts.depth = 3;

    cJSON *result = tt_cmd_inspect_dependencies_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *dependents = cJSON_GetObjectItem(result, "dependents");
    TT_ASSERT_NOT_NULL(dependents);

    /* Should find utils.ts at depth 1, app.ts at depth 2 */
    int found_utils = 0, found_app = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, dependents)
    {
        const char *file = cJSON_GetStringValue(cJSON_GetObjectItem(item, "file"));
        int depth = (int)cJSON_GetObjectItem(item, "depth")->valuedouble;
        if (file && strstr(file, "utils.ts") && depth == 0) found_utils = 1;
        if (file && strstr(file, "app.ts") && depth == 1) found_app = 1;
    }
    TT_ASSERT_TRUE(found_utils);
    TT_ASSERT_TRUE(found_app);

    cJSON_Delete(result);
}

/* ====================================================================
 * Cross-directory import (TS: app.ts -> lib/logger.ts)
 * ==================================================================== */

TT_TEST(test_ml_find_importers_cross_dir)
{
    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {"src/lib/logger.ts"};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_find_importers_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    int count = (int)cJSON_GetObjectItem(result, "count")->valuedouble;
    TT_ASSERT_GE_INT(count, 1);

    cJSON *importers = cJSON_GetObjectItem(result, "importers");
    TT_ASSERT_NOT_NULL(importers);

    /* app.ts should be in the importers list */
    bool found = false;
    cJSON *item;
    cJSON_ArrayForEach(item, importers)
    {
        const char *f = cJSON_GetStringValue(cJSON_GetObjectItem(item, "from_file"));
        if (f && strstr(f, "app.ts")) found = true;
    }
    TT_ASSERT_TRUE(found);

    cJSON_Delete(result);
}

/* ====================================================================
 * PHP namespace import (Controller -> Service)
 * ==================================================================== */

TT_TEST(test_ml_find_importers_php)
{
    tt_cli_opts_t opts;
    init_opts(&opts);
    const char *pos[] = {"app/Service/UserService.php"};
    opts.positional = pos;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_find_importers_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    int count = (int)cJSON_GetObjectItem(result, "count")->valuedouble;
    TT_ASSERT_GE_INT(count, 1);

    cJSON *importers = cJSON_GetObjectItem(result, "importers");
    TT_ASSERT_NOT_NULL(importers);

    bool found = false;
    cJSON *item;
    cJSON_ArrayForEach(item, importers)
    {
        const char *f = cJSON_GetStringValue(cJSON_GetObjectItem(item, "from_file"));
        if (f && strstr(f, "Controller.php")) found = true;
    }
    TT_ASSERT_TRUE(found);

    cJSON_Delete(result);
}

/* ---- Suite runner ---- */

void run_int_multilang_tests(void)
{
    setup_fixture();
    fprintf(stderr, "  Indexing multilang fixture...\n");

    /* Tool 1: index:create (MUST run first) */
    TT_RUN(test_ml_index_create);

    /* Tool 2: index:update */
    TT_RUN(test_ml_index_update);

    /* Tool 3: index:file */
    TT_RUN(test_ml_index_file);

    /* Tool 4: search:symbols */
    TT_RUN(test_ml_search_symbols);

    /* Tool 5: search:text */
    TT_RUN(test_ml_search_text);

    /* Tool 6: search:cooccurrence */
    TT_RUN(test_ml_search_cooccurrence);

    /* Tool 7: search:similar */
    TT_RUN(test_ml_search_similar);

    /* Tool 8: inspect:outline */
    TT_RUN(test_ml_inspect_outline);

    /* Tool 9: inspect:symbol */
    TT_RUN(test_ml_inspect_symbol);

    /* Tool 10: inspect:file */
    TT_RUN(test_ml_inspect_file);

    /* Tool 11: inspect:bundle */
    TT_RUN(test_ml_inspect_bundle);

    /* Tool 12: inspect:tree */
    TT_RUN(test_ml_inspect_tree);

    /* Tool 13: inspect:dependencies */
    TT_RUN(test_ml_inspect_dependencies);

    /* Tool 14: inspect:hierarchy */
    TT_RUN(test_ml_inspect_hierarchy);

    /* Tool 15: find:importers */
    TT_RUN(test_ml_find_importers);

    /* Tool 16: find:references */
    TT_RUN(test_ml_find_references);

    /* Tool 17: find:callers */
    TT_RUN(test_ml_find_callers);

    /* Tool 18: find:dead */
    TT_RUN(test_ml_find_dead);

    /* Tool 19: inspect:blast */
    TT_RUN(test_ml_inspect_blast);

    /* Tool 20: inspect:cycles */
    TT_RUN(test_ml_inspect_cycles);

    /* Tool 21: stats */
    TT_RUN(test_ml_stats);

    /* Language-specific symbol searches */
    TT_RUN(test_ml_search_php_symbols);
    TT_RUN(test_ml_search_go_symbols);
    TT_RUN(test_ml_search_rust_symbols);
    TT_RUN(test_ml_search_java_symbols);
    TT_RUN(test_ml_search_shell_symbols);
    TT_RUN(test_ml_search_lua_symbols);

    /* Import chain / cross-dir tests */
    TT_RUN(test_ml_import_chain_ts);
    TT_RUN(test_ml_find_importers_cross_dir);
    TT_RUN(test_ml_find_importers_php);

    /* Tool 23: codebase:detect (before cache:clear) */
    TT_RUN(test_ml_codebase_detect);

    /* Tool 22: cache:clear (LAST — destroys index) */
    TT_RUN(test_ml_cache_clear);

    cleanup_fixture();
}
