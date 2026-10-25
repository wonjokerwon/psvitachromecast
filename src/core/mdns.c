#include "mdns.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../platform/platform.h"
#include "log.h"
#include "net.h"

#define MDNS_GROUP "224.0.0.251"
#define MDNS_PORT 5353

#define TYPE_A 1
#define TYPE_PTR 12
#define TYPE_TXT 16
#define TYPE_SRV 33
#define TYPE_ANY 255
#define CLASS_IN 1
#define CACHE_FLUSH 0x8000

#define SERVICE "_googlecast._tcp.local"
#define SERVICES_ENUM "_services._dns-sd._udp.local"

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    int overflow;
} pkt_writer;

static void w_bytes(pkt_writer *w, const void *d, size_t n)
{
    if (w->len + n > w->cap) {
        w->overflow = 1;
        return;
    }
    memcpy(w->buf + w->len, d, n);
    w->len += n;
}

static void w_u16(pkt_writer *w, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    w_bytes(w, b, 2);
}

static void w_u32(pkt_writer *w, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    w_bytes(w, b, 4);
}

/* Writes a dotted name as DNS labels (no compression). The first label may
 * contain dots when `first_label` is given separately. */
static void w_name2(pkt_writer *w, const char *first_label, const char *rest)
{
    if (first_label) {
        size_t n = strlen(first_label);
        if (n > 63)
            n = 63;
        uint8_t l = (uint8_t)n;
        w_bytes(w, &l, 1);
        w_bytes(w, first_label, n);
    }
    const char *p = rest;
    while (p && *p) {
        const char *dot = strchr(p, '.');
        size_t n = dot ? (size_t)(dot - p) : strlen(p);
        if (n > 63)
            n = 63;
        uint8_t l = (uint8_t)n;
        w_bytes(w, &l, 1);
        w_bytes(w, p, n);
        p = dot ? dot + 1 : NULL;
    }
    uint8_t z = 0;
    w_bytes(w, &z, 1);
}

static void w_rr_header(pkt_writer *w, const char *first, const char *rest, uint16_t type,
                        uint16_t cls, uint32_t ttl)
{
    w_name2(w, first, rest);
    w_u16(w, type);
    w_u16(w, cls);
    w_u32(w, ttl);
}

/* Reserves a 16 bit rdlength slot and returns its offset. */
static size_t w_rdlen_begin(pkt_writer *w)
{
    size_t at = w->len;
    w_u16(w, 0);
    return at;
}

static void w_rdlen_end(pkt_writer *w, size_t at)
{
    if (w->overflow)
        return;
    size_t n = w->len - at - 2;
    w->buf[at] = (uint8_t)(n >> 8);
    w->buf[at + 1] = (uint8_t)n;
}

size_t mdns_build_response(uint8_t *buf, size_t cap, const mdns_info *info, uint16_t id,
                           int ttl_scale, int services_only)
{
    pkt_writer w = { buf, cap, 0, 0 };
    uint32_t ttl_short = ttl_scale ? 120 : 0;
    uint32_t ttl_long = ttl_scale ? 4500 : 0;
    char host_local[128];
    size_t at;

    snprintf(host_local, sizeof(host_local), "%s.local", info->hostname);

    w_u16(&w, id);
    w_u16(&w, 0x8400);              /* response, authoritative */
    w_u16(&w, 0);                   /* questions */
    w_u16(&w, 1);                   /* answers */
    w_u16(&w, 0);                   /* authority */
    w_u16(&w, services_only ? 0 : 3); /* additional */

    if (services_only) {
        w_rr_header(&w, NULL, SERVICES_ENUM, TYPE_PTR, CLASS_IN, ttl_long);
        at = w_rdlen_begin(&w);
        w_name2(&w, NULL, SERVICE);
        w_rdlen_end(&w, at);
        return w.overflow ? 0 : w.len;
    }

    /* PTR _googlecast._tcp.local -> instance */
    w_rr_header(&w, NULL, SERVICE, TYPE_PTR, CLASS_IN, ttl_long);
    at = w_rdlen_begin(&w);
    w_name2(&w, info->instance, SERVICE);
    w_rdlen_end(&w, at);

    /* SRV instance -> host:port */
    w_rr_header(&w, info->instance, SERVICE, TYPE_SRV, CLASS_IN | CACHE_FLUSH, ttl_short);
    at = w_rdlen_begin(&w);
    w_u16(&w, 0);
    w_u16(&w, 0);
    w_u16(&w, info->port);
    w_name2(&w, NULL, host_local);
    w_rdlen_end(&w, at);

    /* TXT instance */
    w_rr_header(&w, info->instance, SERVICE, TYPE_TXT, CLASS_IN | CACHE_FLUSH, ttl_long);
    at = w_rdlen_begin(&w);
    for (int i = 0; i < info->txt_count; i++) {
        size_t n = strlen(info->txt[i]);
        if (n > 255)
            n = 255;
        uint8_t l = (uint8_t)n;
        w_bytes(&w, &l, 1);
        w_bytes(&w, info->txt[i], n);
    }
    w_rdlen_end(&w, at);

    /* A host -> ip */
    w_rr_header(&w, NULL, host_local, TYPE_A, CLASS_IN | CACHE_FLUSH, ttl_short);
    at = w_rdlen_begin(&w);
    {
        struct in_addr a;
        if (inet_pton(AF_INET, info->ip, &a) != 1)
            a.s_addr = 0;
        w_bytes(&w, &a.s_addr, 4);
    }
    w_rdlen_end(&w, at);

    return w.overflow ? 0 : w.len;
}

/* Reads a (possibly compressed) name at *off into out (lowercase, dotted). */
static int read_name(const uint8_t *pkt, size_t len, size_t *off, char *out, size_t outcap)
{
    size_t pos = *off, o = 0;
    int jumped = 0, hops = 0;

    for (;;) {
        if (pos >= len)
            return -1;
        uint8_t l = pkt[pos];
        if ((l & 0xC0) == 0xC0) {
            if (pos + 1 >= len || ++hops > 16)
                return -1;
            size_t ptr = ((size_t)(l & 0x3F) << 8) | pkt[pos + 1];
            if (!jumped)
                *off = pos + 2;
            jumped = 1;
            pos = ptr;
            continue;
        }
        if (l & 0xC0)
            return -1;
        pos++;
        if (l == 0)
            break;
        if (pos + l > len)
            return -1;
        if (o && o < outcap)
            out[o++] = '.';
        for (int i = 0; i < l && o < outcap - 1; i++)
            out[o++] = (char)tolower(pkt[pos + i]);
        pos += l;
    }
    if (!jumped)
        *off = pos;
    out[o < outcap ? o : outcap - 1] = '\0';
    return 0;
}

static void lower_copy(char *dst, size_t cap, const char *a, const char *b)
{
    snprintf(dst, cap, "%s%s", a, b);
    for (char *p = dst; *p; p++)
        *p = (char)tolower((unsigned char)*p);
}

int mdns_classify_query(const uint8_t *pkt, size_t len, const mdns_info *info, int *unicast,
                        uint16_t *id)
{
    char name[256], inst[256], host[256];
    int result = MDNS_Q_NONE;

    *unicast = 0;
    if (len < 12)
        return MDNS_Q_NONE;
    if (id)
        *id = (uint16_t)((pkt[0] << 8) | pkt[1]);
    uint16_t flags = (uint16_t)((pkt[2] << 8) | pkt[3]);
    if (flags & 0x8000)
        return MDNS_Q_NONE; /* a response, not a query */
    uint16_t qd = (uint16_t)((pkt[4] << 8) | pkt[5]);

    lower_copy(inst, sizeof(inst), info->instance, "." SERVICE);
    lower_copy(host, sizeof(host), info->hostname, ".local");

    size_t off = 12;
    for (unsigned i = 0; i < qd; i++) {
        if (read_name(pkt, len, &off, name, sizeof(name)) < 0 || off + 4 > len)
            break;
        uint16_t qtype = (uint16_t)((pkt[off] << 8) | pkt[off + 1]);
        uint16_t qclass = (uint16_t)((pkt[off + 2] << 8) | pkt[off + 3]);
        off += 4;
        if (qclass & 0x8000)
            *unicast = 1;
        if (strcmp(name, SERVICE) == 0 && (qtype == TYPE_PTR || qtype == TYPE_ANY))
            result = MDNS_Q_SERVICE;
        else if (strcmp(name, inst) == 0)
            result = MDNS_Q_SERVICE;
        else if (strcmp(name, host) == 0 && (qtype == TYPE_A || qtype == TYPE_ANY))
            result = MDNS_Q_SERVICE;
        else if (strcmp(name, SERVICES_ENUM) == 0 && result == MDNS_Q_NONE)
            result = MDNS_Q_SERVICES;
    }
    return result;
}

/* --- responder thread --- */

static pthread_t g_thread;
static int g_fd = -1;
static atomic_int g_running;
static mdns_info g_info;

static void send_full(uint16_t id, const char *ip, uint16_t port, int ttl_scale, int services)
{
    uint8_t buf[1500];
    size_t n = mdns_build_response(buf, sizeof(buf), &g_info, id, ttl_scale, services);
    if (n)
        net_udp_sendto(g_fd, buf, n, ip, port);
}

static void *mdns_thread(void *arg)
{
    uint8_t pkt[1500];
    char from[16];
    uint16_t from_port;
    uint64_t next_announce = 0;
    int announcements = 0;

    (void)arg;
    while (g_running) {
        uint64_t now = plat_ticks_ms();
        /* Announce on startup (RFC 6762 8.3), then refresh every 60s so
         * clients that missed the initial burst still find us quickly. */
        if (now >= next_announce) {
            send_full(0, MDNS_GROUP, MDNS_PORT, 1, 0);
            announcements++;
            next_announce = now + (announcements < 3 ? 1000u * announcements : 60000u);
        }
        int n = net_udp_recvfrom(g_fd, pkt, sizeof(pkt), 500, from, &from_port);
        if (n <= 0)
            continue;
        int unicast = 0;
        uint16_t id = 0;
        int q = mdns_classify_query(pkt, (size_t)n, &g_info, &unicast, &id);
        if (q == MDNS_Q_NONE)
            continue;
        if (from_port != MDNS_PORT) {
            /* Legacy unicast resolver: reply directly with the query id. */
            send_full(id, from, from_port, 1, q == MDNS_Q_SERVICES);
        } else if (unicast) {
            send_full(0, from, from_port, 1, q == MDNS_Q_SERVICES);
        } else {
            send_full(0, MDNS_GROUP, MDNS_PORT, 1, q == MDNS_Q_SERVICES);
        }
    }
    return NULL;
}

int mdns_start(const mdns_info *info)
{
    g_info = *info;
    g_fd = net_udp_multicast(MDNS_PORT, MDNS_GROUP, info->ip);
    if (g_fd < 0) {
        LOGI("mDNS: cannot bind UDP 5353 (discovery disabled)");
        return -1;
    }
    g_running = 1;
    if (pthread_create(&g_thread, NULL, mdns_thread, NULL) != 0) {
        g_running = 0;
        net_close(g_fd);
        g_fd = -1;
        return -1;
    }
    LOGI("mDNS: advertising %s on %s", info->instance, info->ip);
    return 0;
}

void mdns_stop(void)
{
    if (!g_running)
        return;
    g_running = 0;
    pthread_join(g_thread, NULL);
    send_full(0, MDNS_GROUP, MDNS_PORT, 0, 0); /* goodbye */
    net_close(g_fd);
    g_fd = -1;
}

/* ------------------------------------------------------------------ */
/* browsing                                                            */

size_t mdns_build_query(uint8_t *buf, size_t cap)
{
    pkt_writer w = { buf, cap, 0, 0 };
    w_u16(&w, 0);      /* id */
    w_u16(&w, 0);      /* standard query */
    w_u16(&w, 1);      /* one question */
    w_u16(&w, 0);
    w_u16(&w, 0);
    w_u16(&w, 0);
    w_name2(&w, NULL, SERVICE);
    w_u16(&w, TYPE_PTR);
    w_u16(&w, CLASS_IN);
    return w.overflow ? 0 : w.len;
}

static int ends_with(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

static mdns_seen_instance *seen_instance(mdns_collector *c, const char *name, const char *from_ip)
{
    for (int i = 0; i < c->ninst; i++)
        if (strcmp(c->inst[i].instance, name) == 0)
            return &c->inst[i];
    if (c->ninst >= MDNS_MAX_SEEN)
        return NULL;
    mdns_seen_instance *s = &c->inst[c->ninst++];
    memset(s, 0, sizeof(*s));
    snprintf(s->instance, sizeof(s->instance), "%s", name);
    snprintf(s->from_ip, sizeof(s->from_ip), "%s", from_ip ? from_ip : "");
    s->capabilities = -1;
    return s;
}

static void seen_host(mdns_collector *c, const char *host, const char *ip)
{
    for (int i = 0; i < c->nhost; i++) {
        if (strcmp(c->host[i].host, host) == 0) {
            snprintf(c->host[i].ip, sizeof(c->host[i].ip), "%s", ip);
            return;
        }
    }
    if (c->nhost >= MDNS_MAX_SEEN)
        return;
    snprintf(c->host[c->nhost].host, sizeof(c->host[0].host), "%s", host);
    snprintf(c->host[c->nhost].ip, sizeof(c->host[0].ip), "%s", ip);
    c->nhost++;
}

/* Copies TXT value text (case preserved) for key into out. */
static void txt_value(const uint8_t *p, size_t len, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    size_t i = 0;
    while (i < len) {
        size_t n = p[i++];
        if (i + n > len)
            return;
        if (n > klen && memcmp(p + i, key, klen) == 0 && p[i + klen] == '=') {
            size_t vlen = n - klen - 1;
            if (vlen >= cap)
                vlen = cap - 1;
            memcpy(out, p + i + klen + 1, vlen);
            out[vlen] = '\0';
            return;
        }
        i += n;
    }
}

void mdns_collect(mdns_collector *c, const uint8_t *pkt, size_t len, const char *from_ip)
{
    char owner[256], name[256];
    if (len < 12)
        return;
    uint16_t flags = (uint16_t)((pkt[2] << 8) | pkt[3]);
    if (!(flags & 0x8000))
        return; /* only responses */
    unsigned qd = (unsigned)((pkt[4] << 8) | pkt[5]);
    unsigned rr = (unsigned)((pkt[6] << 8) | pkt[7]) + (unsigned)((pkt[8] << 8) | pkt[9]) +
                  (unsigned)((pkt[10] << 8) | pkt[11]);
    size_t off = 12;
    for (unsigned i = 0; i < qd; i++) {
        if (read_name(pkt, len, &off, name, sizeof(name)) < 0 || off + 4 > len)
            return;
        off += 4;
    }
    for (unsigned i = 0; i < rr; i++) {
        if (read_name(pkt, len, &off, owner, sizeof(owner)) < 0 || off + 10 > len)
            return;
        uint16_t type = (uint16_t)((pkt[off] << 8) | pkt[off + 1]);
        uint16_t rdlen = (uint16_t)((pkt[off + 8] << 8) | pkt[off + 9]);
        off += 10;
        if (off + rdlen > len)
            return;
        size_t rd = off;
        off += rdlen;

        if (type == TYPE_PTR && strcmp(owner, SERVICE) == 0) {
            size_t p = rd;
            if (read_name(pkt, len, &p, name, sizeof(name)) == 0 && ends_with(name, "." SERVICE))
                seen_instance(c, name, from_ip);
        } else if (type == TYPE_SRV && ends_with(owner, "." SERVICE) && rdlen > 6) {
            mdns_seen_instance *s = seen_instance(c, owner, from_ip);
            size_t p = rd + 6;
            if (s && read_name(pkt, len, &p, name, sizeof(name)) == 0) {
                s->port = (uint16_t)((pkt[rd + 4] << 8) | pkt[rd + 5]);
                snprintf(s->target, sizeof(s->target), "%s", name);
            }
        } else if (type == TYPE_TXT && ends_with(owner, "." SERVICE)) {
            mdns_seen_instance *s = seen_instance(c, owner, from_ip);
            if (s) {
                char ca[16] = "";
                txt_value(pkt + rd, rdlen, "fn", s->name, sizeof(s->name));
                txt_value(pkt + rd, rdlen, "md", s->model, sizeof(s->model));
                txt_value(pkt + rd, rdlen, "id", s->id, sizeof(s->id));
                txt_value(pkt + rd, rdlen, "ca", ca, sizeof(ca));
                if (ca[0])
                    s->capabilities = atoi(ca);
            }
        } else if (type == TYPE_A && rdlen == 4) {
            char ip[16];
            snprintf(ip, sizeof(ip), "%u.%u.%u.%u", pkt[rd], pkt[rd + 1], pkt[rd + 2], pkt[rd + 3]);
            seen_host(c, owner, ip);
        }
    }
}

int mdns_collector_devices(const mdns_collector *c, cast_device *out, int max)
{
    int n = 0;
    for (int i = 0; i < c->ninst && n < max; i++) {
        const mdns_seen_instance *s = &c->inst[i];
        if (!s->port)
            continue;
        /* Bit 0 of "ca" is video out; skip speakers and audio groups. */
        if (s->capabilities >= 0 && !(s->capabilities & 1))
            continue;
        cast_device *d = &out[n];
        memset(d, 0, sizeof(*d));
        const char *ip = s->from_ip;
        for (int h = 0; h < c->nhost; h++)
            if (strcmp(c->host[h].host, s->target) == 0)
                ip = c->host[h].ip;
        if (!ip[0])
            continue;
        snprintf(d->ip, sizeof(d->ip), "%s", ip);
        d->port = s->port;
        snprintf(d->model, sizeof(d->model), "%s", s->model);
        snprintf(d->id, sizeof(d->id), "%s", s->id);
        if (s->name[0]) {
            snprintf(d->name, sizeof(d->name), "%s", s->name);
        } else {
            /* Fall back to the first label of the instance name. */
            size_t l = strcspn(s->instance, ".");
            snprintf(d->name, sizeof(d->name), "%.*s", (int)(l < 63 ? l : 63), s->instance);
        }
        n++;
    }
    return n;
}

int mdns_browse(const char *iface_ip, int timeout_ms, cast_device *out, int max)
{
    uint8_t q[512], pkt[1500];
    char from[16];
    mdns_collector *c = calloc(1, sizeof(*c));
    if (!c)
        return 0;
    int fd = net_udp_multicast(MDNS_PORT, MDNS_GROUP, iface_ip);
    if (fd < 0) {
        free(c);
        return -1;
    }
    size_t qlen = mdns_build_query(q, sizeof(q));
    uint64_t start = plat_ticks_ms(), next_query = start;
    int sent = 0;
    for (;;) {
        uint64_t now = plat_ticks_ms();
        if (now - start >= (uint64_t)timeout_ms)
            break;
        if (now >= next_query && sent < 3) {
            net_udp_sendto(fd, q, qlen, MDNS_GROUP, MDNS_PORT);
            sent++;
            next_query = now + 700;
        }
        int n = net_udp_recvfrom(fd, pkt, sizeof(pkt), 100, from, NULL);
        if (n > 0)
            mdns_collect(c, pkt, (size_t)n, from);
    }
    net_close(fd);
    int count = mdns_collector_devices(c, out, max);
    free(c);
    return count;
}
