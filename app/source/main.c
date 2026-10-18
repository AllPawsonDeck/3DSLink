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

static PrintConsole bottom_con;
static int state = ST_CHECKING;
static char state_detail[64];
static int install_pct;
static u32 ip;
static char address[32]; /* "192.168.1.47:8080" */
static int viewers = -1;
static char top_fps[8], bottom_fps[8], model[16];
static char log_lines[6][41];

/* ---- small helpers ---------------------------------------------------------- */

static void logf_(const char *fmt, ...)
{
    memmove(log_lines[0], log_lines[1], sizeof(log_lines) - sizeof(log_lines[0]));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(log_lines[5], sizeof(log_lines[5]), fmt, ap);
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

    /* The LCD is mounted sideways: each 240-pixel line is one screen column.
     * Copying a finished image in one go means nothing ever flickers. */
    u16 *fb = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    for (int x = 0; x < UI_W; x++)
        for (int y = 0; y < UI_H; y++)
            fb[x * UI_H + (UI_H - 1 - y)] = canvas[y * UI_W + x];
}

static void draw_bottom(void)
{
    consoleSelect(&bottom_con);
    printf("\x1b[2;2H\x1b[33;1mButtons\x1b[0m");
    printf("\x1b[4;3H\x1b[36mA\x1b[0m      Restart streaming");
    printf("\x1b[5;3H\x1b[36mX\x1b[0m      Turn streaming off");
    printf("\x1b[6;3H\x1b[36mSTART\x1b[0m  Close this app");
    printf("\x1b[7;10H(streaming keeps running)");

    printf("\x1b[10;2H\x1b[33;1mTips\x1b[0m");
    printf("\x1b[12;3H- Click \"Pop out\" in Chrome to share");
    printf("\x1b[13;5Hthe game in Discord.");
    printf("\x1b[14;3H- To stop streaming during a game,");
    printf("\x1b[15;5Hhold START + SELECT for 2 seconds.");
    printf("\x1b[16;3H- Hold L + R while opening this app");
    printf("\x1b[17;5Hto reinstall the streaming service.");

    for (int i = 0; i < 6; i++)
        printf("\x1b[%d;2H\x1b[37m%-38.38s\x1b[0m", 23 + i, log_lines[i]);
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
        /* Show what the service itself logged. */
        FILE *f = fopen("sdmc:/3ds/3DSLink/log.txt", "r");
        char line[64];
        int shown = 0;
        while (f && fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            logf_("%s", line);
            shown++;
        }
        if (f)
            fclose(f);
        if (!shown)
            logf_("Service wrote no log (it never ran).");
        logf_("Log: sdmc:/3ds/3DSLink/log.txt");
    }
}

/* ---- main ------------------------------------------------------------------- */

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, &bottom_con);
    /* Top screen: single-buffered RGB565, drawn by draw_top(). */
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    gfxSetDoubleBuffering(GFX_TOP, false);
    gfxSwapBuffersGpu();
    gspWaitForVBlank();

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
        u32 down = hidKeysDown();
        if (down & KEY_START)
            break;
        if (down & KEY_A) {
            logf_("Restarting streaming...");
            stop_service();
            ensure_service(false);
        }
        if ((down & KEY_X) && state != ST_STOPPED) {
            stop_service();
            state = ST_STOPPED;
            state_detail[0] = 0;
            viewers = -1;
            logf_("Streaming turned off.");
        }

        /* Redraw once a second (and after button presses); the console and
         * QR code are drawn straight into the framebuffer. */
        u64 now = osGetTime();
        if (down || now - last_poll >= 1000) {
            last_poll = now;
            refresh_ip();
            if (state == ST_RUNNING) {
                if (update_status(NULL, 0))
                    state_detail[0] = 0;
                else if (ip)
                    snprintf(state_detail, sizeof(state_detail), "Not answering right now");
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
