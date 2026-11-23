/*
 * site.c -- Security-relevant code sites: helpers.
 */

#include "site.h"

#include <stdlib.h>
#include <string.h>

static const char *CATEGORY_STRINGS[] = {
    "sink", "source", "sanitizer", "acl", "call", "harness"};

#define CATEGORY_COUNT ((int)(sizeof(CATEGORY_STRINGS) / sizeof(CATEGORY_STRINGS[0])))

const char *tt_site_category_str(tt_site_category_e cat)
{
    if ((int)cat >= 0 && (int)cat < CATEGORY_COUNT)
        return CATEGORY_STRINGS[cat];
    return "call";
}

int tt_site_category_from_str(const char *s)
{
    if (!s)
        return -1;
    for (int i = 0; i < CATEGORY_COUNT; i++)
    {
        if (strcmp(CATEGORY_STRINGS[i], s) == 0)
            return i;
    }
    return -1;
}

void tt_site_free(tt_site_t *site)
{
    if (!site)
        return;
    free(site->file);
    free(site->name);
    free(site->qualifier);
    free(site->scope_id);
    free(site->args);
    free(site->arg_masks);
    free(site->trace);
    memset(site, 0, sizeof(*site));
}

void tt_site_array_free(tt_site_t *sites, int count)
{
    if (!sites)
        return;
    for (int i = 0; i < count; i++)
        tt_site_free(&sites[i]);
    free(sites);
}
