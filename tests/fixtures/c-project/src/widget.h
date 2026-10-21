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

/* Runtime configuration. */
typedef struct Config {
    int
        retries; /* declaration split across lines */
    const char *label, *fallback;
    struct {
        int low;
        int high;
    } bounds;
    int (*on_error)(struct Config *c,
                    int code,
                    const char *msg);
#ifdef DEBUG_CONFIG
    int debug_level;
#endif
    char tag[8];
} Config;

#endif
