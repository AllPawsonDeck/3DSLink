#pragma once

#include "core.h"

typedef struct {
    int port;
    const char *version;
    const char *model;      /* shown in the viewer, e.g. "New 3DS" */

    /* Memory budget. Old 3DS only has ~400 KB for the whole service. */
    int max_slots;          /* encoded-frame buffers (min 3) */
    uint32_t slot_bytes;    /* largest JPEG a buffer can hold */
    uint32_t min_slot_bytes;/* give up below this */
    uint32_t strip_bytes;   /* copy buffer per worker (two halves, one per DMA) */
    uint32_t reserve_bytes; /* heap left free for libjpeg + sockets */
    int max_clients;
    int max_streams;
    /* Socket send buffer for streams (0 = system default). Small buffers keep
     * a slow viewer from falling behind on stale queued frames. */
    int stream_sndbuf;
    /* After each frame, pause for this share of the time it took (percent),
     * so a CPU core shared with the HOME Menu and games stays responsive. */
    int rest_pct;
    int helper_rest_pct; /* the same for the extra workers */
    /* Threads that capture and encode. The first is the one that calls
     * server_run(); the others run on other cores via plat_thread_start(). */
    int workers;
    /* Each screen is sent as this many pictures ("tiles"), so the workers
     * can share a screen and unchanged parts aren't sent again. */
    int tiles;

    /* Defaults used until the viewer changes them. */
    int quality;
    int fps_top;
    int fps_bottom;
    bool fast;              /* "Faster, less detail" on by default */
} server_config;

int  server_start(const server_config *cfg);
void server_run(void);   /* returns once plat_should_exit() is true */
void server_stop(void);

/* ---- extra platform hooks used by the server ------------------------------ */

/* Current IPv4 address in network byte order, 0 when offline. The listener is
 * re-created whenever this changes (e.g. after Wi-Fi reconnects). */
uint32_t plat_net_address(void);

/* New 3DS CPU boost (804 MHz + L2). Returns false if unsupported. */
bool plat_set_cpu_boost(bool on);
