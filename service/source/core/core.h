/*
 * 3DSLink streaming core.
 *
 * Everything in core/ is plain C99 + BSD sockets so it builds both for the
 * 3DS (devkitARM + libctru) and for a normal PC, where the host test harness
 * feeds it synthetic framebuffers. Platform code supplies the functions
 * declared in the "platform" section below.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SCREEN_TOP    0
#define SCREEN_BOTTOM 1
#define SCREEN_COUNT  2

/* Framebuffer pixel formats, numbered exactly like GSPGPU_FramebufferFormat. */
enum {
    PIX_RGBA8  = 0,
    PIX_BGR8   = 1,
    PIX_RGB565 = 2,
    PIX_RGB5A1 = 3,
    PIX_RGBA4  = 4,
};

/*
 * Geometry of a framebuffer as the 3DS stores it: the LCDs are mounted
 * sideways, so each framebuffer "line" is one column of the visible screen
 * (240 pixels, bottom to top). We encode frames in this native orientation
 * and let the browser rotate them, which keeps capture memory tiny.
 */
typedef struct {
    int format; /* PIX_* */
    int width;  /* pixels per framebuffer line (240) */
    int lines;  /* framebuffer lines (400 top, 320 bottom) */
    int stride; /* bytes from one line to the next */
} frame_geom;

static inline int pix_bytes(int format)
{
    switch (format) {
    case PIX_RGBA8: return 4;
    case PIX_BGR8:  return 3;
    default:        return 2;
    }
}

/* ---- platform ---------------------------------------------------------- */

/* Read the current framebuffer geometry for a screen. 0 on success. */
int capture_begin(int screen, frame_geom *geom);

/* Start copying `count` framebuffer lines starting at `first` into dst, laid
 * out with geom->stride bytes per line, without waiting for the copy, so it
 * can run while the previous lines are encoded. One copy per screen at a
 * time. 0 on success. */
int capture_start(int screen, int first, int count, uint8_t *dst);

/* Wait for the copy capture_start() began. 0 once dst holds the lines. */
int capture_wait(int screen);

/* Called once per successful capture_begin(), after the last copy. Also
 * cancels a copy that is still running. */
void capture_end(int screen);

/* Monotonic milliseconds. */
uint64_t plat_now_ms(void);
void plat_sleep_ms(int ms);

/* True once the service has been asked to stop. Cheap; called often. */
bool plat_should_exit(void);

/* Persist / restore the settings string (key=value lines). */
int  plat_load_settings(char *buf, int cap);
void plat_save_settings(const char *buf, int len);

/* ---- logging ----------------------------------------------------------- */

/* Platform sink for each log line (3DS: SD card file, PC: stderr). */
void plat_log_write(const char *line, int len);

void core_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Copies the recent log (oldest first) into buf, returns length. */
int  core_log_read(char *buf, int cap);
