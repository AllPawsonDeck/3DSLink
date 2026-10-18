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
#include <string.h>

#include "capture.h"
#include "core/core.h"

#define VRAM_START 0x1F000000u
#define VRAM_END   0x1F600000u
#define DMA_TIMEOUT_NS (50 * 1000 * 1000LL)

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
} cur[SCREEN_COUNT];

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
    if (R_FAILED(import_capture_info(&info)))
        return -1;

    const GSPGPU_CaptureInfoEntry *e = &info.screencapture[screen];
    g->format = (int)(e->format & 7);
    g->width = 240;
    g->lines = screen == SCREEN_TOP ? 400 : 320;
    g->stride = (int)e->framebuf_widthbytesize;
    if (g->format > PIX_RGBA4 || !e->framebuf0_vaddr || g->stride < 240 * pix_bytes(g->format))
        return -1;

    u32 addr = (u32)e->framebuf0_vaddr;
    capture_end(screen);
    Handle proc = open_owner(addr, (u32)g->stride * (u32)g->lines);
    if (!proc)
        return -1;

    cur[screen].vaddr = addr;
    cur[screen].proc = proc;
    cur[screen].geom = *g;
    return 0;
}

static int wait_dma(Handle dma)
{
    /* DMA objects are kernel synchronization objects (class token 0x59), so
     * the handle is signalled when the transfer finishes. Note a timeout is
     * reported as a non-negative result, hence the explicit == 0. */
    if (svcWaitSynchronization(dma, DMA_TIMEOUT_NS) == 0)
        return 0;
    DmaState st;
    return R_SUCCEEDED(svcGetDmaState(&st, dma)) && st == DMASTATE_DONE ? 0 : -1;
}

int capture_lines(int screen, int first, int count, uint8_t *dst)
{
    const frame_geom *g = &cur[screen].geom;
    u32 size = (u32)count * (u32)g->stride;
    u32 src = cur[screen].vaddr + (u32)first * (u32)g->stride;

    /* No dirty cache lines may be written back over the DMA'd data. */
    svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)dst, size);

    DmaConfig cfg;
    dmaConfigInitDefault(&cfg);
    Handle dma = 0;
    Result rc = svcStartInterProcessDma(&dma, CUR_PROCESS_HANDLE, (u32)dst, cur[screen].proc,
                                        src, size, &cfg);
    if (R_FAILED(rc)) {
        owner_pid = 0; /* the owner may have exited; look it up again */
        return -1;
    }
    int ok = wait_dma(dma);
    svcStopDma(dma);
    svcCloseHandle(dma);
    svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)dst, size);
    return ok;
}

void capture_end(int screen)
{
    Handle h = cur[screen].proc;
    if (h && h != CUR_PROCESS_HANDLE)
        svcCloseHandle(h);
    cur[screen].proc = 0;
}
