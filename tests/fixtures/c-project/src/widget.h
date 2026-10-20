/* Widget public API. */
#ifndef WIDGET_H
#define WIDGET_H

typedef struct Widget {
    int width;
    int height;
    int (*draw)(struct Widget *w, const char *label);
    char name[32];
} Widget;

typedef enum Mode {
    MODE_IDLE,
    MODE_ACTIVE
} Mode;

/* Draw the widget. */
int widget_draw(Widget *w, const char *label);
int widget_reset(Widget *w);

#endif
