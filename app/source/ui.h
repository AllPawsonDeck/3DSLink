/* Screen drawing for the 3DSLink app (plain C so it can be previewed on a PC). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define UI_W        400 /* top screen */
#define UI_BOTTOM_W 320
#define UI_H        240

enum { ST_CHECKING, ST_INSTALLING, ST_STARTING, ST_RUNNING, ST_STOPPED, ST_FAILED };

typedef struct {
    int state;
    int install_pct;
    const char *address; /* NULL when the 3DS has no Wi-Fi address */
    int viewers;         /* -1 when unknown */
    const char *top_fps, *bottom_fps;
    const char *version;
} ui_state;

/* Draws the whole top screen into canvas (UI_W x UI_H, RGB565, row-major)
 * using an 8x8 font (8 bytes per glyph, high bit = leftmost pixel). */
void ui_draw_top(const ui_state *ui, uint16_t *canvas, const uint8_t *font);

/* Easter egg: Clawd over an already drawn top screen. alpha 0..256 fades it
 * out; bob moves it up and down a few pixels. */
void ui_overlay_clawd(uint16_t *canvas, const uint8_t *font, int alpha, int bob);

/* ---- bottom screen ---------------------------------------------------------- */

enum { TAB_HOME, TAB_LOGS };

/* Things on the bottom screen you can tap. */
enum {
    BTN_NONE = -1,
    BTN_RESTART,
    BTN_TOGGLE,
    BTN_EXIT,
    BTN_TAB_HOME,
    BTN_TAB_LOGS,
    BTN_SCROLL_UP,
    BTN_SCROLL_DOWN,
};

#define UI_LOG_COLS    29 /* characters per log line */
#define UI_LOG_VISIBLE 18 /* log lines on screen at once */

typedef struct {
    int tab;
    bool streaming_on;  /* labels the on/off button */
    int pressed;        /* BTN_* being touched, for feedback */
    const char *const *lines; /* log lines; ones starting with '#' are headings */
    int line_count;
    int scroll;         /* index of the first visible log line */
} ui_bottom;

void ui_draw_bottom(const ui_bottom *b, uint16_t *canvas, const uint8_t *font);
int  ui_bottom_hit(const ui_bottom *b, int x, int y);
