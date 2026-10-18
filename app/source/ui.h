/* Top-screen drawing for the 3DSLink app (plain C so it can be previewed on a PC). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define UI_W 400
#define UI_H 240

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
