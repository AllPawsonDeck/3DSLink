/*
 * 3DSLink app screens. Everything is drawn into an off-screen canvas so the
 * app can copy a finished frame to the LCD at once (no flicker).
 */
#include <math.h>
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
#define C_PRESS  RGB(52, 62, 76)
#define C_CLAWD  RGB(215, 119, 87)

static uint16_t *canvas;
static int cw; /* canvas width: 400 (top) or 320 (bottom) */
static const uint8_t *font;
static int opacity = 256; /* 256 = paint, less = blend over what's there */

static uint16_t mix(uint16_t under, uint16_t over, int t)
{
    int r = ((under >> 11) * (256 - t) + (over >> 11) * t) >> 8;
    int g = (((under >> 5) & 63) * (256 - t) + ((over >> 5) & 63) * t) >> 8;
    int b = ((under & 31) * (256 - t) + (over & 31) * t) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void rect(int x, int y, int w, int h, uint16_t c)
{
    for (int yy = y < 0 ? 0 : y; yy < y + h && yy < UI_H; yy++)
        for (int xx = x < 0 ? 0 : x; xx < x + w && xx < cw; xx++) {
            uint16_t *p = &canvas[yy * cw + xx];
            *p = opacity >= 256 ? c : mix(*p, c, opacity);
        }
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
    text((cw - text_w(s, scale)) / 2, y, scale, c, s);
}

void ui_draw_top(const ui_state *ui, uint16_t *out, const uint8_t *font_gfx)
{
    canvas = out;
    cw = UI_W;
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
        text(x, 140, 1, C_MUTED, "Tap \"Logs\" on the bottom screen to see why.");
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


/* ---- Easter egg ------------------------------------------------------------ */

/* Clawd, as drawn by the block characters of the Claude Code logo
 *    ▐▛███▜▌
 *   ▝▜█████▛▘
 *     ▘▘ ▝▝
 * with each character split into its 2x2 quadrants. */
static const char *const clawd[] = {
    "...############...",
    "...##.######.##...",
    ".################.",
    "...############...",
    "....#.#....#.#....",
};

/* Small deterministic pseudo-random numbers, so confetti needs no state. */
static float rnd(unsigned i, unsigned salt)
{
    unsigned x = i * 2654435761u ^ salt * 2246822519u;
    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    return (float)(x & 0xFFFF) / 65535.0f;
}

/*
 * Confetti: two party poppers fire from the bottom corners, and more pieces
 * flutter down from the top for the whole show. Positions come straight from
 * the time (no state): a burst that air drag slows down, then a gentle fall
 * at paper speed with a side-to-side flutter.
 */
static void confetti(int t_ms)
{
    static const uint16_t colors[] = {
        RGB(255, 154, 31), RGB(110, 220, 255), RGB(250, 215, 70), RGB(60, 207, 142),
        RGB(240, 100, 150), RGB(170, 130, 255), RGB(255, 255, 255),
    };
    const unsigned ncolors = sizeof(colors) / sizeof(colors[0]);
    const float t = (float)t_ms / 1000.0f, k = 3.0f;

    for (unsigned i = 0; i < 160; i++) {
        float x0, y0, vx = 0, vy = 0, delay;
        if (i < 110) { /* poppers: left half from the left corner, rest from the right */
            bool left = i < 55;
            float ang = (left ? -82.0f + 40.0f * rnd(i, 1) : -138.0f + 40.0f * rnd(i, 1)) * 3.14159265f / 180.0f;
            float speed = 420.0f + 300.0f * rnd(i, 2);
            x0 = left ? 6.0f : (float)UI_W - 6.0f;
            y0 = (float)UI_H + 4.0f;
            vx = cosf(ang) * speed;
            vy = sinf(ang) * speed;
            delay = 0.08f * rnd(i, 3);
        } else { /* rain from the top, spread over the show */
            x0 = (float)UI_W * rnd(i, 1);
            y0 = -10.0f;
            delay = 2.4f * rnd(i, 3);
        }
        float tt = t - delay;
        if (tt < 0)
            continue;
        float slow = (1.0f - expf(-k * tt)) / k;           /* burst, slowed by air */
        float fall = (55.0f + 50.0f * rnd(i, 5)) * (tt - slow); /* then paper-speed fall */
        float x = x0 + vx * slow + 12.0f * sinf(tt * (2.0f + 2.0f * rnd(i, 6)) + (float)i);
        float y = y0 + vy * slow + fall;
        if (y > (float)UI_H + 10.0f || x < -10.0f || x > (float)UI_W + 10.0f)
            continue;
        /* tumbling: each piece flips between wide and thin */
        float spin = fabsf(cosf(tt * (5.0f + 6.0f * rnd(i, 4)) + (float)i));
        int w = 3 + (int)(5.0f * spin), h = 8 - (int)(4.0f * spin);
        rect((int)x - w / 2, (int)y - h / 2, w, h, colors[i % ncolors]);
    }
}

void ui_overlay_clawd(uint16_t *out, const uint8_t *font_gfx, int alpha, int bob, int t_ms)
{
    canvas = out;
    cw = UI_W;
    font = font_gfx;
    if (alpha <= 0)
        return;
    if (alpha > 256)
        alpha = 256;

    /* Dim the screen behind, then Clawd and a caption on top. Pixels are
     * twice as tall as wide, like the terminal cells Clawd comes from. */
    opacity = alpha * 9 / 10;
    rect(0, 0, UI_W, UI_H, RGB(8, 10, 14));
    const int px = 10, py = 20;
    const int w = 18 * px, h = 5 * py;
    const int x0 = (UI_W - w) / 2, y0 = (UI_H - h) / 2 - 14 + bob;
    opacity = alpha;
    for (int row = 0; row < 5; row++)
        for (int col = 0; col < 18; col++)
            if (clawd[row][col] == '#')
                rect(x0 + col * px, y0 + row * py, px, py, C_CLAWD);
    text_center(y0 - 30, 1, RGB(250, 215, 70), "Achievement unlocked!");
    text_center(y0 + h + 22, 1, C_TEXT, "You found Clawd!");
    confetti(t_ms); /* in front, so it flies past Clawd */
    opacity = 256;
}

/* ---- bottom screen ---------------------------------------------------------- */

#define SIDE_X   256 /* the Home/Logs tabs live in the right-hand column */
#define CONTENT  (SIDE_X)

typedef struct {
    int x, y, w, h;
} box;

static const box b_buttons[3] = { { 8, 30, 240, 52 }, { 8, 88, 240, 52 }, { 8, 146, 240, 52 } };
static const box b_tab_home = { SIDE_X, 0, 64, 120 };
static const box b_tab_logs = { SIDE_X, 120, 64, 120 };
static const box b_up = { 8, 212, 116, 24 };
static const box b_down = { 132, 212, 116, 24 };

static bool inside(const box *r, int x, int y)
{
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

int ui_bottom_hit(const ui_bottom *b, int x, int y)
{
    if (inside(&b_tab_home, x, y))
        return BTN_TAB_HOME;
    if (inside(&b_tab_logs, x, y))
        return BTN_TAB_LOGS;
    if (b->tab == TAB_HOME) {
        for (int i = 0; i < 3; i++)
            if (inside(&b_buttons[i], x, y))
                return BTN_RESTART + i;
    } else {
        if (inside(&b_up, x, y))
            return BTN_SCROLL_UP;
        if (inside(&b_down, x, y))
            return BTN_SCROLL_DOWN;
    }
    return BTN_NONE;
}

static void tab(const box *r, const char *label, const char *key, bool active, bool pressed)
{
    rect(r->x, r->y, r->w, r->h, active ? C_HEADER : pressed ? C_PRESS : C_CARD);
    rect(r->x, r->y, 1, r->h, C_BG); /* thin divider */
    uint16_t ink = active ? C_INK : C_TEXT;
    int cx = r->x + r->w / 2;
    text(cx - text_w(label, 1) / 2, r->y + r->h / 2 - 10, 1, ink, label);
    text(cx - text_w(key, 1) / 2, r->y + r->h / 2 + 6, 1, active ? C_INK : C_MUTED, key);
}

/* A big tappable button with the matching 3DS button shown on the left. */
static void action(const box *r, const char *key, const char *title, const char *hint, bool pressed)
{
    rect(r->x, r->y, r->w, r->h, pressed ? C_PRESS : C_CARD);
    int kw = text_w(key, 1) + 12;
    int ky = r->y + (r->h - 18) / 2;
    rect(r->x + 10, ky, kw, 18, C_HEADER);
    text(r->x + 16, ky + 5, 1, C_INK, key);
    int tx = r->x + 10 + kw + 12;
    text(tx, r->y + 14, 1, C_TEXT, title);
    text(tx, r->y + 30, 1, C_MUTED, hint);
}

void ui_draw_bottom(const ui_bottom *b, uint16_t *out, const uint8_t *font_gfx)
{
    canvas = out;
    cw = UI_BOTTOM_W;
    font = font_gfx;

    rect(0, 0, UI_BOTTOM_W, UI_H, C_BG);
    tab(&b_tab_home, "Home", "(L)", b->tab == TAB_HOME, b->pressed == BTN_TAB_HOME);
    tab(&b_tab_logs, "Logs", "(R)", b->tab == TAB_LOGS, b->pressed == BTN_TAB_LOGS);

    if (b->tab == TAB_HOME) {
        text(10, 9, 1, C_MUTED, "What do you want to do?");
        action(&b_buttons[0], "A", "Restart streaming", "If something seems stuck",
               b->pressed == BTN_RESTART);
        action(&b_buttons[1], "X", b->streaming_on ? "Turn streaming off" : "Turn streaming on",
               b->streaming_on ? "Stops sending video" : "Starts sending video",
               b->pressed == BTN_TOGGLE);
        action(&b_buttons[2], "START", "Close this app", "Keeps streaming",
               b->pressed == BTN_EXIT);
        text(10, 210, 1, C_MUTED, "In a game: hold START+SELECT");
        text(10, 222, 1, C_MUTED, "for 2 seconds to stop.");
        return;
    }

    /* Logs */
    text(10, 9, 1, C_TEXT, "Logs");
    char pos[24];
    int last = b->scroll + UI_LOG_VISIBLE < b->line_count ? b->scroll + UI_LOG_VISIBLE : b->line_count;
    snprintf(pos, sizeof(pos), "%d-%d of %d", b->line_count ? b->scroll + 1 : 0, last, b->line_count);
    text(CONTENT - 8 - text_w(pos, 1), 9, 1, C_MUTED, pos);
    rect(8, 24, 240, 184, C_CARD);
    if (!b->line_count)
        text(16, 32, 1, C_MUTED, "Nothing logged yet.");
    for (int i = 0; i < UI_LOG_VISIBLE && b->scroll + i < b->line_count; i++) {
        const char *line = b->lines[b->scroll + i];
        if (line[0] == '#')
            text(12, 28 + i * 10, 1, C_HEADER, line + 1);
        else
            text(12, 28 + i * 10, 1, C_TEXT, line);
    }
    const char up[] = { 0x1E, ' ', 'U', 'p', 0 }, down[] = { 0x1F, ' ', 'D', 'o', 'w', 'n', 0 };
    rect(b_up.x, b_up.y, b_up.w, b_up.h, b->pressed == BTN_SCROLL_UP ? C_PRESS : C_CARD);
    rect(b_down.x, b_down.y, b_down.w, b_down.h, b->pressed == BTN_SCROLL_DOWN ? C_PRESS : C_CARD);
    text(b_up.x + (b_up.w - text_w(up, 1)) / 2, b_up.y + 8, 1, C_TEXT, up);
    text(b_down.x + (b_down.w - text_w(down, 1)) / 2, b_down.y + 8, 1, C_TEXT, down);
}
