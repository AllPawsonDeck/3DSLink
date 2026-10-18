/*
 * 3DSLink app.
 *
 * Installs (or updates) the bundled background streaming service, starts it,
 * and shows the address to open in a browser, plus a QR code for it. The
 * service keeps streaming after you leave this app and start a game.
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

#include "vendor/qrcodegen.h"

#define SERVICE_TID   0x000401300CF10902ULL
#define SERVICE_MEDIA MEDIATYPE_NAND /* where FBI puts system modules too */
#define SERVICE_CIA   "romfs:/3DSLinkService.cia"
#define PORT          8080
#define SOC_SIZE      0x80000

enum { ST_CHECKING, ST_INSTALLING, ST_STARTING, ST_RUNNING, ST_STOPPED, ST_FAILED };

static PrintConsole top_con, bottom_con;
static int state = ST_CHECKING;
static char state_detail[64];
static int install_pct;
static u32 ip;
static char url[48];
static char qr_for[48] = "-";
static int viewers = -1;
static char top_fps[8], bottom_fps[8], model[16];
static char log_lines[5][41];

/* ---- small helpers ---------------------------------------------------------- */

static void logf_(const char *fmt, ...)
{
    memmove(log_lines[0], log_lines[1], sizeof(log_lines) - sizeof(log_lines[0]));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(log_lines[4], sizeof(log_lines[4]), fmt, ap);
    va_end(ap);
}

static void frame(void)
{
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

/* ---- drawing ---------------------------------------------------------------- */

/* The console leaves the top framebuffer as single-buffered RGB565, stored
 * sideways (each 240-pixel line is one screen column, bottom to top). */
static void fill_rect(int x0, int y0, int w, int h, u16 color)
{
    u16 *fb = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    for (int x = x0; x < x0 + w; x++)
        for (int y = y0; y < y0 + h; y++)
            fb[x * 240 + (239 - y)] = color;
}

static void draw_qr(void)
{
    const int area_x = 226, area_y = 32, area = 170;
    /* Only redraw when the address changes; repainting it flickers. */
    if (!strcmp(qr_for, url))
        return;
    snprintf(qr_for, sizeof(qr_for), "%s", url);
    fill_rect(area_x, area_y, area, area, 0x0000);
    if (!url[0])
        return;

    static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
    static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
    if (!qrcodegen_encodeText(url, tmp, qr, qrcodegen_Ecc_MEDIUM, 1, 6, qrcodegen_Mask_AUTO, true))
        return;
    int size = qrcodegen_getSize(qr);
    int px = area / (size + 6); /* 3-module white border each side */
    int total = px * (size + 6);
    int ox = area_x + (area - total) / 2, oy = area_y + (area - total) / 2;
    fill_rect(ox, oy, total, total, 0xFFFF);
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++)
            if (qrcodegen_getModule(qr, x, y))
                fill_rect(ox + (x + 3) * px, oy + (y + 3) * px, px, px, 0x0000);
}

static const char *state_text(void)
{
    switch (state) {
    case ST_CHECKING:   return "\x1b[33mChecking...\x1b[0m";
    case ST_INSTALLING: return "\x1b[33mInstalling...\x1b[0m";
    case ST_STARTING:   return "\x1b[33mStarting...\x1b[0m";
    case ST_RUNNING:    return "\x1b[32mRunning\x1b[0m";
    case ST_STOPPED:    return "\x1b[31mStopped\x1b[0m";
    default:            return "\x1b[31mProblem\x1b[0m";
    }
}

static void draw(void)
{
    /* Top console window is 27 columns wide; ESC[K clears the rest of a line. */
    consoleSelect(&top_con);
    printf("\x1b[1;1H\x1b[33;1m3DSLink\x1b[0m  v%s\x1b[K", TDSL_VERSION);
    printf("\x1b[2;1HStream to your browser\x1b[K");
    printf("\x1b[4;1HService: %s\x1b[K", state_text());
    if (state == ST_INSTALLING)
        printf("\x1b[5;1H%d%%\x1b[K", install_pct);
    else
        printf("\x1b[5;1H%.27s\x1b[K", state_detail);

    if (!ip) {
        printf("\x1b[7;1H\x1b[31mNot connected to Wi-Fi.\x1b[0m\x1b[K");
        printf("\x1b[8;1HConnect in System Settings\x1b[K");
        printf("\x1b[9;1Hand this updates by itself.\x1b[K");
        printf("\x1b[11;1H\x1b[K\x1b[12;1H\x1b[K");
    } else {
        printf("\x1b[7;1HOn your Chromebook, open:\x1b[K");
        printf("\x1b[8;1H\x1b[36;1m%.27s\x1b[0m\x1b[K", url + 7); /* drop "http://" */
        printf("\x1b[9;1H\x1b[K");
        printf("\x1b[11;1HOr scan the code with the\x1b[K");
        printf("\x1b[12;1HCamera app's QR mode.\x1b[K");
    }

    if (state == ST_RUNNING && viewers >= 0) {
        printf("\x1b[15;1HViewers: %d\x1b[K", viewers);
        printf("\x1b[16;1HTop %s  Bottom %s fps\x1b[K", top_fps, bottom_fps);
        printf("\x1b[17;1H%s\x1b[K", model);
    } else {
        printf("\x1b[15;1H\x1b[K\x1b[16;1H\x1b[K\x1b[17;1H\x1b[K");
    }
    draw_qr();

    consoleSelect(&bottom_con);
    printf("\x1b[1;1H\x1b[33;1mControls\x1b[0m");
    printf("\x1b[2;2H\x1b[36mA\x1b[0m      Restart the streaming service");
    printf("\x1b[3;2H\x1b[36mX\x1b[0m      Stop the streaming service");
    printf("\x1b[4;2H\x1b[36mSTART\x1b[0m  Exit (streaming keeps running)");

    printf("\x1b[6;1H\x1b[33;1mHow to use\x1b[0m");
    printf("\x1b[7;2H1. Open the address on the top screen");
    printf("\x1b[8;2H   in Chrome (same Wi-Fi network).");
    printf("\x1b[9;2H2. Press HOME and start any game.");
    printf("\x1b[10;2H   The stream keeps going.");
    printf("\x1b[11;2H3. For Discord, click \"Pop out\" in");
    printf("\x1b[12;2H   the browser and share that window.");
    printf("\x1b[14;2HIn a game, hold \x1b[36mSTART + SELECT\x1b[0m for");
    printf("\x1b[15;2H2 seconds to stop streaming.");
    printf("\x1b[17;2HHold \x1b[36mL + R\x1b[0m while opening this app");
    printf("\x1b[18;2Hto reinstall the service.");

    for (int i = 0; i < 5; i++)
        printf("\x1b[%d;1H\x1b[37m%-40.40s\x1b[0m", 25 + i, log_lines[i]);
}

/* ---- network ---------------------------------------------------------------- */

static void refresh_ip(void)
{
    long id = gethostid();
    u32 now = id > 0 && id != 0x7F000001 ? (u32)id : 0;
    if (now != ip) {
        ip = now;
        if (ip) {
            struct in_addr a = { .s_addr = ip };
            snprintf(url, sizeof(url), "http://%s:%d", inet_ntoa(a), PORT);
        } else {
            url[0] = 0;
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
        logf_(have < 0 ? "Installing streaming service" : "Updating streaming service");
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
        logf_("Service v%s installed", TDSL_VERSION);
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
        logf_("Streaming service is running");
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
    consoleInit(GFX_TOP, &top_con);
    consoleInit(GFX_BOTTOM, &bottom_con);
    consoleSetWindow(&top_con, 1, 1, 27, 28);
    consoleSetWindow(&bottom_con, 0, 0, 40, 30);

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
            logf_("Restarting service...");
            stop_service();
            ensure_service(false);
        }
        if ((down & KEY_X) && state != ST_STOPPED) {
            stop_service();
            state = ST_STOPPED;
            state_detail[0] = 0;
            viewers = -1;
            logf_("Service stopped. Press A to start.");
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
