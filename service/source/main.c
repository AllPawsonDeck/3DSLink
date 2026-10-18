/*
 * 3DSLink background service.
 *
 * Installed as a system module and started by the 3DSLink app. It keeps
 * running while you play, serving the viewer page and live MJPEG streams of
 * both screens on port 8080.
 *
 * Stops when the 3DSLink app stops it, when the console shuts down, or when
 * START + SELECT are held together for two seconds.
 */
#include <3ds.h>
#include <3ds/allocator/mappable.h>
#include <arpa/inet.h>
#include <malloc.h>
#include <netinet/in.h>
#include <string.h>

#include "capture.h"
#include "core/core.h"
#include "core/server.h"

/* Old 3DS: the service shares a small region with every other system module.
 * 0x64000 is the most ChirunoMod found it could take without stopping games
 * and applets from launching. */
#define O3DS_HEAP 0x64000
#define N3DS_HEAP 0x200000
#define O3DS_SOC  0x10000
#define N3DS_SOC  0x40000

/* The server thread (network + first worker) runs at the usual priority of
 * a main thread and takes a break after every picture (rest_pct). 0x20 was
 * above system modules that launch apps, so opening one could stall; 0x3F
 * (below everything) let games starve the stream. Extra workers only use
 * idle time: the lowest priority there is. */
#define SERVER_PRIORITY 0x30
#define HELPER_PRIORITY 0x3F
#define NOTIF_TERMINATE 0x100
#define HID_PAD_REG     0x1EC46000 /* mapped via the exheader IO mappings */
#define COMBO_HOLD_MS   2000

#define SETTINGS_DIR  "/3ds/3DSLink"
#define SETTINGS_PATH "/3ds/3DSLink/settings.txt"
#define LOG_PATH      "/3ds/3DSLink/log.txt" /* the app shows it when we fail */

/* Main thread stack; libctru carves it out of the heap. */
u32 __stacksize__ = 0x4000;

static bool new3ds;
static Handle notif_sem;
static bool exit_requested;
static u64 combo_since;
static FS_Archive sdmc;
static bool sd_ok;
static Handle log_file;
static u64 log_off;

static bool detect_new3ds(void)
{
    return osGetApplicationMemType() >= 6;
}

/* Replaces libctru's default, which would claim all free memory. */
void __system_allocateHeaps(void)
{
    extern char *fake_heap_start;
    extern char *fake_heap_end;
    extern u32 __ctru_heap, __ctru_heap_size;
    extern u32 __ctru_linear_heap, __ctru_linear_heap_size;

    __ctru_heap_size = detect_new3ds() ? N3DS_HEAP : O3DS_HEAP;
    __ctru_linear_heap_size = 0;
    __ctru_linear_heap = 0;

    Result rc = svcControlMemory(&__ctru_heap, OS_HEAP_AREA_BEGIN, 0, __ctru_heap_size,
                                 MEMOP_ALLOC, MEMPERM_READ | MEMPERM_WRITE);
    if (R_FAILED(rc))
        svcBreak(USERBREAK_PANIC);

    mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);
    fake_heap_start = (char *)__ctru_heap;
    fake_heap_end = fake_heap_start + __ctru_heap_size;
}

/* System modules must not touch APT/HID/FS the way applications do. */
void __appInit(void)
{
    srvInit();
}

void __appExit(void)
{
    srvExit();
}

/* ---- platform hooks for the core --------------------------------------- */

uint64_t plat_now_ms(void)
{
    return svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000);
}

void plat_sleep_ms(int ms)
{
    svcSleepThread((s64)ms * 1000000LL);
}

bool plat_should_exit(void)
{
    if (exit_requested)
        return true;

    /* PM sends 0x100 when the app stops us or the console powers off. */
    while (svcWaitSynchronization(notif_sem, 0) == 0) {
        u32 id = 0;
        if (R_SUCCEEDED(srvReceiveNotification(&id)) && id == NOTIF_TERMINATE) {
            core_log("stop requested");
            exit_requested = true;
        }
    }

    /* Emergency stop: hold START + SELECT. Read straight from the HID
     * registers so we don't need an hid:USER session. */
    u32 held = ~(u32)*(vu16 *)HID_PAD_REG & 0xFFF;
    u64 now = plat_now_ms();
    /* Exactly START + SELECT: a register that reads back as all-pressed must
     * never look like the combo. */
    if (held == (KEY_START | KEY_SELECT)) {
        if (!combo_since)
            combo_since = now;
        else if (now - combo_since >= COMBO_HOLD_MS) {
            core_log("START + SELECT held, stopping");
            exit_requested = true;
        }
    } else {
        combo_since = 0;
    }
    return exit_requested;
}

int plat_load_settings(char *buf, int cap)
{
    Handle f;
    u32 n = 0;
    if (!sd_ok ||
        R_FAILED(FSUSER_OpenFile(&f, sdmc, fsMakePath(PATH_ASCII, SETTINGS_PATH), FS_OPEN_READ, 0)))
        return 0;
    FSFILE_Read(f, &n, 0, buf, (u32)cap);
    FSFILE_Close(f);
    return (int)n;
}

void plat_save_settings(const char *buf, int len)
{
    Handle f;
    u32 n = 0;
    if (!sd_ok)
        return;
    FSUSER_CreateDirectory(sdmc, fsMakePath(PATH_ASCII, "/3ds"), 0);
    FSUSER_CreateDirectory(sdmc, fsMakePath(PATH_ASCII, SETTINGS_DIR), 0);
    if (R_FAILED(FSUSER_OpenFile(&f, sdmc, fsMakePath(PATH_ASCII, SETTINGS_PATH),
                                 FS_OPEN_WRITE | FS_OPEN_CREATE, 0)))
        return;
    FSFILE_SetSize(f, 0);
    FSFILE_Write(f, &n, 0, buf, (u32)len, FS_WRITE_FLUSH);
    FSFILE_Close(f);
}

void plat_log_write(const char *line, int len)
{
    u32 n = 0;
    if (!sd_ok)
        return;
    if (!log_file) {
        FSUSER_CreateDirectory(sdmc, fsMakePath(PATH_ASCII, "/3ds"), 0);
        FSUSER_CreateDirectory(sdmc, fsMakePath(PATH_ASCII, SETTINGS_DIR), 0);
        if (R_FAILED(FSUSER_OpenFile(&log_file, sdmc, fsMakePath(PATH_ASCII, LOG_PATH),
                                     FS_OPEN_WRITE | FS_OPEN_CREATE, 0))) {
            log_file = 0;
            return;
        }
        FSFILE_SetSize(log_file, 0);
        log_off = 0;
    }
    FSFILE_Write(log_file, &n, log_off, line, (u32)len, FS_WRITE_FLUSH);
    log_off += n;
}

uint32_t plat_net_address(void)
{
    static u32 addr;
    static u64 checked;
    u64 now = plat_now_ms();
    if (!checked || now - checked >= 1000) {
        checked = now;
        /* gethostid() is a signed long: negative for x.x.x.128 and up. */
        addr = (u32)gethostid();
        if (addr == htonl(INADDR_LOOPBACK) || addr == 0xFFFFFFFF)
            addr = 0;
    }
    return addr;
}

bool plat_set_cpu_boost(bool on)
{
    if (!new3ds)
        return false;
    /* Only ever switch the boost on. Switching it off could slow down a New
     * 3DS game that asked for the fast clock itself; the next game launch
     * restores its own setting anyway. */
    if (on && R_SUCCEEDED(ptmSysmInit())) {
        PTMSYSM_ConfigureNew3DSCPU(3);
        ptmSysmExit();
    }
    return true;
}

/* ---- entry -------------------------------------------------------------- */

static RecursiveLock server_lock;

void plat_lock(void)
{
    RecursiveLock_Lock(&server_lock);
}

void plat_unlock(void)
{
    RecursiveLock_Unlock(&server_lock);
}

void *plat_thread_start(void (*fn)(void *), void *arg)
{
    /* Only used on the Old 3DS: the server runs on core 1, this goes on the
     * game's core 0 at the lowest priority, so the game always comes first. */
    return threadCreate(fn, arg, 0x4000, HELPER_PRIORITY, new3ds ? 3 : 0, false);
}

void plat_thread_join(void *thread)
{
    /* A worker starved by a busy game may take a moment to notice; don't
     * hang the shutdown on it. */
    if (R_SUCCEEDED(threadJoin(thread, 2000000000ULL)))
        threadFree(thread);
}

static void run_server(void *arg)
{
    (void)arg;
    server_run();
}

int main(void)
{
    new3ds = detect_new3ds();
    RecursiveLock_Init(&server_lock);
    srvEnableNotification(&notif_sem);

    if (R_SUCCEEDED(fsInit()) &&
        R_SUCCEEDED(FSUSER_OpenArchive(&sdmc, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""))))
        sd_ok = true;

    core_log("service %s starting (%s, heap %lu KB)", TDSL_VERSION, new3ds ? "New 3DS" : "Old 3DS",
             (unsigned long)(new3ds ? N3DS_HEAP : O3DS_HEAP) / 1024);
    if (capture_init() != 0)
        core_log("could not open gsp::Gpu");

    u32 soc_size = new3ds ? N3DS_SOC : O3DS_SOC;
    u32 *soc_buf = memalign(0x1000, soc_size);
    Result soc_rc = soc_buf ? socInit(soc_buf, soc_size) : -1;
    if (R_FAILED(soc_rc)) {
        core_log("socInit failed: %08lX", (unsigned long)soc_rc);
        goto out;
    }

    server_config cfg = {
        .port = 8080,
        .version = TDSL_VERSION,
        .model = new3ds ? "New 3DS" : "Old 3DS",
        /* Pictures are tiles (half a screen each): four are kept as the
         * newest, each worker encodes into one, and viewers send the rest. */
        .max_slots = 8,
        .slot_bytes = new3ds ? 96 * 1024 : 24 * 1024,
        .min_slot_bytes = new3ds ? 48 * 1024 : 12 * 1024,
        /* New 3DS copies a whole tile per DMA; Old 3DS ~10 lines at a time. */
        .strip_bytes = new3ds ? 400 * 960 : 20 * 1024,
        /* libjpeg needs ~25 KB per worker while encoding, plus the second
         * worker's stack. */
        .reserve_bytes = new3ds ? 96 * 1024 : 80 * 1024,
        .max_clients = new3ds ? 8 : 6,
        .max_streams = new3ds ? 3 : 2,
        /* Old 3DS: socket buffers come out of the 64 KB SOC area, keep the default. */
        .stream_sndbuf = new3ds ? 32 * 1024 : 0,
        /* Old 3DS: the server shares its core with the HOME Menu (which
         * stopped loading banners while we encoded flat out) and the game's
         * system threads. ChirunoMod rests too. The New 3DS's third core is
         * ours alone. */
        .rest_pct = new3ds ? 0 : 30,
        .helper_rest_pct = 5,
        /* Old 3DS: a second worker on the game's core (0) uses whatever time
         * the game or HOME Menu leaves idle. The New 3DS is fast enough with
         * its third core alone. */
        .workers = new3ds ? 1 : 2,
        .tiles = 2,
        .quality = new3ds ? 75 : 60,
        .fps_top = 30,
        .fps_bottom = new3ds ? 30 : 15,
        .fast = !new3ds, /* Old 3DS: smooth first; full detail is a setting */
    };

    if (server_start(&cfg) != 0)
        core_log("server_start failed");
    else {
        Thread t = NULL;
        if (new3ds) {
            /* The New 3DS's third core is usually idle: encode there so
             * games never feel it. */
            t = threadCreate(run_server, NULL, 0x4000, SERVER_PRIORITY, 2, false);
        }
        if (!t)
            svcSetThreadPriority(CUR_THREAD_HANDLE, SERVER_PRIORITY);
        core_log("server running on core %d", t ? 2 : 1);
        if (t) {
            threadJoin(t, U64_MAX);
            threadFree(t);
        } else {
            server_run();
        }
    }
    server_stop();
    SOCU_ShutdownSockets();
    socExit();

out:
    free(soc_buf);
    capture_exit();
    core_log("service exited");
    if (log_file)
        FSFILE_Close(log_file);
    if (sd_ok)
        FSUSER_CloseArchive(sdmc);
    fsExit();
    return 0;
}
