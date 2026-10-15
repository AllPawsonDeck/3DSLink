#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <arpa/inet.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MODULE_TID 0x000401300CF10902ULL
#define PORT 8080

static C3D_RenderTarget *topTarget;
static C3D_RenderTarget *bottomTarget;
static C2D_TextBuf textBuf;

static u32 COL_ORANGE;
static u32 COL_DARK;
static u32 COL_PANEL;
static u32 COL_WHITE;
static u32 COL_MUTED;
static u32 COL_GREEN;
static u32 COL_RED;

static void drawText(float x, float y, float scale, u32 color, const char *s)
{
    C2D_Text t;
    C2D_TextFontParse(&t, NULL, textBuf, s);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, scale, scale, color);
}

static void drawCentered(float centerX, float y, float scale, u32 color, const char *s)
{
    C2D_Text t;
    float w = 0.0f;
    C2D_TextFontParse(&t, NULL, textBuf, s);
    C2D_TextOptimize(&t);
    C2D_TextGetDimensions(&t, scale, scale, &w, NULL);
    C2D_DrawText(&t, C2D_WithColor, centerX - w / 2.0f, y, 0.5f, scale, scale, color);
}

static void drawLogo(void)
{
    /* Simple white linked-screen mark inspired by the project icon. */
    C2D_DrawRectSolid(157, 70, 0.4f, 86, 4, COL_WHITE);
    C2D_DrawRectSolid(157, 70, 0.4f, 4, 52, COL_WHITE);
    C2D_DrawRectSolid(239, 70, 0.4f, 4, 52, COL_WHITE);
    C2D_DrawRectSolid(157, 118, 0.4f, 86, 4, COL_WHITE);
    C2D_DrawRectSolid(167, 80, 0.4f, 66, 32, COL_DARK);

    C2D_DrawRectSolid(168, 148, 0.4f, 64, 3, COL_WHITE);
    C2D_DrawRectSolid(168, 148, 0.4f, 3, 45, COL_WHITE);
    C2D_DrawRectSolid(229, 148, 0.4f, 3, 45, COL_WHITE);
    C2D_DrawRectSolid(168, 190, 0.4f, 64, 3, COL_WHITE);
    C2D_DrawRectSolid(176, 157, 0.4f, 48, 27, COL_DARK);

    C2D_DrawCircleSolid(200, 137, 0.3f, 3, COL_WHITE);
    C2D_DrawRectSolid(198.5f, 127, 0.3f, 3, 8, COL_WHITE);
    C2D_DrawRectSolid(191, 127, 0.3f, 18, 2, COL_WHITE);
}

static void render(bool online, bool launching, const char *url, Result launchResult)
{
    C2D_TextBufClear(textBuf);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

    C2D_TargetClear(topTarget, COL_ORANGE);
    C2D_SceneBegin(topTarget);
    drawCentered(200, 18, 1.0f, COL_WHITE, "3DSLink");
    drawCentered(200, 48, 0.45f, COL_WHITE, "Wireless screen link");
    drawLogo();

    C2D_TargetClear(bottomTarget, COL_DARK);
    C2D_SceneBegin(bottomTarget);

    C2D_DrawRectSolid(12, 12, 0.2f, 296, 54, COL_PANEL);
    C2D_DrawCircleSolid(33, 39, 0.3f, 7, launching ? COL_ORANGE : (online ? COL_GREEN : COL_RED));
    drawText(51, 23, 0.62f, COL_WHITE, launching ? "Starting streamer..." : (online ? "Streamer ready" : "Streamer offline"));
    drawText(51, 45, 0.38f, COL_MUTED, online ? "Background service is listening" : "The service did not answer yet");

    C2D_DrawRectSolid(12, 78, 0.2f, 296, 76, COL_PANEL);
    drawText(28, 90, 0.40f, COL_MUTED, "OPEN ON YOUR COMPUTER");
    if (url && url[0])
        drawCentered(160, 119, 0.52f, COL_WHITE, url);
    else
        drawCentered(160, 119, 0.48f, COL_MUTED, "Wi-Fi address unavailable");

    if (launchResult < 0)
    {
        char err[64];
        snprintf(err, sizeof(err), "Launch error: %08lX", (unsigned long)launchResult);
        drawCentered(160, 163, 0.39f, COL_RED, err);
    }
    else
    {
        C2D_DrawRectSolid(12, 168, 0.2f, 142, 45, COL_ORANGE);
        C2D_DrawRectSolid(166, 168, 0.2f, 142, 45, COL_PANEL);
        drawCentered(83, 181, 0.50f, COL_WHITE, "A   GO HOME");
        drawCentered(237, 181, 0.50f, COL_WHITE, "X   STOP");
    }

    drawCentered(160, 221, 0.34f, COL_MUTED, "HOME is disabled here to avoid the alpha crash");

    C3D_FrameEnd(0);
}

static bool probeServer(u32 ip)
{
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s < 0) return false;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = ip;

    bool ok = connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0;
    if (ok)
    {
        static const char req[] = "GET / HTTP/1.0\r\nConnection: close\r\n\r\n";
        send(s, req, sizeof(req) - 1, 0);
        char b[16];
        ok = recv(s, b, sizeof(b), 0) > 0;
    }

    close(s);
    return ok;
}

int main(void)
{
    aptSetHomeAllowed(false);

    nsInit();
    NS_TerminateProcessTID(MODULE_TID, 0);
    svcSleepThread(120000000LL);

    u32 pid = 0;
    Result launchResult = NS_LaunchTitle(MODULE_TID, 0, &pid);

    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    topTarget = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    bottomTarget = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    textBuf = C2D_TextBufNew(4096);

    COL_ORANGE = C2D_Color32(255, 154, 20, 255);
    COL_DARK   = C2D_Color32(18, 19, 23, 255);
    COL_PANEL  = C2D_Color32(35, 37, 43, 255);
    COL_WHITE  = C2D_Color32(255, 255, 255, 255);
    COL_MUTED  = C2D_Color32(160, 165, 176, 255);
    COL_GREEN  = C2D_Color32(66, 211, 146, 255);
    COL_RED    = C2D_Color32(246, 92, 92, 255);

    u32 *socBuf = (u32 *)memalign(0x1000, 0x10000);
    bool socReady = false;
    u32 ip = 0;
    char url[80] = {0};

    if (socBuf && socInit(socBuf, 0x10000) >= 0)
    {
        socReady = true;
        ip = gethostid();
        if (ip)
        {
            struct in_addr a;
            a.s_addr = ip;
            snprintf(url, sizeof(url), "http://%s:%d/", inet_ntoa(a), PORT);
        }
    }

    bool online = false;
    bool launching = launchResult >= 0;
    int attempts = 0;

    while (aptMainLoop())
    {
        hidScanInput();
        u32 down = hidKeysDown();

        if (launchResult >= 0 && socReady && !online && attempts < 80)
        {
            online = probeServer(ip);
            attempts++;
            if (!online) svcSleepThread(35000000LL);
            else launching = false;
        }
        else if (attempts >= 80)
        {
            launching = false;
        }

        if (down & KEY_X)
        {
            NS_TerminateProcessTID(MODULE_TID, 0);
            break;
        }

        if (down & (KEY_A | KEY_B | KEY_START))
            break;

        render(online, launching, url, launchResult);
    }

    if (socReady) socExit();
    if (socBuf) free(socBuf);

    C2D_TextBufDelete(textBuf);
    C2D_Fini();
    C3D_Fini();
    gfxExit();

    nsExit();
    aptSetHomeAllowed(true);
    return 0;
}
