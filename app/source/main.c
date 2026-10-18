/*
 * 3DSLink app.
 *
 * Installs (or updates) the bundled background streaming service, starts it,
 * and tells you, in plain words, what to open on your computer. The service
 * keeps streaming after you leave this app and start a game.
 */
#include <3ds.h>
#include <arpa/inet.h>
#include <malloc.h>
#include <netinet/in.h>
#include <poll.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ui.h"

#define SERVICE_TID   0x000401300CF10902ULL
#define SERVICE_MEDIA MEDIATYPE_NAND /* where FBI puts system modules too */
#define SERVICE_CIA   "romfs:/3DSLinkService.cia"
#define PORT          8080
#define SOC_SIZE      0x80000

static int state = ST_CHECKING;
static char state_detail[64];
static int install_pct;
static u32 ip;
static char address[32]; /* "192.168.1.47:8080" */
static int viewers = -1;
static char top_fps[8], bottom_fps[8], model[16];

/* Messages from this app, newest last (shown on the Logs tab). */
#define APP_LOG_MAX 24
static char app_log[APP_LOG_MAX][64];
static int app_log_n;

/* Bottom screen state. */
static int tab = TAB_HOME;
static int pressed = BTN_NONE;
static int scroll;
#define LOG_LINES_MAX 120
static char log_text[LOG_LINES_MAX][UI_LOG_COLS + 2];
static const char *log_ptrs[LOG_LINES_MAX];
static int log_count;

/* ---- small helpers ---------------------------------------------------------- */

static void logf_(const char *fmt, ...)
{
    if (app_log_n == APP_LOG_MAX) {
        memmove(app_log[0], app_log[1], sizeof(app_log) - sizeof(app_log[0]));
        app_log_n--;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(app_log[app_log_n++], sizeof(app_log[0]), fmt, ap);
    va_end(ap);
}

static void frame(void)
{
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

/* ---- top screen ------------------------------------------------------------- */

static u16 canvas[UI_W * UI_H];

/* Easter egg: the Konami code shows Clawd for 3 seconds, then fades him out. */
#define CLAWD_SHOW_MS 3000
#define CLAWD_FADE_MS 1000
static const u32 konami[] = { KEY_UP, KEY_UP, KEY_DOWN, KEY_DOWN, KEY_LEFT,
                              KEY_RIGHT, KEY_LEFT, KEY_RIGHT, KEY_B, KEY_A };
static int konami_pos;
static u64 clawd_since; /* 0 when not showing */

/* Feed key presses in; returns true when the code was just completed. */
static bool konami_step(u32 down)
{
    u32 keys = down & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B);
    if (!keys)
        return false;
    if (keys & konami[konami_pos]) {
        if (++konami_pos == (int)(sizeof(konami) / sizeof(konami[0]))) {
            konami_pos = 0;
            return true;
        }
    } else if (keys & KEY_UP) {
        /* An extra Up still leaves the last two presses as "Up, Up". */
        konami_pos = konami_pos == 2 ? 2 : 1;
    } else {
        konami_pos = 0;
    }
    return false;
}

static void draw_top(void)
{
    ui_state ui = {
        .state = state,
        .install_pct = install_pct,
        .address = ip ? address : NULL,
        .viewers = viewers,
        .top_fps = top_fps,
        .bottom_fps = bottom_fps,
        .version = TDSL_VERSION,
    };
    ui_draw_top(&ui, canvas, consoleGetDefault()->font.gfx);
    if (clawd_since) {
        u64 t = osGetTime() - clawd_since;
        int alpha = t < CLAWD_SHOW_MS ? 256 : (int)(256 * (CLAWD_SHOW_MS + CLAWD_FADE_MS - t) / CLAWD_FADE_MS);
        int bob = (int)lrintf(4.0f * sinf((float)t / 160.0f));
        ui_overlay_clawd(canvas, consoleGetDefault()->font.gfx, alpha, bob);
    }

    /* The LCD is mounted sideways: each 240-pixel line is one screen column.
     * Copying a finished image in one go means nothing ever flickers. */
    u16 *fb = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    for (int x = 0; x < UI_W; x++)
        for (int y = 0; y < UI_H; y++)
            fb[x * UI_H + (UI_H - 1 - y)] = canvas[y * UI_W + x];
}

/* ---- bottom screen ---------------------------------------------------------- */

static u16 bcanvas[UI_BOTTOM_W * UI_H];

static void add_log_line(const char *line)
{
    if (log_count == LOG_LINES_MAX) {
        /* Keep the newest lines. */
        memmove(log_text[0], log_text[1], sizeof(log_text) - sizeof(log_text[0]));
        log_count--;
    }
    snprintf(log_text[log_count++], sizeof(log_text[0]), "%s", line);
}

/* Adds text, wrapped to the width of the Logs card. */
static void add_log_wrapped(const char *text)
{
    char chunk[UI_LOG_COLS + 1];
    size_t len = strlen(text);
    if (!len) {
        add_log_line("");
        return;
    }
    for (size_t i = 0; i < len; i += UI_LOG_COLS) {
        snprintf(chunk, sizeof(chunk), "%.*s", UI_LOG_COLS, text + i);
        add_log_line(chunk);
    }
}

/* Rebuilds the Logs tab: this app's messages, then the service's log file. */
static void build_logs(void)
{
    bool at_end = scroll + UI_LOG_VISIBLE >= log_count;
    log_count = 0;
    add_log_line("#This app");
    for (int i = 0; i < app_log_n; i++)
        add_log_wrapped(app_log[i]);
    add_log_line("");
    add_log_line("#Streaming service");
    FILE *f = fopen("sdmc:/3ds/3DSLink/log.txt", "r");
    if (!f) {
        add_log_line("(no log yet - it hasn't run)");
    } else {
        /* Only the end of a long log is interesting. */
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, size > 4096 ? size - 4096 : 0, SEEK_SET);
        char line[160];
        bool first = size > 4096;
        while (fgets(line, sizeof(line), f)) {
            if (first) { /* skip the partial first line */
                first = false;
                continue;
            }
            line[strcspn(line, "\r\n")] = 0;
            add_log_wrapped(line);
        }
        fclose(f);
    }
    for (int i = 0; i < log_count; i++)
        log_ptrs[i] = log_text[i];
    int max_scroll = log_count > UI_LOG_VISIBLE ? log_count - UI_LOG_VISIBLE : 0;
    if (at_end || scroll > max_scroll)
        scroll = max_scroll; /* follow new lines unless you scrolled up */
}

static void scroll_by(int lines)
{
    int max_scroll = log_count > UI_LOG_VISIBLE ? log_count - UI_LOG_VISIBLE : 0;
    scroll += lines;
    if (scroll > max_scroll)
        scroll = max_scroll;
    if (scroll < 0)
        scroll = 0;
}

static void draw_bottom(void)
{
    ui_bottom b = {
        .tab = tab,
        .streaming_on = state != ST_STOPPED,
        .pressed = pressed,
        .lines = log_ptrs,
        .line_count = log_count,
        .scroll = scroll,
    };
    ui_draw_bottom(&b, bcanvas, consoleGetDefault()->font.gfx);
    u16 *fb = (u16 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, NULL, NULL);
    for (int x = 0; x < UI_BOTTOM_W; x++)
        for (int y = 0; y < UI_H; y++)
            fb[x * UI_H + (UI_H - 1 - y)] = bcanvas[y * UI_BOTTOM_W + x];
}

static void draw(void)
{
    draw_top();
    draw_bottom();
}

/* ---- network ---------------------------------------------------------------- */

static void refresh_ip(void)
{
    /* gethostid() returns a signed long, which is negative for addresses
     * ending in 128-255, so compare it as unsigned. */
    u32 now = (u32)gethostid();
    if (now == htonl(INADDR_LOOPBACK) || now == 0xFFFFFFFF)
        now = 0;
    if (now != ip) {
        ip = now;
        if (ip) {
            struct in_addr a = { .s_addr = ip };
            snprintf(address, sizeof(address), "%s:%d", inet_ntoa(a), PORT);
        } else {
            address[0] = 0;
        }
    }
}

/* GET /api/status from the service. Returns body length or -1. */
static int fetch_status(char *body, int cap)
{
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s < 0)
        return -1;
    struct sockaddr_in sa = { 0 };
    sa.sin_family = AF_INET;
    sa.sin_port = htons(PORT);
    static int which;
    /* Alternate between the Wi-Fi address and loopback in case one of them
     * can't reach our own service. */
    sa.sin_addr.s_addr = ip && !(which++ & 1) ? ip : htonl(INADDR_LOOPBACK);

    static char buf[2048];
    int len = 0, result = -1;
    static const char req[] = "GET /api/status HTTP/1.0\r\nConnection: close\r\n\r\n";
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == 0 &&
        send(s, req, sizeof(req) - 1, 0) == (int)sizeof(req) - 1) {
        for (;;) {
            struct pollfd p = { .fd = s, .events = POLLIN };
            if (poll(&p, 1, 1000) <= 0)
                break;
            int n = recv(s, buf + len, sizeof(buf) - 1 - len, 0);
            if (n <= 0)
                break;
            len += n;
            if (len >= (int)sizeof(buf) - 1)
                break;
        }
        buf[len] = 0;
        char *b = strstr(buf, "\r\n\r\n");
        if (!strncmp(buf, "HTTP/1.1 200", 12) && b) {
            snprintf(body, (size_t)cap, "%s", b + 4);
            result = (int)strlen(body);
        }
    }
    close(s);
    return result;
}

/* Tiny helper for our own predictable JSON: text after "key": */
static const char *json_after(const char *json, const char *key)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    return p ? p + strlen(pat) : NULL;
}

static void copy_number(char *dst, int cap, const char *src)
{
    int n = 0;
    while (src && n < cap - 1 && ((*src >= '0' && *src <= '9') || *src == '.'))
        dst[n++] = *src++;
    dst[n] = 0;
}

static bool update_status(char *version_out, int vcap)
{
    char body[1024];
    if (fetch_status(body, sizeof(body)) < 0)
        return false;
    const char *p;
    if (version_out && (p = json_after(body, "version")) && *p == '"') {
        int n = 0;
        for (p++; *p && *p != '"' && n < vcap - 1; p++)
            version_out[n++] = *p;
        version_out[n] = 0;
    }
    viewers = (p = json_after(body, "streams")) ? atoi(p) : -1;
    const char *top = strstr(body, "\"top\":{"), *bottom = strstr(body, "\"bottom\":{");
    copy_number(top_fps, sizeof(top_fps), top ? json_after(top, "fps") : NULL);
    copy_number(bottom_fps, sizeof(bottom_fps), bottom ? json_after(bottom, "fps") : NULL);
    if ((p = json_after(body, "model")) && *p == '"') {
        int n = 0;
        for (p++; *p && *p != '"' && n < (int)sizeof(model) - 1; p++)
            model[n++] = *p;
        model[n] = 0;
    }
    return true;
}

/* ---- service management -------------------------------------------------- */

static int installed_version(void)
{
    u64 tid = SERVICE_TID;
    AM_TitleInfo info;
    if (R_FAILED(AM_GetTitleInfo(SERVICE_MEDIA, 1, &tid, &info)))
        return -1;
    return info.version;
}

static void stop_service(void)
{
    NS_TerminateProcessTID(SERVICE_TID, 3000000000ULL);
    svcSleepThread(300000000LL); /* let it release port 8080 */
}

static Result install_service(void)
{
    FILE *f = fopen(SERVICE_CIA, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    /* Like FBI: remove the old copy first so a different (even older)
     * version always installs cleanly. The service is already stopped. */
    AM_DeleteTitle(SERVICE_MEDIA, SERVICE_TID);
    AM_DeleteTicket(SERVICE_TID);

    Handle cia;
    Result rc = AM_StartCiaInstall(SERVICE_MEDIA, &cia);
    if (R_FAILED(rc)) {
        fclose(f);
        return rc;
    }

    static u8 chunk[0x10000];
    u64 off = 0;
    while (off < (u64)size) {
        size_t n = fread(chunk, 1, sizeof(chunk), f);
        if (n == 0) {
            rc = -1;
            break;
        }
        u32 written = 0;
        rc = FSFILE_Write(cia, &written, off, chunk, (u32)n, 0);
        if (R_FAILED(rc))
            break;
        off += n;
        install_pct = (int)(off * 100 / (u64)size);
        draw();
        frame();
    }
    fclose(f);

    if (R_FAILED(rc)) {
        AM_CancelCIAInstall(cia);
        return rc;
    }
    return AM_FinishCiaInstall(cia);
}

/* Make sure the bundled service is installed and running. */
static void ensure_service(bool force_reinstall)
{
    char running_version[16] = "";
    state = ST_CHECKING;
    state_detail[0] = 0;
    draw();
    frame();

    bool running = update_status(running_version, sizeof(running_version));
    int have = installed_version();
    bool outdated = have != TDSL_TITLE_VERSION ||
                    (running && strcmp(running_version, TDSL_VERSION) != 0);

    if (force_reinstall || outdated || have < 0) {
        state = ST_INSTALLING;
        install_pct = 0;
        logf_(have < 0 ? "Setting up the streaming service..." : "Updating the streaming service...");
        draw();
        frame();
        /* Stop whatever is running (possibly an older build that doesn't
         * answer the status API) so the files can be replaced. */
        stop_service();
        running = false;
        Result rc = install_service();
        if (R_FAILED(rc)) {
            state = ST_FAILED;
            snprintf(state_detail, sizeof(state_detail), "Install failed (%08lX)", rc);
            logf_("Install failed: %08lX", rc);
            return;
        }
        logf_("Streaming service v%s installed.", TDSL_VERSION);
    }

    if (!running) {
        state = ST_STARTING;
        draw();
        frame();
        u32 pid = 0;
        Result rc = NS_LaunchTitle(SERVICE_TID, 0, &pid);
        if (R_FAILED(rc)) {
            state = ST_FAILED;
            snprintf(state_detail, sizeof(state_detail), "Launch failed (%08lX)", rc);
            logf_("Launch failed: %08lX", rc);
            return;
        }
        /* Give it a few seconds to open its port. */
        for (int i = 0; i < 40 && !running; i++) {
            svcSleepThread(150000000LL);
            refresh_ip();
            running = update_status(NULL, 0);
            draw();
            frame();
        }
    }

    if (running) {
        state = ST_RUNNING;
        state_detail[0] = 0;
        logf_("Streaming service is running.");
    } else if (!ip) {
        /* Without Wi-Fi we can't talk to it, but it is running and will
         * start listening once the 3DS connects. */
        state = ST_RUNNING;
        snprintf(state_detail, sizeof(state_detail), "Waiting for Wi-Fi");
    } else {
        state = ST_FAILED;
        snprintf(state_detail, sizeof(state_detail), "Service did not answer");
        logf_("The streaming service didn't answer.");
        /* Show what the service itself logged. */
        tab = TAB_LOGS;
        build_logs();
    }
}

/* ---- main ------------------------------------------------------------------- */

int main(void)
{
    gfxInitDefault();
    /* Both screens: single-buffered RGB565, drawn by draw_top/draw_bottom. */
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    gfxSetScreenFormat(GFX_BOTTOM, GSP_RGB565_OES);
    gfxSetDoubleBuffering(GFX_TOP, false);
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
    gfxSwapBuffersGpu();
    gspWaitForVBlank();
    hidSetRepeatParameters(300, 60);

    hidScanInput();
    bool force = (hidKeysHeld() & (KEY_L | KEY_R)) == (KEY_L | KEY_R);

    romfsInit();
    amInit();
    nsInit();
    u32 *soc_buf = memalign(0x1000, SOC_SIZE);
    if (!soc_buf || R_FAILED(socInit(soc_buf, SOC_SIZE)))
        logf_("Network unavailable");

    refresh_ip();
    ensure_service(force);

    u64 last_poll = 0;
    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown(), held = hidKeysHeld(), repeat = hidKeysDownRepeat();
        bool changed = down != 0;

        if (konami_step(down)) {
            clawd_since = osGetTime();
            down &= ~KEY_A; /* the final A is part of the code, not "Restart" */
        }

        /* Touch: highlight what's under the stylus and act on it right away. */
        int tap = BTN_NONE;
        if (down & KEY_TOUCH) {
            touchPosition t;
            hidTouchRead(&t);
            ui_bottom hit_state = { .tab = tab };
            tap = pressed = ui_bottom_hit(&hit_state, t.px, t.py);
        } else if (!(held & KEY_TOUCH) && pressed != BTN_NONE) {
            pressed = BTN_NONE;
            changed = true;
        }

        if ((down & KEY_START) || tap == BTN_EXIT)
            break;
        if ((down & KEY_A) || tap == BTN_RESTART) {
            logf_("Restarting streaming...");
            stop_service();
            ensure_service(false);
        }
        if ((down & KEY_X) || tap == BTN_TOGGLE) {
            if (state == ST_STOPPED) {
                logf_("Turning streaming on...");
                ensure_service(false);
            } else {
                stop_service();
                state = ST_STOPPED;
                state_detail[0] = 0;
                viewers = -1;
                logf_("Streaming turned off.");
            }
        }
        if ((down & KEY_L) || tap == BTN_TAB_HOME)
            tab = TAB_HOME;
        if ((down & KEY_R) || tap == BTN_TAB_LOGS) {
            if (tab != TAB_LOGS)
                scroll = 1 << 30; /* open at the newest lines */
            tab = TAB_LOGS;
            build_logs();
        }
        if (tab == TAB_LOGS) {
            if ((repeat & KEY_DUP) || tap == BTN_SCROLL_UP)
                scroll_by(tap == BTN_SCROLL_UP ? -UI_LOG_VISIBLE / 2 : -1);
            if ((repeat & KEY_DDOWN) || tap == BTN_SCROLL_DOWN)
                scroll_by(tap == BTN_SCROLL_DOWN ? UI_LOG_VISIBLE / 2 : 1);
            changed |= (repeat & (KEY_DUP | KEY_DDOWN)) != 0;
        }

        /* Clawd animates every frame while he's on screen. */
        if (clawd_since) {
            if (osGetTime() - clawd_since >= CLAWD_SHOW_MS + CLAWD_FADE_MS)
                clawd_since = 0;
            draw_top();
        }

        /* Redraw once a second, and right away after any input. */
        u64 now = osGetTime();
        if (changed || now - last_poll >= 1000) {
            if (now - last_poll >= 1000) {
                last_poll = now;
                refresh_ip();
                if (state == ST_RUNNING) {
                    if (update_status(NULL, 0))
                        state_detail[0] = 0;
                    else if (ip)
                        snprintf(state_detail, sizeof(state_detail), "Not answering right now");
                }
                if (tab == TAB_LOGS)
                    build_logs();
            }
            draw();
        }
        frame();
    }

    socExit();
    free(soc_buf);
    nsExit();
    amExit();
    romfsExit();
    gfxExit();
    return 0;
}
