/*
 * test_e2e_security.c -- E2E tests for the security:* CLI commands and the
 * Perl/CGI index on the perl-project fixture (compiled binary).
 */

#include "test_framework.h"
#include "test_helpers.h"
#include "test_e2e_helpers.h"

#include <stdlib.h>
#include <string.h>

static const char *s_fx = NULL;

static int run_json(const char *args, cJSON **out)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "%s --path %s", args, s_fx);
    return tt_e2e_run(cmd, out);
}

static int jnum(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valueint : -1;
}

TT_TEST(test_e2e_sec_index)
{
    cJSON *j = NULL;
    run_json("cache:clear", NULL);
    TT_ASSERT_EQ_INT(0, run_json("index:create", &j));
    TT_ASSERT_NOT_NULL(j);
    if (j)
    {
        TT_ASSERT_EQ_INT(9, jnum(j, "files"));
        cJSON *focus = cJSON_GetObjectItemCaseSensitive(j, "language_focus");
        TT_ASSERT_EQ_STR("perl", cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(focus, "profile")));
        cJSON_Delete(j);
    }
}

TT_TEST(test_e2e_sec_sinks_scope_and_taint)
{
    cJSON *j = NULL;
    TT_ASSERT_EQ_INT(0, run_json("security:sinks mymod --taint param -n", &j));
    if (j)
    {
        TT_ASSERT_EQ_INT(2, jnum(j, "count"));
        cJSON_Delete(j);
    }
    j = NULL;
    TT_ASSERT_EQ_INT(0, run_json("security:sinks \"mymod/*.cgi\" -c", &j));
    if (j)
    {
        TT_ASSERT_EQ_INT(1, jnum(j, "count"));
        cJSON *first = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(j, "results"), 0);
        TT_ASSERT_EQ_STR("mymod/save.cgi", cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(first, "f")));
        TT_ASSERT_EQ_INT(1, jnum(first, "cgi"));
        cJSON_Delete(j);
    }
}

TT_TEST(test_e2e_sec_flow_unauth)
{
    cJSON *j = NULL;
    TT_ASSERT_EQ_INT(0, run_json("security:flow --taint in --entry unauth -c", &j));
    if (j)
    {
        TT_ASSERT_EQ_INT(1, jnum(j, "count"));
        cJSON *f = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(j, "flows"), 0);
        cJSON *chain = cJSON_GetObjectItemCaseSensitive(f, "c");
        TT_ASSERT_EQ_STR("forgot.cgi:6 sink system", cJSON_GetStringValue(cJSON_GetArrayItem(chain, 1)));
        cJSON_Delete(j);
    }
}

TT_TEST(test_e2e_sec_flow_invalid_entry)
{
    cJSON *j = NULL;
    TT_ASSERT_EQ_INT(1, run_json("security:flow --entry nowhere", &j));
    if (j)
    {
        TT_ASSERT_EQ_STR("invalid_value", cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "error")));
        cJSON_Delete(j);
    }
}

TT_TEST(test_e2e_sec_defs_refs_and_detect)
{
    cJSON *j = NULL;
    TT_ASSERT_EQ_INT(0, run_json("search:symbols run_backup -n", &j));
    if (j)
    {
        TT_ASSERT_EQ_INT(1, jnum(j, "definitions"));
        TT_ASSERT_EQ_INT(2, jnum(j, "references"));
        cJSON_Delete(j);
    }
    j = NULL;
    run_json("codebase:detect", &j);
    if (j)
    {
        TT_ASSERT_EQ_STR("perl", cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "primary")));
        cJSON_Delete(j);
    }
}

TT_TEST(test_e2e_sec_help_cli_name)
{
    cJSON *j = NULL;
    TT_ASSERT_EQ_INT(0, tt_e2e_run("help security:acl-gates", &j));
    if (j)
    {
        TT_ASSERT_EQ_STR("security_acl_gates", cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "tool")));
        TT_ASSERT_EQ_STR("security:acl-gates", cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "cli")));
        cJSON_Delete(j);
    }
}

TT_TEST(test_e2e_sec_cleanup)
{
    TT_ASSERT_EQ_INT(0, run_json("cache:clear", NULL));
}

void run_e2e_security_tests(void)
{
    s_fx = tt_test_fixture("perl-project");
    if (!s_fx || !tt_e2e_binary())
    {
        TT_SKIP(test_e2e_sec_index, "perl-project fixture or binary not found");
        return;
    }
    TT_RUN(test_e2e_sec_index);
    TT_RUN(test_e2e_sec_sinks_scope_and_taint);
    TT_RUN(test_e2e_sec_flow_unauth);
    TT_RUN(test_e2e_sec_flow_invalid_entry);
    TT_RUN(test_e2e_sec_defs_refs_and_detect);
    TT_RUN(test_e2e_sec_help_cli_name);
    TT_RUN(test_e2e_sec_cleanup);
}
