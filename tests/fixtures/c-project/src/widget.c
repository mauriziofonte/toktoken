#include "widget.h"
#include <string.h>

// Internal helper.
static int clamp(int v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

/*
 * Draw the widget on the given label.
 */
int widget_draw(Widget *w, const char *label)
{
    /* nothing to draw */
    if (!w || !label)
        return -1;
    int len = clamp((int)strlen(label), 0, w->width);
    memcpy(w->name, label, len);
    return len;
}

int widget_reset(Widget *w)
{
    if (!w)
        return -1;
    w->width = 0;
    w->height = 0;
    return 0;
}
