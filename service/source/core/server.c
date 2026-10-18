/*
 * 3DSLink HTTP server + capture scheduler.
 *
 * One non-blocking loop accepts connections, serves the viewer page and JSON
 * API, and pushes MJPEG pictures to stream clients. Each screen is cut into
 * a few tiles (bands of framebuffer lines) that are captured strip by strip
 * and encoded as separate JPEGs, so several worker threads (on different CPU
 * cores) can share the work and a tile that didn't change isn't sent again.
 * The first worker is the network loop's own thread: it pumps the sockets
 * between strips, so slow Wi-Fi never stalls encoding (and vice versa). A
 * client that falls behind simply skips pictures.
 *
 * Everything the workers share is guarded by plat_lock(); encoding itself
 * runs unlocked, into a buffer only that worker uses.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core.h"
#include "jpeg.h"
#include "server.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

extern const unsigned char web_index_gz[];
extern const unsigned int web_index_gz_len;

#define BOUNDARY "3dslink"
#define REQ_TIMEOUT_MS     5000
#define STALL_TIMEOUT_MS  10000
#define DRAIN_TIMEOUT_MS   3000  /* how long a finished response waits for the browser to hang up */
#define KEEPALIVE_MS       2000  /* empty part sent when screens stay still */
#define BOOST_REFRESH_MS  10000  /* a game launch resets the New 3DS clock */
#define STATIC_POLL_MS      100  /* how often an unchanged screen is re-checked */
#define STATIC_AFTER         15  /* unchanged captures before slowing down */
#define PUMP_EVERY_MS         5  /* network service interval while encoding */
#define STUCK_MS            150  /* a tile held this long may be taken over */
#define MAX_TILES             4
#define MAX_WORKERS           2

static const char *const screen_names[SCREEN_COUNT] = { "top", "bottom" };

/* ---- logging ------------------------------------------------------------ */

#define LOG_SIZE 2048
static char log_ring[LOG_SIZE];
static int log_head, log_full;

void core_log(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(line) - 2)
        n = (int)sizeof(line) - 2;
    line[n++] = '\n';
    plat_lock();
    for (int i = 0; i < n; i++) {
        log_ring[log_head++] = line[i];
        if (log_head == LOG_SIZE) {
            log_head = 0;
            log_full = 1;
        }
    }
    plat_log_write(line, n);
    plat_unlock();
}

int core_log_read(char *buf, int cap)
{
    int len = 0;
    if (log_full)
        for (int i = log_head; i < LOG_SIZE && len < cap; i++)
            buf[len++] = log_ring[i];
    for (int i = 0; i < log_head && len < cap; i++)
        buf[len++] = log_ring[i];
    return len;
}

/* ---- settings ----------------------------------------------------------- */

typedef struct {
    int quality;
    int fps[SCREEN_COUNT];
    bool sharp;
    bool boost;
    bool fast;  /* encode every pair of lines as one: about twice as fast */
} settings_t;

static settings_t settings;
static bool boost_supported;

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void settings_set(const char *key, const char *val)
{
    int v = atoi(val);
    if (!strcmp(key, "quality"))
        settings.quality = clampi(v, 10, 95);
    else if (!strcmp(key, "fps_top"))
        settings.fps[SCREEN_TOP] = clampi(v, 1, 60);
    else if (!strcmp(key, "fps_bottom"))
        settings.fps[SCREEN_BOTTOM] = clampi(v, 1, 60);
    else if (!strcmp(key, "sharp"))
        settings.sharp = v != 0;
    else if (!strcmp(key, "boost"))
        settings.boost = v != 0;
    else if (!strcmp(key, "fast"))
        settings.fast = v != 0;
}

static void settings_load(void)
{
    char buf[256];
    int n = plat_load_settings(buf, (int)sizeof(buf) - 1);
    if (n <= 0)
        return;
    buf[n] = 0;
    for (char *line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = 0;
            settings_set(line, eq + 1);
        }
    }
}

static void settings_save(void)
{
    char buf[160];
    int n = snprintf(buf, sizeof(buf),
                     "quality=%d\nfps_top=%d\nfps_bottom=%d\nsharp=%d\nboost=%d\nfast=%d\n",
                     settings.quality, settings.fps[SCREEN_TOP],
                     settings.fps[SCREEN_BOTTOM], settings.sharp, settings.boost, settings.fast);
    plat_save_settings(buf, n);
}

/* ---- picture pool ------------------------------------------------------- */

typedef struct {
    uint8_t *data;
    uint32_t len;
    uint32_t seq;     /* unique per picture */
    int screen;       /* -1 when empty */
    int tile;
    uint32_t frame;   /* the screen's capture pass it came from */
    int first, count; /* framebuffer lines it covers */
    int refs;         /* clients currently sending it */
    bool latest;      /* newest picture of its tile */
    bool encoding;    /* a worker is filling it */
} slot_t;

static slot_t *slots;
static int nslots;
static uint32_t slot_bytes;
static uint32_t publish_seq;

/* A buffer a worker can encode into without disturbing anyone. */
static int pool_find_scratch(unsigned wanted)
{
    for (int i = 0; i < nslots; i++)
        if (!slots[i].encoding && slots[i].refs == 0 && !slots[i].latest)
            return i;
    /* Fall back to a cached picture of a screen nobody is watching. */
    for (int i = 0; i < nslots; i++)
        if (!slots[i].encoding && slots[i].refs == 0 && slots[i].screen >= 0 &&
            !(wanted & (1u << slots[i].screen)))
            return i;
    return -1;
}

static void pool_take(int i);
static void pool_release(int i)
{
    slots[i].encoding = false;
}

/* ---- screens and tiles --------------------------------------------------- */

typedef struct {
    int latest;           /* slot with its newest picture, -1 if none */
    uint32_t busy_frame;  /* pass a worker is capturing it for (0: free) */
    uint64_t busy_since;
    uint32_t shown_frame; /* pass of the picture in `latest` */
    uint32_t hash;        /* of the lines in `latest` */
    bool have_hash;
    int static_run;       /* captures in a row that found it unchanged */
    int format;           /* of `latest` */
    bool fast;
} tile_t;

typedef struct {
    tile_t tile[MAX_TILES];
    int next_tile;        /* next tile of the current pass to hand out */
    uint32_t frame;       /* capture passes started */
    uint32_t counted;     /* last pass counted as a new frame */
    uint32_t frames;      /* passes that changed something */
    uint64_t last_start;
    uint64_t pass;        /* stride-scheduling virtual time */
    int quality_cut;      /* lowered after a picture overflowed its buffer */
    int good_run;
    int format;

    /* stats, refreshed once per second */
    uint64_t window_start;
    uint32_t frames_window, bytes_window;
    uint32_t fps10, kbps;
    uint32_t encode_ms;   /* smoothed, per tile: capture + encode */
    uint32_t copy_ms;     /* smoothed: of that, waiting for framebuffer copies */
    uint32_t last_size;   /* bytes of the last picture */
    char error[64];
    bool ok_logged;       /* "sending pictures" logged since the last error */
} screen_t;

static screen_t screens[SCREEN_COUNT];
static int ntiles = 1;

/* Lines [first, first + count) of tile t. Boundaries fall on multiples of 16
 * (whole JPEG blocks), so the seams don't show. */
static void tile_range(int lines, int t, int *first, int *count)
{
    int a = (lines * t / ntiles) & ~15;
    int b = t + 1 == ntiles ? lines : (lines * (t + 1) / ntiles) & ~15;
    *first = a;
    *count = b - a;
}

static void pool_take(int i)
{
    slot_t *f = &slots[i];
    if (f->latest)
        screens[f->screen].tile[f->tile].latest = -1;
    f->latest = false;
    f->screen = -1;
    f->len = 0;
    f->encoding = true;
}

/* ---- clients ------------------------------------------------------------ */

enum { C_FREE, C_REQ, C_RESP, C_DRAIN, C_STREAM };

typedef struct {
    int fd;
    int state;
    uint64_t since;  /* last progress */
    int req_len;
    char buf[1024];  /* request, then reused for small response bodies */
    char hdr[256];
    int hdr_len, hdr_off;
    const uint8_t *body;
    uint32_t body_len, body_off;
    int body_slot;   /* frame slot this client holds a reference on */

    unsigned want;   /* SCREEN mask (streams) */
    uint32_t sent_seq[SCREEN_COUNT][MAX_TILES];
    int rr;          /* tile to look at first next time (round robin) */
    bool first_part;
} client_t;

static client_t *clients;
static int max_clients, max_streams;

static server_config cfg;
static int listen_fd = -1;
static uint32_t listen_addr;
static uint64_t listen_retry_at;
static uint64_t started_at;

static uint64_t total_frames;
static uint64_t last_pump;
static uint64_t busy_window_start;
static uint32_t busy_pct;

/* ---- workers ------------------------------------------------------------ */

typedef struct {
    int index;
    jpeg_enc *enc;
    uint8_t *strip_mem;
    uint8_t *strip;       /* two halves: one is copied into while the other is encoded */
    uint32_t half_bytes;
    capture_ctx *cap;
    uint32_t busy_ms;     /* capturing + encoding, this second */
    void *thread;
} worker_t;

static worker_t workers[MAX_WORKERS];
static int nworkers;          /* set up */
static int running_workers;   /* actually running (a thread may fail to start) */
static bool stopping;         /* guarded by the lock, like everything shared */

static bool is_stopping(void)
{
    plat_lock();
    bool s = stopping;
    plat_unlock();
    return s;
}

static void set_nonblocking(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void client_close(client_t *c)
{
    if (c->state == C_FREE)
        return;
    if (c->body_slot >= 0) {
        slots[c->body_slot].refs--;
        c->body_slot = -1;
    }
    close(c->fd);
    c->fd = -1;
    c->state = C_FREE;
}

static int count_streams(void)
{
    int n = 0;
    for (int i = 0; i < max_clients; i++)
        n += clients[i].state == C_STREAM;
    return n;
}

static unsigned wanted_screens(void)
{
    unsigned m = 0;
    for (int i = 0; i < max_clients; i++)
        if (clients[i].state == C_STREAM)
            m |= clients[i].want;
    return m;
}

static bool pending(const client_t *c)
{
    return c->hdr_off < c->hdr_len || c->body_off < c->body_len;
}

/* ---- responses ---------------------------------------------------------- */

static void respond(client_t *c, int status, const char *type, const char *extra,
                    const void *body, uint32_t len)
{
    const char *reason = status == 200 ? "OK"
                       : status == 404 ? "Not Found"
                       : status == 405 ? "Method Not Allowed"
                       : status == 431 ? "Request Header Fields Too Large"
                       : "Service Unavailable";
    c->hdr_len = snprintf(c->hdr, sizeof(c->hdr),
                          "HTTP/1.1 %d %s\r\n"
                          "Content-Type: %s\r\n"
                          "Content-Length: %u\r\n"
                          "Cache-Control: no-cache\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "%s"
                          "Connection: close\r\n\r\n",
                          status, reason, type, (unsigned)len, extra ? extra : "");
    c->hdr_off = 0;
    c->body = body;
    c->body_len = len;
    c->body_off = 0;
    c->state = C_RESP;
}

static void respond_text(client_t *c, int status, const char *text)
{
    /* text may alias c->buf */
    respond(c, status, "text/plain; charset=utf-8", NULL, text, (uint32_t)strlen(text));
}

static void json_str(char *dst, int cap, const char *src)
{
    int n = 0;
    for (; *src && n < cap - 2; src++) {
        unsigned char ch = (unsigned char)*src;
        if (ch == '"' || ch == '\\') {
            dst[n++] = '\\';
            dst[n++] = (char)ch;
        } else if (ch >= 0x20 && ch < 0x7F) {
            dst[n++] = (char)ch;
        }
    }
    dst[n] = 0;
}

static int settings_json(char *b, int cap)
{
    return snprintf(b, (size_t)cap,
                    "{\"quality\":%d,\"fps_top\":%d,\"fps_bottom\":%d,"
                    "\"sharp\":%s,\"boost\":%s,\"boost_supported\":%s,\"fast\":%s}",
                    settings.quality, settings.fps[SCREEN_TOP], settings.fps[SCREEN_BOTTOM],
                    settings.sharp ? "true" : "false", settings.boost ? "true" : "false",
                    boost_supported ? "true" : "false", settings.fast ? "true" : "false");
}

static const char *format_name(int f)
{
    static const char *const names[] = { "RGBA8", "BGR8", "RGB565", "RGB5A1", "RGBA4" };
    return f >= 0 && f <= 4 ? names[f] : "none";
}

/* Unchanged for a while: re-checked less often. */
static bool screen_static(const screen_t *st)
{
    for (int t = 0; t < ntiles; t++)
        if (st->tile[t].static_run < STATIC_AFTER)
            return false;
    return true;
}

static int status_json(char *b, int cap)
{
    uint64_t now = plat_now_ms();
    uint32_t ip = plat_net_address();
    struct in_addr a;
    a.s_addr = ip;
    int clients_n = 0;
    for (int i = 0; i < max_clients; i++)
        clients_n += clients[i].state != C_FREE;

    int n = snprintf(b, (size_t)cap,
                     "{\"app\":\"3DSLink\",\"version\":\"%s\",\"model\":\"%s\","
                     "\"ip\":\"%s\",\"uptime\":%u,\"clients\":%d,\"streams\":%d,"
                     "\"frames\":%llu,\"settings\":",
                     cfg.version, cfg.model, ip ? inet_ntoa(a) : "",
                     (unsigned)((now - started_at) / 1000), clients_n, count_streams(),
                     (unsigned long long)total_frames);
    n += settings_json(b + n, cap - n);
    n += snprintf(b + n, (size_t)(cap - n), ",\"screens\":{");
    for (int s = 0; s < SCREEN_COUNT && n < cap; s++) {
        screen_t *st = &screens[s];
        char err[64];
        json_str(err, sizeof(err), st->error);
        n += snprintf(b + n, (size_t)(cap - n),
                      "%s\"%s\":{\"frames\":%u,\"fps\":%u.%u,\"kbps\":%u,\"frame_bytes\":%u,"
                      "\"encode_ms\":%u,\"copy_ms\":%u,\"format\":\"%s\",\"static\":%s,"
                      "\"quality\":%d,\"error\":\"%s\"}",
                      s ? "," : "", screen_names[s], (unsigned)st->frames, (unsigned)(st->fps10 / 10),
                      (unsigned)(st->fps10 % 10), (unsigned)st->kbps, (unsigned)st->last_size,
                      (unsigned)st->encode_ms, (unsigned)st->copy_ms, format_name(st->format),
                      screen_static(st) ? "true" : "false",
                      clampi(settings.quality - st->quality_cut, 10, 95), err);
    }
    n += snprintf(b + n, (size_t)(cap - n), "},\"busy\":%u,\"cores\":%d", (unsigned)busy_pct,
                  running_workers);
    n += snprintf(b + n, (size_t)(cap - n),
                  ",\"memory\":{\"slots\":%d,\"slot_kb\":%u,\"strip_kb\":%u}}",
                  nslots, (unsigned)(slot_bytes / 1024), (unsigned)(cfg.strip_bytes / 1024));
    return n < cap ? n : cap - 1;
}

/* Find `key` in a query string and copy its value. */
static bool query_get(const char *q, const char *key, char *out, int cap)
{
    size_t klen = strlen(key);
    while (q && *q) {
        if (!strncmp(q, key, klen) && q[klen] == '=') {
            q += klen + 1;
            int n = 0;
            while (*q && *q != '&' && n < cap - 1)
                out[n++] = *q++;
            out[n] = 0;
            return true;
        }
        q = strchr(q, '&');
        if (q)
            q++;
    }
    return false;
}

static void start_stream(client_t *c, const char *query)
{
    unsigned want = 0;
    char v[32];
    if (query_get(query, "screens", v, sizeof(v))) {
        if (strstr(v, "top"))
            want |= 1u << SCREEN_TOP;
        if (strstr(v, "bottom"))
            want |= 1u << SCREEN_BOTTOM;
    }
    if (!want)
        want = (1u << SCREEN_TOP) | (1u << SCREEN_BOTTOM);

    if (count_streams() >= max_streams) {
        respond_text(c, 503, "Too many viewers are connected to this 3DS.\n");
        return;
    }

    if (cfg.stream_sndbuf > 0) {
        int sz = cfg.stream_sndbuf;
        setsockopt(c->fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
    }

    c->state = C_STREAM;
    c->want = want;
    memset(c->sent_seq, 0, sizeof(c->sent_seq));
    c->rr = 0;
    c->first_part = true;
    c->hdr_len = snprintf(c->hdr, sizeof(c->hdr),
                          "HTTP/1.1 200 OK\r\n"
                          "Content-Type: multipart/x-mixed-replace; boundary=" BOUNDARY "\r\n"
                          "Cache-Control: no-store\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "X-Accel-Buffering: no\r\n"
                          "Connection: close\r\n\r\n");
    c->hdr_off = 0;
    c->body = NULL;
    c->body_len = c->body_off = 0;
    core_log("viewer connected (%s%s)", want & 1 ? "top " : "", want & 2 ? "bottom" : "");
}

static void handle_request(client_t *c)
{
    /* "GET /path?query HTTP/1.1" (parsed by hand: sscanf costs ~30 KB of code) */
    char method[8], target[256];
    const char *p = c->buf;
    size_t n = strcspn(p, " \r\n");
    if (n == 0 || n >= sizeof(method) || p[n] != ' ') {
        respond_text(c, 405, "Bad request\n");
        return;
    }
    memcpy(method, p, n);
    method[n] = 0;
    p += n + 1;
    n = strcspn(p, " \r\n");
    if (n == 0 || n >= sizeof(target)) {
        respond_text(c, 405, "Bad request\n");
        return;
    }
    memcpy(target, p, n);
    target[n] = 0;
    if (strcmp(method, "GET") && strcmp(method, "HEAD")) {
        respond_text(c, 405, "Only GET is supported\n");
        return;
    }

    char *query = strchr(target, '?');
    if (query)
        *query++ = 0;

    if (!strcmp(target, "/") || !strcmp(target, "/index.html")) {
        respond(c, 200, "text/html; charset=utf-8", "Content-Encoding: gzip\r\n",
                web_index_gz, web_index_gz_len);
    } else if (!strcmp(target, "/stream")) {
        start_stream(c, query);
    } else if (!strcmp(target, "/api/status")) {
        int n = status_json(c->buf, sizeof(c->buf));
        respond(c, 200, "application/json", NULL, c->buf, (uint32_t)n);
    } else if (!strcmp(target, "/api/settings")) {
        static const char *const keys[] = { "quality", "fps_top", "fps_bottom", "sharp", "boost", "fast" };
        bool changed = false;
        char v[16];
        for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            if (query_get(query, keys[i], v, sizeof(v))) {
                settings_set(keys[i], v);
                changed = true;
            }
        }
        if (changed) {
            for (int s = 0; s < SCREEN_COUNT; s++)
                screens[s].quality_cut = 0;
            if (boost_supported)
                plat_set_cpu_boost(settings.boost);
            settings_save();
        }
        int n = settings_json(c->buf, sizeof(c->buf));
        respond(c, 200, "application/json", NULL, c->buf, (uint32_t)n);
    } else if (!strcmp(target, "/api/log")) {
        int n = core_log_read(c->buf, sizeof(c->buf));
        respond(c, 200, "text/plain; charset=utf-8", NULL, c->buf, (uint32_t)n);
    } else {
        respond_text(c, 404, "Not found\n");
    }

    if (!strcmp(method, "HEAD") && c->state == C_RESP)
        c->body_len = 0;
}

/* ---- streaming ---------------------------------------------------------- */

/* Queue the newest unsent picture for a stream client, taking the screens'
 * tiles in turn. */
static bool queue_frame(client_t *c)
{
    const int n = SCREEN_COUNT * MAX_TILES;
    for (int k = 0; k < n; k++) {
        int idx = (c->rr + k) % n, s = idx / MAX_TILES, t = idx % MAX_TILES;
        if (t >= ntiles || !(c->want & (1u << s)))
            continue;
        int i = screens[s].tile[t].latest;
        if (i < 0 || slots[i].seq == c->sent_seq[s][t])
            continue;
        slot_t *f = &slots[i];
        c->hdr_len = snprintf(c->hdr, sizeof(c->hdr),
                              "%s--" BOUNDARY "\r\n"
                              "Content-Type: image/jpeg\r\n"
                              "Content-Length: %u\r\n"
                              "X-Screen: %s\r\n"
                              "X-Frame: %u\r\n"
                              "X-Lines: %d %d\r\n\r\n",
                              c->first_part ? "" : "\r\n", (unsigned)f->len,
                              screen_names[s], (unsigned)f->frame, f->first, f->count);
        c->hdr_off = 0;
        c->body = f->data;
        c->body_len = f->len;
        c->body_off = 0;
        c->body_slot = i;
        f->refs++;
        c->sent_seq[s][t] = f->seq;
        c->rr = (idx + 1) % n;
        c->first_part = false;
        return true;
    }
    return false;
}

/* Lets the viewer tell a still screen apart from a dead connection. */
static void queue_keepalive(client_t *c)
{
    c->hdr_len = snprintf(c->hdr, sizeof(c->hdr),
                          "%s--" BOUNDARY "\r\n"
                          "Content-Type: text/plain\r\n"
                          "Content-Length: 0\r\n\r\n",
                          c->first_part ? "" : "\r\n");
    c->hdr_off = 0;
    c->body = NULL;
    c->body_len = c->body_off = 0;
    c->first_part = false;
}

/*
 * Closing a socket right after the last send() can cost the browser the end
 * of the response (unsent data may be dropped, and a reset makes the browser
 * throw away what it hasn't read yet): a cut-off page shows up blank. So the
 * end of the response is marked with shutdown() and the socket is only closed
 * once the browser hangs up too, or after DRAIN_TIMEOUT_MS.
 */
static void finish_response(client_t *c, uint64_t now)
{
    shutdown(c->fd, SHUT_WR);
    c->state = C_DRAIN;
    c->since = now;
}

/* Push as much pending output as the socket takes right now. */
static void client_flush(client_t *c, uint64_t now)
{
    for (int rounds = 0; rounds < 4 && c->state != C_FREE; rounds++) {
        while (pending(c)) {
            const uint8_t *p;
            uint32_t len;
            if (c->hdr_off < c->hdr_len) {
                p = (const uint8_t *)c->hdr + c->hdr_off;
                len = (uint32_t)(c->hdr_len - c->hdr_off);
            } else {
                p = c->body + c->body_off;
                len = c->body_len - c->body_off;
            }
            if (len > 0x8000)
                len = 0x8000;
            int n = (int)send(c->fd, p, len, MSG_NOSIGNAL);
            if (n > 0) {
                if (c->hdr_off < c->hdr_len) {
                    c->hdr_off += n;
                } else {
                    c->body_off += (uint32_t)n;
                    if (c->body_slot >= 0)
                        screens[slots[c->body_slot].screen].bytes_window += (uint32_t)n;
                }
                c->since = now;
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
                return;
            client_close(c);
            return;
        }

        if (c->body_slot >= 0) {
            slots[c->body_slot].refs--;
            c->body_slot = -1;
        }
        if (c->state == C_RESP) {
            finish_response(c, now);
            return;
        }
        if (c->state != C_STREAM || !queue_frame(c))
            return;
    }
}

/* ---- networking --------------------------------------------------------- */

/*
 * Open the listening socket the way HzMod/ChirunoMod do on real hardware:
 * only once Wi-Fi has an address, bound to that address, without
 * SO_REUSEADDR, and with a listen backlog of 1 (an Old 3DS rejected 4 and 8
 * with errno 9). Binding to INADDR_ANY is only tried as a fallback.
 */
static void open_listener(uint64_t now)
{
    static unsigned failures;
    static bool told_wifi;
    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }
    listen_addr = plat_net_address();
    listen_retry_at = now + 1000;
    if (!listen_addr) {
        if (!told_wifi)
            core_log("waiting for Wi-Fi");
        told_wifi = true;
        return;
    }
    told_wifi = false;

    /* Log the first few failures, then only now and then, so the log on the
     * SD card doesn't fill up while we retry every second. */
    bool loud = failures < 3 || failures % 30 == 0;
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
        if (loud)
            core_log("socket() failed (errno %d)", errno);
        failures++;
        return;
    }
#ifdef TDSL_HOST
    int one = 1; /* lets the PC test harness restart instantly */
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)cfg.port);
    sa.sin_addr.s_addr = failures % 2 == 0 ? listen_addr : htonl(INADDR_ANY);
    const char *step = "bind";
    int rc = bind(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (rc == 0) {
        step = "listen";
        rc = listen(fd, 1);
    }
    if (rc < 0) {
        if (loud) {
            struct in_addr a = sa.sin_addr;
            core_log("%s on %s:%d failed (errno %d), retrying", step,
                     a.s_addr ? inet_ntoa(a) : "any address", cfg.port, errno);
        }
        close(fd);
        failures++;
        return;
    }
    set_nonblocking(fd);
    listen_fd = fd;
    failures = 0;

    struct in_addr a;
    a.s_addr = listen_addr;
    core_log("listening on http://%s:%d/", listen_addr ? inet_ntoa(a) : "0.0.0.0", cfg.port);
}

/* Shown if the 3DS has no room for another connection (it retries). */
#define BUSY_PAGE "<!doctype html><meta http-equiv=\"refresh\" content=\"2\">" \
                  "<title>3DSLink</title><p>The 3DS is busy. Trying again in a moment...</p>\n"

/* When every connection slot is taken: one that is finished already, or else
 * the oldest one the browser opened "just in case" and hasn't sent anything
 * on yet (Chrome opens spares like that when a page loads). */
static client_t *spare_client(void)
{
    client_t *best = NULL;
    for (int i = 0; i < max_clients; i++) {
        client_t *o = &clients[i];
        if (o->state == C_DRAIN)
            return o;
        if (o->state == C_REQ && o->req_len == 0 && (!best || o->since < best->since))
            best = o;
    }
    return best;
}

static void accept_clients(uint64_t now)
{
    for (;;) {
        struct sockaddr_in sa;
        socklen_t sl = sizeof(sa);
        int fd = accept(listen_fd, (struct sockaddr *)&sa, &sl);
        if (fd < 0) {
            int e = errno;
            if (e == EAGAIN || e == EWOULDBLOCK || e == EINTR || e == ECONNABORTED)
                return;
            if (e == ENOMEM || e == ENOBUFS || e == EMFILE || e == ENFILE) {
                /* Out of sockets for now: they free up as viewers hang up. */
                static uint64_t told_at;
                if (!told_at || now - told_at > 10000)
                    core_log("too many connections at once (errno %d)", e);
                told_at = now;
                return;
            }
            core_log("accept failed (errno %d), restarting listener", e);
            open_listener(now);
            return;
        }
        set_nonblocking(fd);
#ifdef TCP_NODELAY
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif
        client_t *c = NULL;
        for (int i = 0; i < max_clients && !c; i++)
            if (clients[i].state == C_FREE)
                c = &clients[i];
        if (!c && (c = spare_client()))
            client_close(c);
        if (!c) {
            char busy[320];
            int len = snprintf(busy, sizeof(busy),
                               "HTTP/1.1 503 Service Unavailable\r\n"
                               "Content-Type: text/html; charset=utf-8\r\n"
                               "Content-Length: %u\r\nRetry-After: 2\r\n"
                               "Connection: close\r\n\r\n%s",
                               (unsigned)(sizeof(BUSY_PAGE) - 1), BUSY_PAGE);
            send(fd, busy, (size_t)len, MSG_NOSIGNAL);
            close(fd);
            continue;
        }
        memset(c, 0, sizeof(*c));
        c->fd = fd;
        c->state = C_REQ;
        c->since = now;
        c->body_slot = -1;
    }
}

static void client_read(client_t *c, uint64_t now)
{
    char tmp[256];
    for (;;) {
        char *dst = c->state == C_REQ ? c->buf + c->req_len : tmp;
        int room = c->state == C_REQ ? (int)sizeof(c->buf) - 1 - c->req_len : (int)sizeof(tmp);
        if (room <= 0) {
            respond_text(c, 431, "Request too large\n");
            return;
        }
        int n = (int)recv(c->fd, dst, (size_t)room, 0);
        if (n == 0) {
            client_close(c);
            return;
        }
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                client_close(c);
            return;
        }
        if (c->state != C_REQ)
            continue; /* streams and responses ignore anything the browser sends */

        c->req_len += n;
        c->buf[c->req_len] = 0;
        c->since = now;
        if (strstr(c->buf, "\r\n\r\n") || strstr(c->buf, "\n\n")) {
            handle_request(c);
            return;
        }
    }
}

/* One round of network I/O, waiting up to timeout_ms for activity. Only the
 * first worker (the server_run() thread) calls this; the lock is let go
 * while it waits. */
static void net_pump(int timeout_ms)
{
    struct pollfd pfd[1 + 16];
    int map[1 + 16];
    int n = 0;
    uint64_t now = plat_now_ms();
    last_pump = now;

    plat_lock();
    if (listen_fd < 0 && now >= listen_retry_at)
        open_listener(now);

    if (listen_fd >= 0) {
        pfd[n].fd = listen_fd;
        pfd[n].events = POLLIN;
        pfd[n].revents = 0;
        map[n++] = -1;
    }
    for (int i = 0; i < max_clients && n < 17; i++) {
        client_t *c = &clients[i];
        if (c->state == C_FREE)
            continue;
        pfd[n].fd = c->fd;
        pfd[n].events = POLLIN | (pending(c) ? POLLOUT : 0);
        pfd[n].revents = 0;
        map[n++] = i;
    }

    plat_unlock();

    int rc = 0;
    if (n == 0) {
        /* Nothing to wait on (no network yet): just sleep a little. */
        plat_sleep_ms(timeout_ms);
    } else {
        rc = poll(pfd, (nfds_t)n, timeout_ms);
    }
    int poll_errno = errno;
    now = plat_now_ms();

    plat_lock();
    if (rc < 0 && poll_errno != EINTR)
        core_log("poll failed (errno %d)", poll_errno);

    for (int k = 0; k < n; k++) {
        short re = pfd[k].revents;
        if (map[k] < 0) {
            if (re & (POLLERR | POLLNVAL)) {
                core_log("listener error, restarting");
                open_listener(now);
            } else if (re & POLLIN) {
                accept_clients(now);
            }
            continue;
        }
        client_t *c = &clients[map[k]];
        if (c->state == C_FREE || c->fd != pfd[k].fd)
            continue;
        if (re & (POLLERR | POLLNVAL)) {
            client_close(c);
            continue;
        }
        if (re & (POLLIN | POLLHUP))
            client_read(c, now);
    }

    for (int i = 0; i < max_clients; i++) {
        client_t *c = &clients[i];
        if (c->state == C_FREE)
            continue;
        if (c->state == C_STREAM && !pending(c) && !queue_frame(c) &&
            now - c->since > KEEPALIVE_MS)
            queue_keepalive(c);
        if (pending(c))
            client_flush(c, now);
        if (c->state == C_FREE)
            continue;
        uint64_t idle = now - c->since;
        if ((c->state == C_REQ && idle > REQ_TIMEOUT_MS) ||
            (c->state == C_DRAIN && idle > DRAIN_TIMEOUT_MS) ||
            (pending(c) && idle > STALL_TIMEOUT_MS))
            client_close(c);
    }

    uint32_t ip = plat_net_address();
    if (ip != listen_addr) {
        core_log("network address changed");
        open_listener(now);
    }
    plat_unlock();
}

/* ---- capture + encode --------------------------------------------------- */

static uint32_t hash_lines(uint32_t h, const uint8_t *p, int lines, const frame_geom *g)
{
    int words = g->width * pix_bytes(g->format) / 4;
    for (int l = 0; l < lines; l++, p += g->stride) {
        const uint32_t *w = (const uint32_t *)p;
        for (int i = 0; i < words; i++)
            h = (h ^ w[i]) * 16777619u;
    }
    return h;
}

static bool geom_ok(const frame_geom *g)
{
    return g->format >= PIX_RGBA8 && g->format <= PIX_RGBA4 && g->width == 240 &&
           g->lines > 0 && g->lines <= 400 && g->stride >= g->width * pix_bytes(g->format) &&
           (g->stride & 3) == 0;
}

/* Call with the lock held. */
static void screen_error(screen_t *st, const char *msg)
{
    st->ok_logged = false;
    if (strcmp(st->error, msg)) {
        snprintf(st->error, sizeof(st->error), "%s", msg);
        core_log("%s screen: %s", screen_names[st - screens], msg);
    }
}

static void locked_screen_error(int s, const char *msg)
{
    plat_lock();
    screen_error(&screens[s], msg);
    plat_unlock();
}

/*
 * Reads lines [first, first + count) of a framebuffer a strip at a time
 * through the two halves of a worker's strip buffer: while one half is hashed
 * and encoded, the copy of the next lines into the other half is already
 * running.
 */
typedef struct {
    worker_t *w;
    int end, per_strip;
    int first, count; /* lines being copied right now (count 0: none) */
    int which;        /* half they are going into */
    uint32_t wait_ms; /* time spent waiting for copies */
} strip_reader;

static uint8_t *strip_half(worker_t *w, int which)
{
    return w->strip + (which ? w->half_bytes : 0);
}

static int reader_start(strip_reader *r, worker_t *w, int first, int count, int per_strip)
{
    r->w = w;
    r->end = first + count;
    r->per_strip = per_strip;
    r->first = first;
    r->count = per_strip < count ? per_strip : count;
    r->which = 0;
    r->wait_ms = 0;
    return capture_start(w->cap, first, r->count, strip_half(w, 0));
}

/* Waits for the next strip and starts copying the one after it. Returns how
 * many lines *out holds: 0 once the range is done, -1 if a copy failed. */
static int reader_next(strip_reader *r, const uint8_t **out)
{
    if (r->count == 0)
        return 0;
    uint64_t t = plat_now_ms();
    int rc = capture_wait(r->w->cap);
    r->wait_ms += (uint32_t)(plat_now_ms() - t);
    if (rc != 0)
        return -1;

    int n = r->count, done = r->which, next = r->first + n;
    r->count = 0;
    if (next < r->end) {
        int m = r->end - next < r->per_strip ? r->end - next : r->per_strip;
        if (capture_start(r->w->cap, next, m, strip_half(r->w, !done)) != 0)
            return -1;
        r->first = next;
        r->count = m;
        r->which = !done;
    }
    *out = strip_half(r->w, done);
    return n;
}

/* Hash lines without encoding them (cheap change detection). */
static int probe_hash(worker_t *w, const frame_geom *g, int first, int count, int per_strip,
                      uint32_t *out)
{
    uint32_t h = 2166136261u;
    strip_reader r;
    const uint8_t *lines;
    int n;
    if (reader_start(&r, w, first, count, per_strip) != 0)
        return -1;
    while ((n = reader_next(&r, &lines)) > 0)
        h = hash_lines(h, lines, n, g);
    *out = h;
    return n;
}

static bool should_stop(worker_t *w)
{
    /* Only the server_run() thread may look at the platform's stop signal. */
    return is_stopping() || (w->index == 0 && plat_should_exit());
}

typedef struct {
    int screen, tile;
    uint32_t frame; /* capture pass */
} job_t;

/* Capture one tile of a screen and, if it changed, encode and publish it. */
static void encode_tile(worker_t *w, const job_t *j, const frame_geom *gp, uint64_t t0)
{
    const int s = j->screen, t = j->tile;
    screen_t *st = &screens[s];
    tile_t *tl = &st->tile[t];
    frame_geom g = *gp;
    if (!geom_ok(&g)) {
        locked_screen_error(s, "unsupported framebuffer layout");
        return;
    }
    int first, count;
    tile_range(g.lines, t, &first, &count);

    /* What we need to know about the tile, read while holding the lock. */
    plat_lock();
    const bool fast = settings.fast, sharp = settings.sharp;
    const int quality = clampi(settings.quality - st->quality_cut, 10, 95);
    const bool same_kind = tl->latest >= 0 && tl->have_hash && g.format == tl->format &&
                           fast == tl->fast;
    const bool probe = same_kind && tl->static_run > 0;
    const uint32_t old_hash = tl->hash;
    plat_unlock();

    int per_strip = (int)(w->half_bytes / (uint32_t)g.stride);
    if (per_strip > count)
        per_strip = count;
    if (fast)
        per_strip &= ~1; /* fast mode encodes lines in pairs */
    if (per_strip < 1) {
        locked_screen_error(s, "capture buffer too small");
        return;
    }

    /* A tile that sat still last time is first checked cheaply. */
    if (probe) {
        uint32_t h;
        if (probe_hash(w, &g, first, count, per_strip, &h) != 0) {
            locked_screen_error(s, "framebuffer copy failed");
            return;
        }
        if (h == old_hash) {
            plat_lock();
            if (tl->shown_frame < j->frame)
                tl->static_run++;
            plat_unlock();
            return;
        }
    }

    plat_lock();
    int slot = pool_find_scratch(wanted_screens());
    if (slot >= 0)
        pool_take(slot);
    plat_unlock();
    if (slot < 0)
        return; /* every buffer is busy being sent; try again next time */

    frame_geom tg = g;
    tg.lines = count;
    int rc = jpeg_enc_begin(w->enc, &tg, quality, sharp, fast, slots[slot].data, slot_bytes);
    uint32_t h = 2166136261u;
    strip_reader r = { 0 };
    bool copy_failed = rc == ENC_OK && reader_start(&r, w, first, count, per_strip) != 0;
    while (rc == ENC_OK && !copy_failed) {
        const uint8_t *lines;
        int n = reader_next(&r, &lines);
        if (n == 0)
            break;
        if (n < 0) {
            copy_failed = true;
            break;
        }
        h = hash_lines(h, lines, n, &g);
        rc = jpeg_enc_lines(w->enc, lines, n);
        /* Keep viewers fed, but don't ask the network module after every
         * strip: each check is a round trip to another process. */
        if (w->index == 0 && plat_now_ms() - last_pump >= PUMP_EVERY_MS)
            net_pump(0);
        if (should_stop(w)) {
            jpeg_enc_abort(w->enc);
            plat_lock();
            pool_release(slot);
            plat_unlock();
            return;
        }
    }
    uint32_t len = 0;
    if (copy_failed)
        jpeg_enc_abort(w->enc);
    else if (rc == ENC_OK)
        rc = jpeg_enc_finish(w->enc, &len);
    uint32_t ms = (uint32_t)(plat_now_ms() - t0);

    plat_lock();
    if (copy_failed || rc != ENC_OK) {
        pool_release(slot);
        if (copy_failed) {
            screen_error(st, "framebuffer copy failed");
        } else if (rc == ENC_OVERFLOW) {
            st->quality_cut = clampi(st->quality_cut + 10, 0, 80);
            st->good_run = 0;
            core_log("%s picture too big for its buffer, lowering quality to %d", screen_names[s],
                     clampi(settings.quality - st->quality_cut, 10, 95));
        } else {
            screen_error(st, "JPEG encoding failed");
        }
        plat_unlock();
        return;
    }
    st->error[0] = 0;
    st->encode_ms = st->encode_ms ? (st->encode_ms * 3 + ms) / 4 : ms;
    st->copy_ms = (st->copy_ms * 3 + r.wait_ms) / 4;
    st->format = g.format;

    /* Another worker took this tile over and already showed something newer,
     * or the picture is identical to what viewers already have. */
    bool stale = tl->shown_frame > j->frame;
    bool same = same_kind && tl->latest >= 0 && h == tl->hash;
    if (stale || same) {
        if (same && !stale)
            tl->static_run++;
        pool_release(slot);
        plat_unlock();
        return;
    }
    tl->hash = h;
    tl->have_hash = true;
    tl->static_run = 0;
    tl->format = g.format;
    tl->fast = fast;
    tl->shown_frame = j->frame;

    /* Creep back up after an overflow forced the quality down. */
    if (st->quality_cut > 0 && ++st->good_run >= 60) {
        st->quality_cut -= 5;
        st->good_run = 0;
    }
    if (!st->ok_logged) {
        st->ok_logged = true;
        core_log("%s screen: sending pictures (%s, %u KB per tile)", screen_names[s],
                 format_name(g.format), (unsigned)((len + 1023) / 1024));
    }

    if (tl->latest >= 0)
        slots[tl->latest].latest = false;
    slot_t *f = &slots[slot];
    f->len = len;
    f->seq = ++publish_seq;
    f->screen = s;
    f->tile = t;
    f->frame = j->frame;
    f->first = first;
    f->count = count;
    f->latest = true;
    f->encoding = false;
    tl->latest = slot;
    st->last_size = len;
    if (st->counted != j->frame) {
        st->counted = j->frame;
        st->frames++;
        st->frames_window++;
        total_frames++;
    }
    plat_unlock();
}

static void run_job(worker_t *w, const job_t *j)
{
    uint64_t t0 = plat_now_ms();
    frame_geom g;
    if (capture_begin(w->cap, j->screen, &g) != 0) {
        locked_screen_error(j->screen, "screen is not available right now");
    } else {
        encode_tile(w, j, &g, t0);
        capture_end(w->cap);
    }
    plat_lock();
    tile_t *tl = &screens[j->screen].tile[j->tile];
    if (tl->busy_frame == j->frame)
        tl->busy_frame = 0;
    w->busy_ms += (uint32_t)(plat_now_ms() - t0);
    plat_unlock();
}

/* Give the rest of the system a turn (server_config.rest_pct). */
static void rest_after(const worker_t *w, uint64_t t0)
{
    int pct = w->index == 0 ? cfg.rest_pct : cfg.helper_rest_pct;
    if (pct > 0) {
        int rest = (int)((plat_now_ms() - t0) * (uint64_t)pct / 100);
        plat_sleep_ms(rest < 1 ? 1 : rest);
    }
}

/* ---- scheduling (call with the lock held) -------------------------------- */

/* Would any viewer of screen s accept a new picture right now? */
static bool screen_ready(int s)
{
    for (int i = 0; i < max_clients; i++) {
        client_t *c = &clients[i];
        if (c->state != C_STREAM || !(c->want & (1u << s)))
            continue;
        bool busy_with_s = c->body_slot >= 0 && slots[c->body_slot].screen == s && pending(c);
        if (!busy_with_s)
            return true;
    }
    return false;
}

static int interval_ms(int s)
{
    int iv = 1000 / settings.fps[s];
    if (screen_static(&screens[s]) && iv < STATIC_POLL_MS)
        iv = STATIC_POLL_MS;
    return iv;
}

/* Can this worker capture the tile now? A tile another worker has held for
 * too long (its core is busy with a game) may be taken over by the first
 * worker, so half a screen never freezes. */
static bool tile_free(const tile_t *tl, const worker_t *w, uint64_t now)
{
    return !tl->busy_frame || (w->index == 0 && now - tl->busy_since > STUCK_MS);
}

/* Pick the screen to start capturing next (stride scheduling, capped by fps). */
static int pick_screen(uint64_t now, int *wait_ms, unsigned wanted, const worker_t *w)
{
    int best = -1;
    uint64_t max_pass = 0;
    for (int s = 0; s < SCREEN_COUNT; s++)
        if ((wanted & (1u << s)) && screens[s].pass > max_pass)
            max_pass = screens[s].pass;

    for (int s = 0; s < SCREEN_COUNT; s++) {
        screen_t *st = &screens[s];
        if (!(wanted & (1u << s)))
            continue;
        /* A screen nobody watched for a while must not bank up credit. */
        if (st->pass + 200 < max_pass)
            st->pass = max_pass - 200;

        uint64_t due = st->last_start + (uint64_t)interval_ms(s);
        if (now < due) {
            int wt = (int)(due - now);
            if (wt < *wait_ms)
                *wait_ms = wt;
            continue;
        }
        if (!screen_ready(s) || !tile_free(&st->tile[0], w, now))
            continue;
        if (best < 0 || st->pass < screens[best].pass)
            best = s;
    }
    return best;
}

static bool take_job(worker_t *w, job_t *j, uint64_t now, int *wait_ms)
{
    *wait_ms = 50;
    unsigned wanted = wanted_screens();
    if (!wanted)
        return false;

    /* Hand out the rest of the passes already started first. */
    int s = -1;
    for (int k = 0; k < SCREEN_COUNT && s < 0; k++) {
        screen_t *st = &screens[k];
        if ((wanted & (1u << k)) && st->next_tile < ntiles &&
            tile_free(&st->tile[st->next_tile], w, now))
            s = k;
    }
    if (s < 0) {
        s = pick_screen(now, wait_ms, wanted, w);
        if (s < 0)
            return false;
        screen_t *st = &screens[s];
        st->last_start = now;
        st->pass += (uint64_t)interval_ms(s);
        st->frame++;
        st->next_tile = 0;
    }
    screen_t *st = &screens[s];
    tile_t *tl = &st->tile[st->next_tile];
    j->screen = s;
    j->tile = st->next_tile++;
    j->frame = st->frame;
    tl->busy_frame = j->frame;
    tl->busy_since = now;
    return true;
}

static void update_stats(uint64_t now)
{
    if (now - busy_window_start >= 1000) {
        uint64_t total = 0;
        for (int i = 0; i < nworkers; i++) {
            total += workers[i].busy_ms;
            workers[i].busy_ms = 0;
        }
        uint64_t span = (now - busy_window_start) * (uint64_t)(running_workers ? running_workers : 1);
        uint32_t pct = (uint32_t)(total * 100ull / span);
        busy_pct = pct > 100 ? 100 : pct;
        busy_window_start = now;
    }
    for (int s = 0; s < SCREEN_COUNT; s++) {
        screen_t *st = &screens[s];
        uint64_t span = now - st->window_start;
        if (span < 1000)
            continue;
        st->fps10 = (uint32_t)(st->frames_window * 10000ull / span);
        st->kbps = (uint32_t)(st->bytes_window * 8ull / span);
        st->frames_window = 0;
        st->bytes_window = 0;
        st->window_start = now;
    }
}

/* ---- lifecycle ---------------------------------------------------------- */

static void worker_free(worker_t *w)
{
    free(w->strip_mem);
    w->strip_mem = w->strip = NULL;
    jpeg_enc_destroy(w->enc);
    w->enc = NULL;
}

static bool worker_init(worker_t *w, int index)
{
    memset(w, 0, sizeof(*w));
    w->index = index;
    /* Two halves, each a whole number of cache lines (the DMA writes one
     * while the CPU reads the other). */
    uint32_t bytes = cfg.strip_bytes & ~127u;
    w->half_bytes = bytes / 2;
    w->strip_mem = malloc(bytes + 64);
    w->strip = w->strip_mem ? (uint8_t *)(((uintptr_t)w->strip_mem + 63) & ~(uintptr_t)63) : NULL;
    w->enc = jpeg_enc_create();
    w->cap = capture_ctx_create();
    if (!w->strip_mem || !w->enc || !w->cap) {
        worker_free(w);
        return false;
    }
    return true;
}

/* Grab as many / as large picture buffers as the heap allows while leaving a
 * reserve for libjpeg's per-picture allocations, worker stacks and sockets. */
static bool alloc_slots(int needed)
{
    for (slot_bytes = cfg.slot_bytes; slot_bytes >= cfg.min_slot_bytes; slot_bytes -= 2048) {
        nslots = 0;
        while (nslots < cfg.max_slots && (slots[nslots].data = malloc(slot_bytes)))
            nslots++;
        void *reserve = malloc(cfg.reserve_bytes);
        bool ok = reserve && nslots >= needed;
        free(reserve);
        if (ok)
            return true;
        while (nslots > 0)
            free(slots[--nslots].data);
    }
    return false;
}

int server_start(const server_config *config)
{
    cfg = *config;
    started_at = plat_now_ms();
    max_clients = cfg.max_clients > 16 ? 16 : cfg.max_clients;
    max_streams = cfg.max_streams;
    ntiles = cfg.tiles < 1 ? 1 : cfg.tiles > MAX_TILES ? MAX_TILES : cfg.tiles;

    settings.quality = cfg.quality;
    settings.fps[SCREEN_TOP] = cfg.fps_top;
    settings.fps[SCREEN_BOTTOM] = cfg.fps_bottom;
    settings.sharp = false;
    settings.boost = true;
    settings.fast = cfg.fast;
    settings_load();
    boost_supported = plat_set_cpu_boost(settings.boost);

    clients = calloc((size_t)max_clients, sizeof(client_t));
    slots = calloc((size_t)(cfg.max_slots > 0 ? cfg.max_slots : 1), sizeof(slot_t));
    if (!clients || !slots || !worker_init(&workers[0], 0)) {
        core_log("out of memory during startup");
        return -1;
    }
    nworkers = 1;
    for (int i = 0; i < max_clients; i++) {
        clients[i].fd = -1;
        clients[i].body_slot = -1;
    }

    /* Every tile keeps its newest picture, and each worker needs one to
     * encode into. If memory is short, settle for one worker. */
    int want_workers = cfg.workers < 1 ? 1 : cfg.workers > MAX_WORKERS ? MAX_WORKERS : cfg.workers;
    while (nworkers < want_workers && worker_init(&workers[nworkers], nworkers))
        nworkers++;
    while (!alloc_slots(SCREEN_COUNT * ntiles + nworkers)) {
        if (nworkers == 1) {
            core_log("not enough memory for picture buffers");
            return -1;
        }
        worker_free(&workers[--nworkers]);
        core_log("memory is short: using %d worker%s", nworkers, nworkers == 1 ? "" : "s");
    }
    for (int i = 0; i < nslots; i++)
        slots[i].screen = -1;

    for (int s = 0; s < SCREEN_COUNT; s++) {
        screens[s].format = -1;
        screens[s].window_start = started_at;
        screens[s].next_tile = ntiles;
        for (int t = 0; t < MAX_TILES; t++) {
            screens[s].tile[t].latest = -1;
            screens[s].tile[t].format = -1;
        }
    }
    busy_window_start = started_at;

    core_log("3DSLink %s on %s: %d x %u KB picture buffers, %u KB copy buffers, %d tiles per screen",
             cfg.version, cfg.model, nslots, (unsigned)(slot_bytes / 1024),
             (unsigned)(cfg.strip_bytes / 1024), ntiles);
    open_listener(started_at);
    return 0;
}

static void worker_main(void *arg)
{
    worker_t *w = arg;
    while (!is_stopping()) {
        job_t j;
        int wait;
        uint64_t now = plat_now_ms();
        plat_lock();
        bool got = take_job(w, &j, now, &wait);
        plat_unlock();
        if (got) {
            run_job(w, &j);
            rest_after(w, now);
        } else {
            plat_sleep_ms(wait < 1 ? 1 : wait > 5 ? 5 : wait);
        }
    }
}

void server_run(void)
{
    plat_lock();
    stopping = false;
    plat_unlock();
    for (int i = 1; i < nworkers; i++) {
        workers[i].thread = plat_thread_start(worker_main, &workers[i]);
        if (!workers[i].thread)
            core_log("could not start worker %d: using fewer CPU cores", i + 1);
    }
    int running = 1;
    for (int i = 1; i < nworkers; i++)
        running += workers[i].thread != NULL;
    plat_lock();
    running_workers = running;
    plat_unlock();
    core_log("encoding on %d CPU core%s", running, running == 1 ? "" : "s");

    uint64_t boosted_at = 0;
    worker_t *w = &workers[0];
    while (!plat_should_exit()) {
        uint64_t now = plat_now_ms();
        job_t j;
        int wait;
        plat_lock();
        update_stats(now);
        bool boost = wanted_screens() && boost_supported && settings.boost &&
                     now - boosted_at >= BOOST_REFRESH_MS;
        bool got = take_job(w, &j, now, &wait);
        plat_unlock();
        if (boost) {
            plat_set_cpu_boost(true);
            boosted_at = now;
        }
        if (got) {
            run_job(w, &j);
            rest_after(w, now);
            net_pump(0);
        } else {
            net_pump(wait < 1 ? 1 : wait);
        }
    }

    plat_lock();
    stopping = true;
    plat_unlock();
    for (int i = 1; i < nworkers; i++)
        if (workers[i].thread)
            plat_thread_join(workers[i].thread);
}

void server_stop(void)
{
    plat_lock();
    for (int i = 0; clients && i < max_clients; i++)
        client_close(&clients[i]);
    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }
    plat_unlock();
    core_log("stopped");
}
