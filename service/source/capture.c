/*
 * Framebuffer capture on the 3DS.
 *
 * GSP tells us where the currently displayed framebuffers live (a virtual
 * address in whichever process drew them). VRAM is mapped into this service,
 * so VRAM framebuffers are copied from our own address space; framebuffers in
 * FCRAM belong to the game/applet, so we find the process that maps that
 * address and DMA from it. Copies are done a strip of lines at a time so the
 * Old 3DS memory budget is never exceeded.
 */
#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "capture.h"
#include "core/core.h"

#define VRAM_START 0x1F000000u
#define VRAM_END   0x1F600000u
#define DMA_TIMEOUT_NS (20 * 1000 * 1000LL)

static Handle gsp;
static u32 own_pid;

static GSPGPU_CaptureInfo info;

/* PID of the process that owned the framebuffer last time. Only the PID is
 * cached: holding a handle to a game after it exits could keep its memory
 * from being freed, so handles are opened per frame and closed right after. */
static u32 owner_pid;

static struct {
    u32 vaddr; /* first line of the framebuffer being copied */
    Handle proc;
    frame_geom geom;
    Handle dma; /* copy in flight, if any */
    uint8_t *dst;
    u32 size;
} cur[SCREEN_COUNT];

/* Stop (if still running) and release the copy in flight. */
static void capture_cancel(int screen)
{
    if (cur[screen].dma) {
        svcStopDma(cur[screen].dma);
        svcCloseHandle(cur[screen].dma);
        cur[screen].dma = 0;
    }
}

/*
 * The service log says what capture found and why it failed, but each kind
 * of message only when it changes: a failure repeated every frame shows up
 * once. "where" notes come from capture_begin, "copy" notes from the DMA.
 */
enum { NOTE_WHERE, NOTE_COPY, NOTE_KINDS };
static char last_note[SCREEN_COUNT][NOTE_KINDS][80];

static void note(int screen, int kind, const char *fmt, ...)
{
    char msg[80];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (strcmp(msg, last_note[screen][kind])) {
        strcpy(last_note[screen][kind], msg);
        core_log("%s screen: %s", screen == SCREEN_TOP ? "top" : "bottom", msg);
    }
}

int capture_init(void)
{
    Result rc = srvGetServiceHandle(&gsp, "gsp::Gpu");
    if (R_FAILED(rc))
        return -1;
    svcGetProcessId(&own_pid, CUR_PROCESS_HANDLE);
    return 0;
}

void capture_exit(void)
{
    for (int s = 0; s < SCREEN_COUNT; s++)
        capture_end(s);
    if (gsp)
        svcCloseHandle(gsp);
    gsp = 0;
}

/* GSPGPU::ImportDisplayCaptureInfo on our own session (no GPU rights needed). */
static Result import_capture_info(GSPGPU_CaptureInfo *out)
{
    u32 *cmd = getThreadCommandBuffer();
    cmd[0] = IPC_MakeHeader(0x18, 0, 0);
    Result rc = svcSendSyncRequest(gsp);
    if (R_FAILED(rc))
        return rc;
    rc = (Result)cmd[1];
    if (R_SUCCEEDED(rc))
        memcpy(out, &cmd[2], sizeof(*out));
    return rc;
}

static bool readable_in(Handle proc, u32 addr, u32 size)
{
    MemInfo mi;
    PageInfo pi;
    u32 last = addr + size - 1;
    if (R_FAILED(svcQueryProcessMemory(&mi, &pi, proc, addr)) || mi.state == MEMSTATE_FREE ||
        !(mi.perm & MEMPERM_READ))
        return false;
    if (last < mi.base_addr + mi.size)
        return true;
    /* Range spans two mappings; check the end as well. */
    return R_SUCCEEDED(svcQueryProcessMemory(&mi, &pi, proc, last)) &&
           mi.state != MEMSTATE_FREE && (mi.perm & MEMPERM_READ);
}

/* Open a process that maps [addr, addr+size). The caller closes it. */
static Handle open_owner(u32 addr, u32 size)
{
    Handle h;
    if (addr >= VRAM_START && addr + size <= VRAM_END)
        return CUR_PROCESS_HANDLE;

    if (owner_pid && R_SUCCEEDED(svcOpenProcess(&h, owner_pid))) {
        if (readable_in(h, addr, size))
            return h;
        svcCloseHandle(h);
    }
    owner_pid = 0;

    u32 pids[64];
    s32 count = 0;
    if (R_FAILED(svcGetProcessList(&count, pids, 64)))
        return 0;
    /* Newest processes first: the game or applet on screen is usually the
     * most recently started one. */
    for (s32 i = count - 1; i >= 0; i--) {
        if (pids[i] == own_pid || R_FAILED(svcOpenProcess(&h, pids[i])))
            continue;
        if (readable_in(h, addr, size)) {
            owner_pid = pids[i];
            return h;
        }
        svcCloseHandle(h);
    }
    return 0;
}

int capture_begin(int screen, frame_geom *g)
{
    Result rc = import_capture_info(&info);
    if (R_FAILED(rc)) {
        note(screen, NOTE_WHERE, "GSP gave no screen info (%08lX)", (unsigned long)rc);
        return -1;
    }

    const GSPGPU_CaptureInfoEntry *e = &info.screencapture[screen];
    u32 addr = (u32)e->framebuf0_vaddr;
    g->format = (int)(e->format & 7);
    g->width = 240;
    g->lines = screen == SCREEN_TOP ? 400 : 320;
    g->stride = (int)e->framebuf_widthbytesize;
    if (g->format > PIX_RGBA4 || !addr || g->stride < 240 * pix_bytes(g->format)) {
        note(screen, NOTE_WHERE, "odd screen info (at %08lX, format %lX, stride %d)",
             (unsigned long)addr, (unsigned long)e->format, g->stride);
        return -1;
    }

    capture_end(screen);
    Handle proc = open_owner(addr, (u32)g->stride * (u32)g->lines);
    if (!proc) {
        note(screen, NOTE_WHERE, "no process has its picture (at %08lX)", (unsigned long)addr);
        return -1;
    }
    /* Not the address itself: double buffering flips it every frame. */
    if (proc == CUR_PROCESS_HANDLE)
        note(screen, NOTE_WHERE, "picture is in VRAM (format %d, stride %d)", g->format, g->stride);
    else
        note(screen, NOTE_WHERE, "picture is in process %lu (format %d, stride %d)",
             (unsigned long)owner_pid, g->format, g->stride);

    cur[screen].vaddr = addr;
    cur[screen].proc = proc;
    cur[screen].geom = *g;
    return 0;
}

/*
 * Copies run in the background while the previous lines are encoded. To know
 * when one is finished, the last word of the destination is set to a marker
 * first: the DMA writes the buffer in order, so once that word changed, the
 * whole copy has landed. That doesn't depend on how the DMA handle or its
 * state behave (ChirunoMod found the state reads 0, "starting", once a copy
 * is done). If the framebuffer happens to end in the marker value itself,
 * the copy is taken as done after DMA_TIMEOUT_NS, as before.
 */
#define COPY_MARKER 0x5A3C96E1u

int capture_start(int screen, int first, int count, uint8_t *dst)
{
    const frame_geom *g = &cur[screen].geom;
    u32 size = (u32)count * (u32)g->stride;
    u32 src = cur[screen].vaddr + (u32)first * (u32)g->stride;

    capture_cancel(screen);
    vu32 *last = (vu32 *)(dst + size - 4);
    *last = COPY_MARKER;
    /* Write the marker out, and make sure no dirty cache line can later be
     * written back over the copied data. */
    svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)dst, size);

    DmaConfig cfg;
    dmaConfigInitDefault(&cfg);
    Handle dma = 0;
    Result rc = svcStartInterProcessDma(&dma, CUR_PROCESS_HANDLE, (u32)dst, cur[screen].proc,
                                        src, size, &cfg);
    if (R_FAILED(rc)) {
        note(screen, NOTE_COPY, "copy refused (%08lX, %lu bytes from %08lX)", (unsigned long)rc,
             (unsigned long)size, (unsigned long)src);
        owner_pid = 0; /* the owner may have exited; look it up again */
        return -1;
    }
    cur[screen].dma = dma;
    cur[screen].dst = dst;
    cur[screen].size = size;
    return 0;
}

static bool copy_landed(int screen)
{
    u32 end = (u32)cur[screen].dst + cur[screen].size;
    svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (end - 4) & ~31u, 32);
    return *(vu32 *)(end - 4) != COPY_MARKER;
}

int capture_wait(int screen)
{
    Handle dma = cur[screen].dma;
    if (!dma)
        return -1;

    bool done = false;
    DmaState st = DMASTATE_RUNNING;
    u64 deadline = svcGetSystemTick() + (u64)(SYSCLOCK_ARM11 / 1000000000.0 * DMA_TIMEOUT_NS);
    while (!done) {
        done = copy_landed(screen) || svcWaitSynchronization(dma, 0) == 0 ||
               (R_SUCCEEDED(svcGetDmaState(&st, dma)) && st == DMASTATE_DONE);
        if (done || svcGetSystemTick() > deadline)
            break;
        svcSleepThread(100 * 1000); /* 0.1 ms */
    }
    if (!done && R_SUCCEEDED(svcGetDmaState(&st, dma)) && st == DMASTATE_STARTING)
        done = true; /* see above: the framebuffer ends in the marker value */

    capture_cancel(screen);
    if (!done) {
        note(screen, NOTE_COPY, "copy did not finish (state %d)", (int)st);
        return -1;
    }
    svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)cur[screen].dst, cur[screen].size);
    return 0;
}

void capture_end(int screen)
{
    capture_cancel(screen);
    Handle h = cur[screen].proc;
    if (h && h != CUR_PROCESS_HANDLE)
        svcCloseHandle(h);
    cur[screen].proc = 0;
}
