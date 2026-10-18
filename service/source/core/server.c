/*
 * 3DSLink HTTP server + capture scheduler.
 *
 * Single-threaded and non-blocking: one loop accepts connections, serves the
 * viewer page and JSON API, and pushes MJPEG frames to stream clients. When a
 * screen is due, the loop captures it strip by strip and encodes it, pumping
 * the sockets between strips so slow Wi-Fi never stalls encoding (and vice
 * versa). A client that falls behind simply skips frames.
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
#define KEEPALIVE_MS       2000  /* empty part sent when screens stay still */
#define BOOST_REFRESH_MS  10000  /* a game launch resets the New 3DS clock */
#define STATIC_POLL_MS      100  /* how often an unchanged screen is re-checked */
#define STATIC_AFTER         15  /* unchanged captures before slowing down */

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
    for (int i = 0; i < n; i++) {
        log_ring[log_head++] = line[i];
        if (log_head == LOG_SIZE) {
            log_head = 0;
            log_full = 1;
        }
    }
#ifdef TDSL_HOST
    fwrite(line, 1, (size_t)n, stderr);
#endif
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
                     "quality=%d\nfps_top=%d\nfps_bottom=%d\nsharp=%d\nboost=%d\n",
                     settings.quality, settings.fps[SCREEN_TOP],
                     settings.fps[SCREEN_BOTTOM], settings.sharp, settings.boost);
    plat_save_settings(buf, n);
}

/* ---- frame pool --------------------------------------------------------- */

typedef struct {
    uint8_t *data;
    uint32_t len;
    uint32_t seq;
    int screen; /* -1 when empty */
    int refs;   /* clients currently sending this frame */
} slot_t;

static slot_t *slots;
static int nslots;
static uint32_t slot_bytes;
static int latest[SCREEN_COUNT] = { -1, -1 };

static bool slot_is_latest(int i)
{
    return latest[SCREEN_TOP] == i || latest[SCREEN_BOTTOM] == i;
}

/* A buffer we can encode into without disturbing anyone. */
static int pool_find_scratch(unsigned wanted)
{
    for (int i = 0; i < nslots; i++)
        if (slots[i].refs == 0 && !slot_is_latest(i))
            return i;
    /* Fall back to the cached frame of a screen nobody is watching. */
    for (int s = 0; s < SCREEN_COUNT; s++) {
        int i = latest[s];
        if (i >= 0 && !(wanted & (1u << s)) && slots[i].refs == 0)
            return i;
    }
    return -1;
}

static void pool_take(int i)
{
    for (int s = 0; s < SCREEN_COUNT; s++)
        if (latest[s] == i)
            latest[s] = -1;
    slots[i].screen = -1;
    slots[i].len = 0;
}

/* ---- screens ------------------------------------------------------------ */

typedef struct {
    uint32_t seq;
    uint64_t last_start;
    uint64_t pass;        /* stride-scheduling virtual time */
    uint32_t hash;
    bool have_hash;
    int static_run;
    int quality_cut;      /* lowered after a frame overflowed its buffer */
    int good_run;
    int format;

    /* stats, refreshed once per second */
    uint64_t window_start;
    uint32_t frames_window, bytes_window;
    uint32_t fps10, kbps;
    uint32_t encode_ms;   /* smoothed */
    uint32_t last_size;
    char error[64];
} screen_t;

static screen_t screens[SCREEN_COUNT];

/* ---- clients ------------------------------------------------------------ */

enum { C_FREE, C_REQ, C_RESP, C_STREAM };

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
    uint32_t sent_seq[SCREEN_COUNT];
    int rr;
    bool first_part;
} client_t;

static client_t *clients;
static int max_clients, max_streams;

static server_config cfg;
static jpeg_enc *enc;
static uint8_t *strip;
static uint32_t strip_bytes;
static int listen_fd = -1;
static uint32_t listen_addr;
static uint64_t listen_retry_at;
static uint64_t started_at;

static uint64_t total_frames;

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
                    "\"sharp\":%s,\"boost\":%s,\"boost_supported\":%s}",
                    settings.quality, settings.fps[SCREEN_TOP], settings.fps[SCREEN_BOTTOM],
                    settings.sharp ? "true" : "false", settings.boost ? "true" : "false",
                    boost_supported ? "true" : "false");
}

static const char *format_name(int f)
{
    static const char *const names[] = { "RGBA8", "BGR8", "RGB565", "RGB5A1", "RGBA4" };
    return f >= 0 && f <= 4 ? names[f] : "none";
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
                      "\"encode_ms\":%u,\"format\":\"%s\",\"static\":%s,"
                      "\"quality\":%d,\"error\":\"%s\"}",
                      s ? "," : "", screen_names[s], (unsigned)st->seq, (unsigned)(st->fps10 / 10),
                      (unsigned)(st->fps10 % 10), (unsigned)st->kbps, (unsigned)st->last_size,
                      (unsigned)st->encode_ms, format_name(st->format),
                      st->static_run >= STATIC_AFTER ? "true" : "false",
                      clampi(settings.quality - st->quality_cut, 10, 95), err);
    }
    n += snprintf(b + n, (size_t)(cap - n),
                  "},\"memory\":{\"slots\":%d,\"slot_kb\":%u,\"strip_kb\":%u}}",
                  nslots, (unsigned)(slot_bytes / 1024), (unsigned)(strip_bytes / 1024));
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
    c->sent_seq[0] = c->sent_seq[1] = 0;
    c->rr = SCREEN_TOP;
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
        static const char *const keys[] = { "quality", "fps_top", "fps_bottom", "sharp", "boost" };
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

/* Queue the newest unsent frame for a stream client. */
static bool queue_frame(client_t *c)
{
    for (int k = 0; k < SCREEN_COUNT; k++) {
        int s = (c->rr + k) % SCREEN_COUNT;
        int i = latest[s];
        if (!(c->want & (1u << s)) || i < 0 || slots[i].seq == c->sent_seq[s])
            continue;
        slot_t *f = &slots[i];
        c->hdr_len = snprintf(c->hdr, sizeof(c->hdr),
                              "%s--" BOUNDARY "\r\n"
                              "Content-Type: image/jpeg\r\n"
                              "Content-Length: %u\r\n"
                              "X-Screen: %s\r\n"
                              "X-Frame: %u\r\n\r\n",
                              c->first_part ? "" : "\r\n", (unsigned)f->len,
                              screen_names[s], (unsigned)f->seq);
        c->hdr_off = 0;
        c->body = f->data;
        c->body_len = f->len;
        c->body_off = 0;
        c->body_slot = i;
        f->refs++;
        c->sent_seq[s] = f->seq;
        c->rr = (s + 1) % SCREEN_COUNT;
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
            client_close(c);
            return;
        }
        if (c->state != C_STREAM || !queue_frame(c))
            return;
    }
}

/* ---- networking --------------------------------------------------------- */

static void open_listener(uint64_t now)
{
    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }
    listen_addr = plat_net_address();

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
        core_log("socket() failed: %d", errno);
        listen_retry_at = now + 1000;
        return;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)cfg.port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(fd, 8) < 0) {
        core_log("port %d busy (errno %d), retrying", cfg.port, errno);
        close(fd);
        listen_retry_at = now + 1000;
        return;
    }
    set_nonblocking(fd);
    listen_fd = fd;

    struct in_addr a;
    a.s_addr = listen_addr;
    core_log("listening on http://%s:%d/", listen_addr ? inet_ntoa(a) : "0.0.0.0", cfg.port);
}

static void accept_clients(uint64_t now)
{
    for (;;) {
        struct sockaddr_in sa;
        socklen_t sl = sizeof(sa);
        int fd = accept(listen_fd, (struct sockaddr *)&sa, &sl);
        if (fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR &&
                errno != ECONNABORTED) {
                core_log("accept failed (errno %d), restarting listener", errno);
                open_listener(now);
            }
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
        if (!c) {
            static const char busy[] =
                "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n"
                "Connection: close\r\n\r\n";
            send(fd, busy, sizeof(busy) - 1, MSG_NOSIGNAL);
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

/* One round of network I/O, waiting up to timeout_ms for activity. */
static void net_pump(int timeout_ms)
{
    struct pollfd pfd[1 + 16];
    int map[1 + 16];
    int n = 0;
    uint64_t now = plat_now_ms();

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

    if (n == 0) {
        /* Nothing to wait on (no network yet): just sleep a little. */
        plat_sleep_ms(timeout_ms);
    } else if (poll(pfd, (nfds_t)n, timeout_ms) < 0) {
        if (errno != EINTR)
            core_log("poll failed (errno %d)", errno);
    }
    now = plat_now_ms();

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
            (pending(c) && idle > STALL_TIMEOUT_MS))
            client_close(c);
    }

    uint32_t ip = plat_net_address();
    if (ip != listen_addr) {
        core_log("network address changed");
        open_listener(now);
    }
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

static void screen_error(screen_t *st, const char *msg)
{
    if (strcmp(st->error, msg)) {
        snprintf(st->error, sizeof(st->error), "%s", msg);
        core_log("%s", msg);
    }
}

/* Hash the framebuffer without encoding it (cheap change detection). */
static int probe_hash(int s, const frame_geom *g, int per_strip, uint32_t *out)
{
    uint32_t h = 2166136261u;
    for (int first = 0; first < g->lines; first += per_strip) {
        int n = g->lines - first < per_strip ? g->lines - first : per_strip;
        if (capture_lines(s, first, n, strip) != 0)
            return -1;
        h = hash_lines(h, strip, n, g);
    }
    *out = h;
    return 0;
}

static void produce_frame(int s, unsigned wanted, const frame_geom *gp, uint64_t t0);

static void produce(int s, unsigned wanted)
{
    frame_geom g;
    uint64_t t0 = plat_now_ms();
    if (capture_begin(s, &g) != 0) {
        screen_error(&screens[s], "screen is not available right now");
        return;
    }
    produce_frame(s, wanted, &g, t0);
    capture_end(s);
}

static void produce_frame(int s, unsigned wanted, const frame_geom *gp, uint64_t t0)
{
    screen_t *st = &screens[s];
    frame_geom g = *gp;
    if (!geom_ok(&g)) {
        screen_error(st, "unsupported framebuffer layout");
        return;
    }
    int per_strip = (int)(strip_bytes / (uint32_t)g.stride);
    if (per_strip > g.lines)
        per_strip = g.lines;
    if (per_strip < 1) {
        screen_error(st, "capture buffer too small");
        return;
    }

    bool changed_format = g.format != st->format;
    st->format = g.format;
    /* Unchanged frames are only skipped while viewers can still be given the
     * cached copy; once that buffer was recycled we must encode again. */
    bool cached = latest[s] >= 0;

    /* A screen that sat still last time is first checked cheaply. */
    if (st->static_run > 0 && st->have_hash && !changed_format && cached) {
        uint32_t h;
        if (probe_hash(s, &g, per_strip, &h) != 0) {
            screen_error(st, "framebuffer copy failed");
            return;
        }
        if (h == st->hash) {
            st->static_run++;
            return;
        }
    }

    int slot = pool_find_scratch(wanted);
    if (slot < 0)
        return;
    pool_take(slot);

    int quality = clampi(settings.quality - st->quality_cut, 10, 95);
    int rc = jpeg_enc_begin(enc, &g, quality, settings.sharp, slots[slot].data, slot_bytes);
    uint32_t h = 2166136261u;
    for (int first = 0; rc == ENC_OK && first < g.lines; first += per_strip) {
        int n = g.lines - first < per_strip ? g.lines - first : per_strip;
        if (capture_lines(s, first, n, strip) != 0) {
            jpeg_enc_abort(enc);
            screen_error(st, "framebuffer copy failed");
            return;
        }
        h = hash_lines(h, strip, n, &g);
        rc = jpeg_enc_lines(enc, strip, n);
        net_pump(0);
        if (plat_should_exit()) {
            jpeg_enc_abort(enc);
            return;
        }
    }

    uint32_t len = 0;
    if (rc == ENC_OK)
        rc = jpeg_enc_finish(enc, &len);
    if (rc == ENC_OVERFLOW) {
        st->quality_cut = clampi(st->quality_cut + 10, 0, 80);
        st->good_run = 0;
        core_log("%s frame too big for buffer, lowering quality to %d", screen_names[s],
                 clampi(settings.quality - st->quality_cut, 10, 95));
        return;
    }
    if (rc != ENC_OK) {
        screen_error(st, "JPEG encoding failed");
        return;
    }
    st->error[0] = 0;

    uint32_t ms = (uint32_t)(plat_now_ms() - t0);
    st->encode_ms = st->encode_ms ? (st->encode_ms * 3 + ms) / 4 : ms;

    if (st->have_hash && h == st->hash && !changed_format && cached) {
        st->static_run++;
        return; /* identical to what viewers already have */
    }
    st->hash = h;
    st->have_hash = true;
    st->static_run = 0;

    /* Creep back up after an overflow forced the quality down. */
    if (st->quality_cut > 0 && ++st->good_run >= 60) {
        st->quality_cut -= 5;
        st->good_run = 0;
    }

    slots[slot].len = len;
    slots[slot].seq = ++st->seq;
    slots[slot].screen = s;
    latest[s] = slot;
    st->last_size = len;
    st->frames_window++;
    total_frames++;
}

/* Would any viewer of screen s accept a new frame right now? */
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
    if (screens[s].static_run >= STATIC_AFTER && iv < STATIC_POLL_MS)
        iv = STATIC_POLL_MS;
    return iv;
}

/* Pick the screen to capture next (stride scheduling, capped by fps). */
static int pick_screen(uint64_t now, int *wait_ms, unsigned wanted)
{
    int best = -1;
    *wait_ms = 50;
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
            int w = (int)(due - now);
            if (w < *wait_ms)
                *wait_ms = w;
            continue;
        }
        if (!screen_ready(s))
            continue;
        if (best < 0 || st->pass < screens[best].pass)
            best = s;
    }
    return best;
}

static void update_stats(uint64_t now)
{
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

int server_start(const server_config *config)
{
    cfg = *config;
    started_at = plat_now_ms();
    max_clients = cfg.max_clients > 16 ? 16 : cfg.max_clients;
    max_streams = cfg.max_streams;

    settings.quality = cfg.quality;
    settings.fps[SCREEN_TOP] = cfg.fps_top;
    settings.fps[SCREEN_BOTTOM] = cfg.fps_bottom;
    settings.sharp = false;
    settings.boost = true;
    settings_load();
    boost_supported = plat_set_cpu_boost(settings.boost);

    clients = calloc((size_t)max_clients, sizeof(client_t));
    enc = jpeg_enc_create();
    strip_bytes = cfg.strip_bytes;
    strip = malloc(strip_bytes);
    if (!clients || !enc || !strip) {
        core_log("out of memory during startup");
        return -1;
    }
    for (int i = 0; i < max_clients; i++) {
        clients[i].fd = -1;
        clients[i].body_slot = -1;
    }

    /* Grab as many / as large frame buffers as the heap allows while leaving
     * a reserve for libjpeg's per-frame allocations and socket buffers. */
    slots = calloc((size_t)cfg.max_slots, sizeof(slot_t));
    if (!slots)
        return -1;
    for (slot_bytes = cfg.slot_bytes; slot_bytes >= cfg.min_slot_bytes; slot_bytes -= 4096) {
        nslots = 0;
        while (nslots < cfg.max_slots && (slots[nslots].data = malloc(slot_bytes)))
            nslots++;
        void *reserve = malloc(cfg.reserve_bytes);
        if (reserve && nslots >= 3) {
            free(reserve);
            break;
        }
        free(reserve);
        while (nslots > 0)
            free(slots[--nslots].data);
    }
    if (nslots < 3) {
        core_log("not enough memory for frame buffers");
        return -1;
    }
    for (int i = 0; i < nslots; i++)
        slots[i].screen = -1;

    for (int s = 0; s < SCREEN_COUNT; s++) {
        screens[s].format = -1;
        screens[s].window_start = started_at;
    }

    core_log("3DSLink %s on %s: %d x %u KB frame buffers, %u KB strips", cfg.version,
             cfg.model, nslots, (unsigned)(slot_bytes / 1024), (unsigned)(strip_bytes / 1024));
    open_listener(started_at);
    return 0;
}

void server_run(void)
{
    uint64_t boosted_at = 0;
    while (!plat_should_exit()) {
        uint64_t now = plat_now_ms();
        update_stats(now);

        unsigned wanted = wanted_screens();
        if (wanted && boost_supported && settings.boost && now - boosted_at >= BOOST_REFRESH_MS) {
            plat_set_cpu_boost(true);
            boosted_at = now;
        }
        int wait = 50;
        int s = wanted ? pick_screen(now, &wait, wanted) : -1;
        if (s >= 0) {
            screens[s].last_start = now;
            screens[s].pass += (uint64_t)interval_ms(s);
            produce(s, wanted);
            net_pump(0);
        } else {
            net_pump(wait < 1 ? 1 : wait);
        }
    }
}

void server_stop(void)
{
    for (int i = 0; clients && i < max_clients; i++)
        client_close(&clients[i]);
    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }
    core_log("stopped");
}
