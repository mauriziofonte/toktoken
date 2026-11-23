#include "widget.h"

int widget_label_is_empty(const Widget *w)
{
    return w && w->name[0] == '\0';
}
