/*
 * PC stand-in for the 3DS: runs the real streaming core against synthetic
 * framebuffers stored exactly the way the 3DS stores them (sideways lines,
 * GSP pixel formats), so the browser viewer can be tested end to end.
 *
 *   ./3dslink-host [port]
 *   TDSL_TOP_FORMAT / TDSL_BOTTOM_FORMAT = 0..4 (GSP format, default 1 and 2)
 *   TDSL_BOTTOM_STATIC=1 keeps the bottom screen still
 */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/core.h"
#include "core/server.h"

static volatile sig_atomic_t stop_requested;
static int formats[SCREEN_COUNT] = { PIX_BGR8, PIX_RGB565 };
static bool bottom_static;
static bool bottom_broken; /* capture of the bottom screen always fails */
static uint8_t *fb[SCREEN_COUNT];
static const int lines_of[SCREEN_COUNT] = { 400, 320 };

uint64_t plat_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

void plat_sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

bool plat_should_exit(void)
{
    return stop_requested != 0;
}

int plat_load_settings(char *buf, int cap)
{
    const char *path = getenv("TDSL_SETTINGS");
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f)
        return 0;
    int n = (int)fread(buf, 1, (size_t)cap, f);
    fclose(f);
    return n;
}

void plat_save_settings(const char *buf, int len)
{
    const char *path = getenv("TDSL_SETTINGS");
    FILE *f = path ? fopen(path, "wb") : NULL;
    if (f) {
        fwrite(buf, 1, (size_t)len, f);
        fclose(f);
    }
}

uint32_t plat_net_address(void)
{
    return htonl(INADDR_LOOPBACK);
}

bool plat_set_cpu_boost(bool on)
{
    (void)on;
    return false;
}

/* Store display pixel (x, y) the way the LCD framebuffer does. */
static void put(uint8_t *base, int format, int x, int y, int r, int g, int b)
{
    int bpp = pix_bytes(format);
    uint8_t *p = base + (x * 240 + (239 - y)) * bpp;
    uint16_t v;
    switch (format) {
    case PIX_RGBA8: p[0] = 0xFF; p[1] = (uint8_t)b; p[2] = (uint8_t)g; p[3] = (uint8_t)r; return;
    case PIX_BGR8:  p[0] = (uint8_t)b; p[1] = (uint8_t)g; p[2] = (uint8_t)r; return;
    case PIX_RGB565: v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); break;
    case PIX_RGB5A1: v = (uint16_t)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1); break;
    default:         v = (uint16_t)(((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | 0xF); break;
    }
    memcpy(p, &v, 2);
}

/*
 * Test card: gradient background, a solid 24x24 marker in each corner (top
 * screen: red TL, green TR, blue BL, white BR; bottom screen: magenta TL,
 * cyan TR, yellow BL, black BR) and a yellow box sliding across the middle.
 */
static void render(int s, uint64_t t)
{
    int w = lines_of[s], fmt = formats[s];
    static const int corners[SCREEN_COUNT][4][3] = {
        { { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 255 } },
        { { 255, 0, 255 }, { 0, 255, 255 }, { 255, 255, 0 }, { 0, 0, 0 } },
    };
    bool moving = s == SCREEN_TOP || !bottom_static;
    int box = moving ? (int)((t / 8) % (uint64_t)(w - 40)) : 20;
    for (int x = 0; x < w; x++) {
        for (int y = 0; y < 240; y++) {
            int r = x * 200 / w, g = y * 200 / 240, b = 90;
            int ci = -1;
            if (x < 24 && y < 24) ci = 0;
            else if (x >= w - 24 && y < 24) ci = 1;
            else if (x < 24 && y >= 216) ci = 2;
            else if (x >= w - 24 && y >= 216) ci = 3;
            if (ci >= 0) {
                r = corners[s][ci][0]; g = corners[s][ci][1]; b = corners[s][ci][2];
            } else if (x >= box && x < box + 40 && y >= 100 && y < 140) {
                r = 250; g = 220; b = 30;
            }
            put(fb[s], fmt, x, y, r, g, b);
        }
    }
}

int capture_begin(int screen, frame_geom *g)
{
    if (screen == SCREEN_BOTTOM && bottom_broken)
        return -1;
    g->format = formats[screen];
    g->width = 240;
    g->lines = lines_of[screen];
    g->stride = 240 * pix_bytes(g->format);
    render(screen, plat_now_ms());
    return 0;
}

int capture_lines(int screen, int first, int count, uint8_t *dst)
{
    int stride = 240 * pix_bytes(formats[screen]);
    memcpy(dst, fb[screen] + first * stride, (size_t)(count * stride));
    return 0;
}

void plat_log_write(const char *line, int len)
{
    fwrite(line, 1, (size_t)len, stderr);
}

void capture_end(int screen)
{
    (void)screen;
}

static void on_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : 8080;
    const char *e;
    if ((e = getenv("TDSL_TOP_FORMAT"))) formats[SCREEN_TOP] = atoi(e) % 5;
    if ((e = getenv("TDSL_BOTTOM_FORMAT"))) formats[SCREEN_BOTTOM] = atoi(e) % 5;
    bottom_static = (e = getenv("TDSL_BOTTOM_STATIC")) && atoi(e);
    bottom_broken = (e = getenv("TDSL_BOTTOM_BROKEN")) && atoi(e);

    for (int s = 0; s < SCREEN_COUNT; s++)
        fb[s] = calloc(1, (size_t)(400 * 240 * 4));

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* Same budget as an Old 3DS so tests exercise the strip path. */
    server_config cfg = {
        .port = port,
        .version = TDSL_VERSION,
        .model = "PC test harness",
        .max_slots = 3,
        .slot_bytes = 56 * 1024,
        .min_slot_bytes = 24 * 1024,
        .strip_bytes = 32 * 960,
        .reserve_bytes = 48 * 1024,
        .max_clients = 6,
        .max_streams = 3,
        .stream_sndbuf = 32 * 1024,
        .quality = 70,
        .fps_top = 30,
        .fps_bottom = 20,
    };
    if (server_start(&cfg) != 0)
        return 1;
    server_run();
    server_stop();
    return 0;
}
