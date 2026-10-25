#include "http.h"

#include <ctype.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../platform/platform.h"
#include "log.h"
#include "net.h"
#include "tls.h"

#define HS_BLOCK (256 * 1024)
#define HS_SLOTS 32
#define HS_PREFETCH 4
#define HTTP_TIMEOUT_MS 15000
#define HTTP_POLL_MS 250
#define MAX_REDIRECTS 6

int http_parse_url(const char *url, http_url *out)
{
    const char *p;
    memset(out, 0, sizeof(*out));
    if (strncasecmp(url, "http://", 7) == 0) {
        out->tls = 0;
        out->port = 80;
        p = url + 7;
    } else if (strncasecmp(url, "https://", 8) == 0) {
        out->tls = 1;
        out->port = 443;
        p = url + 8;
    } else {
        return -1;
    }
    const char *host_end = p + strcspn(p, "/?#");
    const char *at = memchr(p, '@', (size_t)(host_end - p));
    if (at)
        p = at + 1; /* drop userinfo */
    const char *colon = NULL;
    if (*p == '[')
        return -1; /* IPv6 literals are not supported */
    colon = memchr(p, ':', (size_t)(host_end - p));
    size_t hlen = (size_t)((colon ? colon : host_end) - p);
    if (hlen == 0 || hlen >= sizeof(out->host))
        return -1;
    memcpy(out->host, p, hlen);
    if (colon) {
        long port = strtol(colon + 1, NULL, 10);
        if (port <= 0 || port > 65535)
            return -1;
        out->port = (uint16_t)port;
    }
    const char *path = host_end;
    size_t plen = strcspn(path, "#");
    if (plen == 0) {
        strcpy(out->path, "/");
    } else {
        if (plen + 2 >= sizeof(out->path))
            return -1;
        if (*path == '?')
            out->path[0] = '/';
        strncat(out->path, path, plen);
    }
    /* Escape spaces, which some senders leave unencoded. */
    if (strchr(out->path, ' ')) {
        char tmp[sizeof(out->path)];
        size_t o = 0;
        for (const char *s = out->path; *s && o + 4 < sizeof(tmp); s++) {
            if (*s == ' ') {
                memcpy(tmp + o, "%20", 3);
                o += 3;
            } else {
                tmp[o++] = *s;
            }
        }
        tmp[o] = '\0';
        strcpy(out->path, tmp);
    }
    return 0;
}

/* --- connection with buffered reads --- */

typedef struct http_conn {
    tstream ts;
    int open;
    int tls;
    char host[256];
    uint16_t port;
    uint8_t buf[16384];
    size_t pos, len;
    atomic_int *abort_flag;
    atomic_int phase; /* conn_phase, for http_source_debug() */
} http_conn;

enum conn_phase { PH_IDLE, PH_CONNECT, PH_SEND, PH_HEAD, PH_BODY, PH_SKIP };
static const char *const g_phase_names[] = { "idle", "connect", "send", "head", "body", "skip" };

typedef struct http_resp {
    int status;
    int64_t content_length;
    int64_t range_start;
    int64_t range_total;
    int keep_alive;
    int chunked;
    char location[3072];
    char content_type[128];
} http_resp;

typedef struct body_reader {
    http_conn *c;
    int chunked;
    int64_t remaining; /* -1: until EOF */
    int done;
} body_reader;

static void conn_close(http_conn *c)
{
    if (c->open)
        tstream_close(&c->ts);
    c->open = 0;
    c->pos = c->len = 0;
}

static int conn_open(http_conn *c, const http_url *u)
{
    if (c->open && c->tls == u->tls && c->port == u->port && strcmp(c->host, u->host) == 0)
        return 0;
    conn_close(c);
    if (tstream_connect(&c->ts, u->host, u->port, u->tls, HTTP_TIMEOUT_MS) != 0)
        return -1;
    c->open = 1;
    c->tls = u->tls;
    c->port = u->port;
    snprintf(c->host, sizeof(c->host), "%s", u->host);
    return 0;
}

static int conn_fill(http_conn *c)
{
    int waited = 0;
    if (c->pos < c->len)
        return 1;
    c->pos = c->len = 0;
    for (;;) {
        if (c->abort_flag && *c->abort_flag)
            return -1;
        int n = tstream_read(&c->ts, c->buf, sizeof(c->buf), HTTP_POLL_MS);
        if (n == NET_TIMEOUT) {
            waited += HTTP_POLL_MS;
            if (waited >= HTTP_TIMEOUT_MS)
                return -1;
            continue;
        }
        if (n <= 0)
            return n == 0 ? 0 : -1;
        c->len = (size_t)n;
        return 1;
    }
}

static int conn_read(http_conn *c, uint8_t *dst, size_t n)
{
    int rc = conn_fill(c);
    if (rc <= 0)
        return rc;
    size_t avail = c->len - c->pos;
    if (n > avail)
        n = avail;
    memcpy(dst, c->buf + c->pos, n);
    c->pos += n;
    return (int)n;
}

static int conn_read_line(http_conn *c, char *line, size_t cap)
{
    size_t o = 0;
    for (;;) {
        int rc = conn_fill(c);
        if (rc <= 0)
            return -1;
        char ch = (char)c->buf[c->pos++];
        if (ch == '\n')
            break;
        if (ch != '\r' && o + 1 < cap)
            line[o++] = ch;
    }
    line[o] = '\0';
    return (int)o;
}

static int send_request(http_conn *c, const http_url *u, int64_t start, int64_t end)
{
    char req[4096];
    char range[96] = "";
    char hosthdr[300];

    if (start >= 0) {
        if (end >= 0)
            snprintf(range, sizeof(range), "Range: bytes=%lld-%lld\r\n", (long long)start,
                     (long long)end);
        else
            snprintf(range, sizeof(range), "Range: bytes=%lld-\r\n", (long long)start);
    }
    if ((u->tls && u->port == 443) || (!u->tls && u->port == 80))
        snprintf(hosthdr, sizeof(hosthdr), "%s", u->host);
    else
        snprintf(hosthdr, sizeof(hosthdr), "%s:%u", u->host, (unsigned)u->port);
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: VitaCast/1.0 (PlayStation Vita)\r\n"
                     "Accept: */*\r\n"
                     "Accept-Encoding: identity\r\n"
                     "%s"
                     "Connection: keep-alive\r\n\r\n",
                     u->path, hosthdr, range);
    if (n <= 0 || (size_t)n >= sizeof(req))
        return -1;
    return tstream_write(&c->ts, req, (size_t)n);
}

static int read_response_head(http_conn *c, http_resp *r)
{
    char line[4096];
    memset(r, 0, sizeof(*r));
    r->content_length = -1;
    r->range_start = -1;
    r->range_total = -1;
    r->keep_alive = 1;

    if (conn_read_line(c, line, sizeof(line)) < 0)
        return -1;
    if (strncmp(line, "HTTP/", 5) != 0)
        return -1;
    const char *sp = strchr(line, ' ');
    if (!sp)
        return -1;
    r->status = atoi(sp + 1);
    if (strncmp(line, "HTTP/1.0", 8) == 0)
        r->keep_alive = 0;

    for (;;) {
        int n = conn_read_line(c, line, sizeof(line));
        if (n < 0)
            return -1;
        if (n == 0)
            break;
        char *colon = strchr(line, ':');
        if (!colon)
            continue;
        *colon = '\0';
        char *v = colon + 1;
        while (*v == ' ' || *v == '\t')
            v++;
        if (strcasecmp(line, "Content-Length") == 0) {
            r->content_length = strtoll(v, NULL, 10);
        } else if (strcasecmp(line, "Content-Range") == 0) {
            /* bytes 0-1023/4096 */
            const char *s = strchr(v, ' ');
            s = s ? s + 1 : v;
            r->range_start = strtoll(s, NULL, 10);
            const char *slash = strchr(s, '/');
            if (slash && slash[1] != '*')
                r->range_total = strtoll(slash + 1, NULL, 10);
        } else if (strcasecmp(line, "Connection") == 0) {
            if (strncasecmp(v, "close", 5) == 0)
                r->keep_alive = 0;
            else if (strncasecmp(v, "keep-alive", 10) == 0)
                r->keep_alive = 1;
        } else if (strcasecmp(line, "Transfer-Encoding") == 0) {
            if (strstr(v, "chunked"))
                r->chunked = 1;
        } else if (strcasecmp(line, "Location") == 0) {
            snprintf(r->location, sizeof(r->location), "%s", v);
        } else if (strcasecmp(line, "Content-Type") == 0) {
            snprintf(r->content_type, sizeof(r->content_type), "%s", v);
        }
    }
    return 0;
}

static void body_init(body_reader *b, http_conn *c, const http_resp *r)
{
    b->c = c;
    b->chunked = r->chunked;
    b->done = 0;
    if (r->chunked)
        b->remaining = 0;
    else if (r->content_length >= 0)
        b->remaining = r->content_length;
    else
        b->remaining = -1;
    if (!r->chunked && r->content_length == 0)
        b->done = 1;
}

static int body_read(body_reader *b, uint8_t *dst, size_t n)
{
    char line[128];
    if (b->done)
        return 0;
    if (b->chunked && b->remaining == 0) {
        if (conn_read_line(b->c, line, sizeof(line)) < 0)
            return -1;
        b->remaining = strtoll(line, NULL, 16);
        if (b->remaining <= 0) {
            /* trailers */
            while (conn_read_line(b->c, line, sizeof(line)) > 0)
                ;
            b->done = 1;
            return 0;
        }
    }
    if (b->remaining >= 0 && (int64_t)n > b->remaining)
        n = (size_t)b->remaining;
    int rc = conn_read(b->c, dst, n);
    if (rc == 0) {
        if (b->remaining < 0) {
            b->done = 1;
            return 0;
        }
        return -1; /* premature EOF */
    }
    if (rc < 0)
        return -1;
    if (b->remaining > 0) {
        b->remaining -= rc;
        if (b->remaining == 0) {
            if (b->chunked)
                conn_read_line(b->c, line, sizeof(line)); /* CRLF after chunk */
            else
                b->done = 1;
        }
    }
    return rc;
}

/* Reads exactly n bytes from the body (fewer only at end). Returns count or -1. */
static int64_t body_read_full(body_reader *b, uint8_t *dst, size_t n)
{
    size_t got = 0;
    while (got < n) {
        int rc = body_read(b, dst ? dst + got : NULL, n - got);
        if (rc < 0)
            return -1;
        if (rc == 0)
            break;
        got += (size_t)rc;
    }
    return (int64_t)got;
}

static int body_skip(body_reader *b, int64_t n)
{
    uint8_t tmp[8192];
    while (n > 0) {
        size_t want = n > (int64_t)sizeof(tmp) ? sizeof(tmp) : (size_t)n;
        int rc = body_read(b, tmp, want);
        if (rc <= 0)
            return -1;
        n -= rc;
    }
    return 0;
}

static int fits(int n, size_t cap)
{
    return n >= 0 && (size_t)n < cap ? 0 : -1;
}

/* Resolves a Location header against the current URL. Fails if the result
 * does not fit in out. */
static int resolve_location(const http_url *base, const char *loc, char *out, size_t cap)
{
    if (strncasecmp(loc, "http://", 7) == 0 || strncasecmp(loc, "https://", 8) == 0)
        return fits(snprintf(out, cap, "%s", loc), cap);
    const char *scheme = base->tls ? "https" : "http";
    if (loc[0] == '/' && loc[1] == '/')
        return fits(snprintf(out, cap, "%s:%s", scheme, loc), cap);
    char hostport[300];
    snprintf(hostport, sizeof(hostport), "%s:%u", base->host, (unsigned)base->port);
    if (loc[0] == '/')
        return fits(snprintf(out, cap, "%s://%s%s", scheme, hostport, loc), cap);
    /* Relative to the current directory. */
    char dir[sizeof(base->path)];
    snprintf(dir, sizeof(dir), "%s", base->path);
    char *q = strchr(dir, '?');
    if (q)
        *q = '\0';
    char *slash = strrchr(dir, '/');
    if (slash)
        slash[1] = '\0';
    return fits(snprintf(out, cap, "%s://%s%s%s", scheme, hostport, dir, loc), cap);
}

/* --- cached source --- */

enum { SLOT_EMPTY = 0, SLOT_LOADING, SLOT_READY };

typedef struct hs_slot {
    int64_t block;
    int state;
    uint32_t len;
    uint64_t last_used;
    uint8_t *data;
} hs_slot;

struct http_source {
    http_url url;
    int64_t size;
    int ranges_ok;
    char content_type[128];

    pthread_mutex_t lock;
    pthread_cond_t cond;
    hs_slot slots[HS_SLOTS];
    uint64_t use_counter;

    pthread_mutex_t reader_lock;
    http_conn reader;
    http_conn prefetcher;

    pthread_t prefetch_thread;
    int prefetch_started;
    int64_t pf_from, pf_to;
    int closing;
    atomic_int aborted;
    /* where each thread is, for http_source_debug() */
    atomic_int reader_waiting;
    atomic_int prefetch_waiting;
    atomic_llong prefetch_block;
};

/* Fetches block `blk` into dst using connection c. Returns bytes or -1. */
static int64_t fetch_block(http_source *hs, http_conn *c, int64_t blk, uint8_t *dst)
{
    int64_t start = blk * HS_BLOCK;
    int64_t end = start + HS_BLOCK - 1;
    if (end >= hs->size)
        end = hs->size - 1;
    int64_t want = end - start + 1;

    for (int attempt = 0; attempt < 3 && !hs->aborted; attempt++) {
        http_resp r;
        body_reader b;
        int ok;
        c->phase = PH_CONNECT;
        ok = conn_open(c, &hs->url) == 0;
        c->phase = PH_SEND;
        ok = ok && send_request(c, &hs->url, start, end) == 0;
        c->phase = PH_HEAD;
        ok = ok && read_response_head(c, &r) == 0;
        if (!ok) {
            c->phase = PH_IDLE;
            if (!hs->aborted)
                LOGI("HTTP: no response for bytes %lld- (attempt %d)", (long long)start, attempt + 1);
            conn_close(c);
            continue;
        }
        body_init(&b, c, &r);
        int64_t got = -1;
        int reusable = r.keep_alive;
        c->phase = PH_BODY;
        if (r.status == 206 && r.range_start == start) {
            got = body_read_full(&b, dst, (size_t)want);
            if (got == want && !b.done) {
                uint8_t extra;
                /* Consumes the final chunk marker; any real data means the
                 * server sent more than asked, so drop the connection. */
                if (body_read(&b, &extra, 1) != 0)
                    reusable = 0;
            }
        } else if (r.status == 200) {
            /* Server ignores ranges: skip to the block from the start. */
            hs->ranges_ok = 0;
            c->phase = PH_SKIP;
            if (body_skip(&b, start) == 0)
                got = body_read_full(&b, dst, (size_t)want);
            reusable = 0;
        } else {
            LOGI("HTTP: unexpected status %d for range %lld", r.status, (long long)start);
            reusable = 0;
        }
        if (!reusable || !b.done)
            conn_close(c);
        c->phase = PH_IDLE;
        if (got == want)
            return got;
        if (!hs->aborted)
            LOGI("HTTP: short read at %lld: %lld of %lld bytes (attempt %d)", (long long)start,
                 (long long)got, (long long)want, attempt + 1);
        conn_close(c);
    }
    return -1;
}

static hs_slot *find_slot(http_source *hs, int64_t blk)
{
    for (int i = 0; i < HS_SLOTS; i++)
        if (hs->slots[i].state != SLOT_EMPTY && hs->slots[i].block == blk)
            return &hs->slots[i];
    return NULL;
}

/* Picks a slot to reuse, preferring empty ones and avoiding [keep_from, keep_to]. */
static hs_slot *evict_slot(http_source *hs, int64_t keep_from, int64_t keep_to)
{
    hs_slot *best = NULL;
    for (int i = 0; i < HS_SLOTS; i++) {
        hs_slot *s = &hs->slots[i];
        if (s->state == SLOT_EMPTY)
            return s;
        if (s->state != SLOT_READY)
            continue;
        if (s->block >= keep_from && s->block <= keep_to)
            continue;
        if (!best || s->last_used < best->last_used)
            best = s;
    }
    if (!best) {
        for (int i = 0; i < HS_SLOTS; i++) {
            hs_slot *s = &hs->slots[i];
            if (s->state == SLOT_READY && (!best || s->last_used < best->last_used))
                best = s;
        }
    }
    return best;
}

static void *prefetch_main(void *arg)
{
    http_source *hs = arg;
    pthread_mutex_lock(&hs->lock);
    while (!hs->closing) {
        int64_t blk = -1;
        if (!hs->aborted && hs->ranges_ok) {
            for (int64_t b = hs->pf_from; b <= hs->pf_to; b++) {
                if (b * HS_BLOCK >= hs->size)
                    break;
                if (!find_slot(hs, b)) {
                    blk = b;
                    break;
                }
            }
        }
        if (blk < 0) {
            hs->prefetch_waiting = 1;
            pthread_cond_wait(&hs->cond, &hs->lock);
            hs->prefetch_waiting = 0;
            continue;
        }
        hs_slot *s = evict_slot(hs, hs->pf_from - 1, hs->pf_to);
        if (!s) {
            pthread_cond_wait(&hs->cond, &hs->lock);
            continue;
        }
        s->block = blk;
        s->state = SLOT_LOADING;
        hs->prefetch_block = blk;
        pthread_mutex_unlock(&hs->lock);
        int64_t n = fetch_block(hs, &hs->prefetcher, blk, s->data);
        pthread_mutex_lock(&hs->lock);
        if (n < 0) {
            s->state = SLOT_EMPTY;
            if (hs->pf_to >= blk)
                hs->pf_to = blk - 1; /* let the reader retry on demand */
        } else {
            s->len = (uint32_t)n;
            s->state = SLOT_READY;
            s->last_used = ++hs->use_counter;
        }
        pthread_cond_broadcast(&hs->cond);
    }
    pthread_mutex_unlock(&hs->lock);
    return NULL;
}

static void hs_free(http_source *hs)
{
    for (int i = 0; i < HS_SLOTS; i++)
        free(hs->slots[i].data);
    conn_close(&hs->reader);
    conn_close(&hs->prefetcher);
    pthread_mutex_destroy(&hs->lock);
    pthread_mutex_destroy(&hs->reader_lock);
    pthread_cond_destroy(&hs->cond);
    free(hs);
}

http_source *http_source_open(const char *url, char *err, size_t errcap)
{
    char cur[3072];
    http_source *hs = calloc(1, sizeof(*hs));
    if (!hs) {
        snprintf(err, errcap, "out of memory");
        return NULL;
    }
    pthread_mutex_init(&hs->lock, NULL);
    pthread_mutex_init(&hs->reader_lock, NULL);
    pthread_cond_init(&hs->cond, NULL);
    hs->reader.abort_flag = &hs->aborted;
    hs->prefetcher.abort_flag = &hs->aborted;
    for (int i = 0; i < HS_SLOTS; i++) {
        hs->slots[i].data = malloc(HS_BLOCK);
        if (!hs->slots[i].data) {
            snprintf(err, errcap, "out of memory");
            hs_free(hs);
            return NULL;
        }
    }

    snprintf(cur, sizeof(cur), "%s", url);
    for (int redirects = 0;; redirects++) {
        http_resp r;
        body_reader b;
        if (http_parse_url(cur, &hs->url) != 0) {
            snprintf(err, errcap, "unsupported URL");
            goto fail;
        }
        if (conn_open(&hs->reader, &hs->url) != 0) {
            snprintf(err, errcap, "cannot connect to %s", hs->url.host);
            goto fail;
        }
        if (send_request(&hs->reader, &hs->url, 0, HS_BLOCK - 1) != 0 ||
            read_response_head(&hs->reader, &r) != 0) {
            snprintf(err, errcap, "no HTTP response from %s", hs->url.host);
            goto fail;
        }
        if (r.status >= 300 && r.status < 400 && r.location[0]) {
            conn_close(&hs->reader);
            if (redirects >= MAX_REDIRECTS) {
                snprintf(err, errcap, "too many redirects");
                goto fail;
            }
            if (resolve_location(&hs->url, r.location, cur, sizeof(cur)) != 0) {
                snprintf(err, errcap, "redirect URL too long");
                goto fail;
            }
            continue;
        }
        snprintf(hs->content_type, sizeof(hs->content_type), "%s", r.content_type);
        body_init(&b, &hs->reader, &r);
        if (r.status == 206 && r.range_start == 0 && r.range_total > 0) {
            hs->size = r.range_total;
            hs->ranges_ok = 1;
        } else if (r.status == 200 && r.content_length > 0) {
            hs->size = r.content_length;
            hs->ranges_ok = 0;
        } else {
            if (r.status == 200)
                snprintf(err, errcap, "server did not report a size (live streams are not supported)");
            else
                snprintf(err, errcap, "HTTP error %d", r.status);
            goto fail;
        }
        /* Keep the first block: every container probe starts there. */
        int64_t want = hs->size < HS_BLOCK ? hs->size : HS_BLOCK;
        hs_slot *s = &hs->slots[0];
        int64_t got = body_read_full(&b, s->data, (size_t)want);
        if (got != want) {
            snprintf(err, errcap, "read error");
            goto fail;
        }
        s->block = 0;
        s->len = (uint32_t)got;
        s->state = SLOT_READY;
        s->last_used = ++hs->use_counter;
        if (!hs->ranges_ok || !r.keep_alive || !b.done)
            conn_close(&hs->reader);
        break;
    }

    if (hs->ranges_ok) {
        hs->pf_from = 1;
        hs->pf_to = 0;
        if (pthread_create(&hs->prefetch_thread, NULL, prefetch_main, hs) == 0)
            hs->prefetch_started = 1;
    } else {
        LOGI("HTTP: server does not support ranges; seeking will be slow");
    }
    return hs;
fail:
    hs_free(hs);
    return NULL;
}

int64_t http_source_size(const http_source *hs)
{
    return hs->size;
}

const char *http_source_content_type(const http_source *hs)
{
    return hs->content_type;
}

int http_source_read(http_source *hs, uint8_t *buf, uint64_t pos, uint32_t len)
{
    int total = 0;
    int64_t last_blk = -1;

    pthread_mutex_lock(&hs->lock);
    while (len > 0) {
        if (hs->aborted) {
            total = -1;
            break;
        }
        if ((int64_t)pos >= hs->size)
            break;
        int64_t blk = (int64_t)(pos / HS_BLOCK);
        uint32_t off = (uint32_t)(pos % HS_BLOCK);
        hs_slot *s = find_slot(hs, blk);
        if (s && s->state == SLOT_LOADING) {
            hs->reader_waiting = 1;
            pthread_cond_wait(&hs->cond, &hs->lock);
            hs->reader_waiting = 0;
            continue;
        }
        if (!s) {
            s = evict_slot(hs, blk, blk + HS_PREFETCH);
            if (!s) {
                pthread_cond_wait(&hs->cond, &hs->lock);
                continue;
            }
            s->block = blk;
            s->state = SLOT_LOADING;
            pthread_mutex_unlock(&hs->lock);
            pthread_mutex_lock(&hs->reader_lock);
            int64_t n = fetch_block(hs, &hs->reader, blk, s->data);
            pthread_mutex_unlock(&hs->reader_lock);
            pthread_mutex_lock(&hs->lock);
            if (n < 0) {
                s->state = SLOT_EMPTY;
                pthread_cond_broadcast(&hs->cond);
                total = total ? total : -1;
                break;
            }
            s->len = (uint32_t)n;
            s->state = SLOT_READY;
            pthread_cond_broadcast(&hs->cond);
        }
        if (off >= s->len)
            break;
        uint32_t chunk = s->len - off;
        if (chunk > len)
            chunk = len;
        memcpy(buf, s->data + off, chunk);
        s->last_used = ++hs->use_counter;
        buf += chunk;
        pos += chunk;
        len -= chunk;
        total += (int)chunk;
        last_blk = blk;
    }
    if (last_blk >= 0 && hs->prefetch_started) {
        hs->pf_from = last_blk + 1;
        hs->pf_to = last_blk + HS_PREFETCH;
        pthread_cond_broadcast(&hs->cond);
    }
    pthread_mutex_unlock(&hs->lock);
    return total;
}

void http_source_abort(http_source *hs)
{
    pthread_mutex_lock(&hs->lock);
    hs->aborted = 1;
    pthread_cond_broadcast(&hs->cond);
    pthread_mutex_unlock(&hs->lock);
}

void http_source_close(http_source *hs)
{
    if (!hs)
        return;
    pthread_mutex_lock(&hs->lock);
    hs->aborted = 1;
    hs->closing = 1;
    pthread_cond_broadcast(&hs->cond);
    pthread_mutex_unlock(&hs->lock);
    if (hs->prefetch_started)
        pthread_join(hs->prefetch_thread, NULL);
    hs_free(hs);
}

void http_source_debug(http_source *hs, char *out, size_t cap)
{
    static const char states[] = { '.', 'L', 'R' };
    char slots[HS_SLOTS * 12 + 1];
    size_t o = 0;
    /* Read without the lock: this runs when something may be holding it. */
    for (int i = 0; i < HS_SLOTS && o + 12 < sizeof(slots); i++) {
        if (hs->slots[i].state == SLOT_EMPTY)
            continue;
        o += (size_t)snprintf(slots + o, sizeof(slots) - o, "%lld%c ", (long long)hs->slots[i].block,
                              states[hs->slots[i].state]);
    }
    slots[o] = '\0';
    snprintf(out, cap,
             "size=%lld ranges=%d aborted=%d window=[%lld,%lld] reader: phase=%s waiting=%d "
             "prefetcher: phase=%s waiting=%d block=%lld started=%d slots: %s",
             (long long)hs->size, hs->ranges_ok, (int)hs->aborted, (long long)hs->pf_from,
             (long long)hs->pf_to, g_phase_names[hs->reader.phase], (int)hs->reader_waiting,
             g_phase_names[hs->prefetcher.phase], (int)hs->prefetch_waiting,
             (long long)hs->prefetch_block, hs->prefetch_started, slots);
}
