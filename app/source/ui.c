/*
 * 3DSLink app: top screen. Everything is drawn into an off-screen canvas so
 * the app can copy a finished frame to the LCD at once (no flicker).
 */
#include <stdio.h>
#include <string.h>

#include "ui.h"

#define RGB(r, g, b) (uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))
#define C_BG     RGB(16, 20, 26)
#define C_HEADER RGB(255, 154, 31)
#define C_INK    RGB(29, 18, 6)
#define C_CARD   RGB(30, 36, 45)
#define C_TEXT   RGB(236, 240, 245)
#define C_MUTED  RGB(160, 170, 184)
#define C_GREEN  RGB(60, 207, 142)
#define C_YELLOW RGB(245, 185, 65)
#define C_RED    RGB(240, 100, 100)
#define C_CYAN   RGB(110, 220, 255)

static uint16_t *canvas;
static const uint8_t *font;

static void rect(int x, int y, int w, int h, uint16_t c)
{
    for (int yy = y < 0 ? 0 : y; yy < y + h && yy < 240; yy++)
        for (int xx = x < 0 ? 0 : x; xx < x + w && xx < 400; xx++)
            canvas[yy * 400 + xx] = c;
}

static void disc(int cx, int cy, int r, uint16_t c)
{
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r)
                rect(cx + x, cy + y, 1, 1, c);
}

static int text_w(const char *s, int scale)
{
    return (int)strlen(s) * 8 * scale;
}

/* Text using the console's 8x8 font, optionally scaled up. */
static void text(int x, int y, int scale, uint16_t c, const char *s)
{
    for (; *s; s++, x += 8 * scale) {
        const uint8_t *g = font + 8 * (uint8_t)*s;
        for (int row = 0; row < 8; row++)
            for (int col = 0; col < 8; col++)
                if (g[row] & (0x80 >> col))
                    rect(x + col * scale, y + row * scale, scale, scale, c);
    }
}

static void text_center(int y, int scale, uint16_t c, const char *s)
{
    text((400 - text_w(s, scale)) / 2, y, scale, c, s);
}

void ui_draw_top(const ui_state *ui, uint16_t *out, const uint8_t *font_gfx)
{
    canvas = out;
    font = font_gfx;

    rect(0, 0, 400, 240, C_BG);

    /* header */
    rect(0, 0, 400, 30, C_HEADER);
    text(12, 7, 2, C_INK, "3DSLink");
    char ver[16];
    snprintf(ver, sizeof(ver), "v%s", ui->version);
    text(388 - text_w(ver, 1), 11, 1, C_INK, ver);

    /* status */
    uint16_t dot = C_YELLOW;
    const char *title = "Getting ready...";
    switch (ui->state) {
    case ST_INSTALLING: title = "Setting up..."; break;
    case ST_STARTING:   title = "Starting..."; break;
    case ST_RUNNING:
        if (ui->address) {
            dot = C_GREEN;
            title = "Ready to stream!";
        } else {
            title = "Waiting for Wi-Fi";
        }
        break;
    case ST_STOPPED: dot = C_MUTED; title = "Streaming is off"; break;
    case ST_FAILED:  dot = C_RED; title = "Something went wrong"; break;
    }
    disc(24, 52, 7, dot);
    text(40, 44, 2, C_TEXT, title);

    /* main card */
    rect(10, 72, 380, 130, C_CARD);
    const int x = 24;
    if (ui->state == ST_RUNNING && ui->address) {
        text(x, 84, 1, C_TEXT, "1  On your computer, open Chrome and go to:");
        text_center(102, 2, C_CYAN, ui->address);
        text(x, 132, 1, C_TEXT, "2  Press HOME and start any game.");
        text(x, 150, 1, C_TEXT, "3  Your game shows up in Chrome!");
        text(x, 178, 1, C_MUTED, "Keep both on the same Wi-Fi network.");
    } else if (ui->state == ST_RUNNING) {
        text(x, 86, 1, C_TEXT, "Your 3DS isn't connected to Wi-Fi.");
        text(x, 108, 1, C_TEXT, "Connect it in System Settings >");
        text(x, 124, 1, C_TEXT, "Internet Settings.");
        text(x, 150, 1, C_MUTED, "This screen updates by itself.");
    } else if (ui->state == ST_INSTALLING) {
        text(x, 86, 1, C_TEXT, "Setting up the streaming service.");
        text(x, 104, 1, C_MUTED, "This only happens the first time.");
        rect(x, 132, 352, 16, C_BG);
        rect(x, 132, 352 * ui->install_pct / 100, 16, C_HEADER);
    } else if (ui->state == ST_STOPPED) {
        text(x, 92, 1, C_TEXT, "Streaming is turned off.");
        text(x, 116, 1, C_TEXT, "Press A to turn it back on.");
    } else if (ui->state == ST_FAILED) {
        text(x, 86, 1, C_TEXT, "The streaming service didn't start.");
        text(x, 110, 1, C_TEXT, "Press A to try again.");
        text(x, 140, 1, C_MUTED, "Details are on the bottom screen.");
    } else {
        text(x, 92, 1, C_TEXT, "Starting the streaming service...");
        text(x, 116, 1, C_MUTED, "This takes a few seconds.");
    }

    /* footer: who is watching */
    if (ui->state == ST_RUNNING && ui->address && ui->viewers >= 0) {
        char line[64];
        if (ui->viewers == 0)
            snprintf(line, sizeof(line), "Nobody is watching yet");
        else
            snprintf(line, sizeof(line), "Watching: %d    Top %s fps    Bottom %s fps",
                     ui->viewers, ui->top_fps, ui->bottom_fps);
        text_center(216, 1, ui->viewers ? C_GREEN : C_MUTED, line);
    }
}

