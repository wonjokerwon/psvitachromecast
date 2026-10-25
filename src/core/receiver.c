#include "receiver.h"

#include <math.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../platform/platform.h"
#include "castmsg.h"
#include "json.h"
#include "log.h"
#include "mdns.h"
#include "net.h"
#include "sb.h"
#include "tls.h"

#define MAX_CLIENTS 12
#define MAX_VCONNS 16
#define MAX_QUEUE_BYTES (1024 * 1024)
#define PING_IDLE_MS 5000
#define CLIENT_TIMEOUT_MS 60000
#define LOAD_TIMEOUT_MS 45000
#define THREAD_STACK (256 * 1024)

#define RECEIVER_ID "receiver-0"
#define PLATFORM_ID "Tr@n$p0rt-0"
#define DEFAULT_APP_ID "CC1AD845"
#define BACKDROP_APP_ID "E8C28D3C"
#define MEDIA_COMMANDS 15 /* PAUSE | SEEK | STREAM_VOLUME | STREAM_MUTE */

typedef struct vconn {
    char src[128];
    char dst[128];
} vconn;

typedef struct outmsg {
    struct outmsg *next;
    size_t len;
    uint8_t data[];
} outmsg;

typedef struct client {
    struct client *next;
    int id;
    int fd;
    tstream ts;
    char peer[16];
    pthread_mutex_t qlock;
    outmsg *qhead, *qtail;
    size_t qbytes;
    vconn vc[MAX_VCONNS];
    int nvc;
    atomic_int dead;
    uint64_t last_rx;
    uint64_t last_ping;
} client;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static receiver_opts g_opts;
static vc_config g_cfg;
static char g_ip[16];
static char g_model[64];
static atomic_int g_running;
static uint64_t g_start_ms;

static client *g_clients;
static int g_client_count;
static int g_next_client_id;
static int g_listen_fd = -1;
static pthread_t g_accept_thread;
static int g_http_fd = -1, g_https_fd = -1;
static pthread_t g_http_thread;
static int g_http_started;

/* application session */
static int g_app_running;
static char g_app_id[32];
static char g_app_name[64];
static char g_session_id[40];
static char g_transport_id[48];
static int g_transport_counter;

/* device volume */
static double g_volume = 1.0;
static int g_muted;

/* media session */
static int g_media_session_id;
static int g_media_counter;
static json *g_media;
static unsigned g_player_gen;
static char g_title[128], g_subtitle[128];
static player_status g_last;
static int g_last_valid;
static uint64_t g_last_tick;

static int g_pending;
static int g_pending_req;
static int g_pending_client;
static char g_pending_sender[128];
static uint64_t g_pending_deadline;

/* Apps that need their own (non media namespace) protocol: reporting them
 * as available would only make the sender hang. */
static const char *const g_unsupported_apps[] = {
    "233637DE", /* YouTube */
    "CA5E8412", /* Netflix */
    "CC32E753", /* Spotify */
    NULL,
};

/* ------------------------------------------------------------------ */
/* sending                                                             */

static void client_enqueue(client *c, sb_t *framed)
{
    if (framed->oom || framed->len == 0)
        return;
    outmsg *m = malloc(sizeof(*m) + framed->len);
    if (!m)
        return;
    m->next = NULL;
    m->len = framed->len;
    memcpy(m->data, framed->buf, framed->len);
    pthread_mutex_lock(&c->qlock);
    if (c->qbytes + m->len > MAX_QUEUE_BYTES) {
        c->dead = 1; /* sender is not reading; drop it */
        free(m);
    } else {
        if (c->qtail)
            c->qtail->next = m;
        else
            c->qhead = m;
        c->qtail = m;
        c->qbytes += m->len;
    }
    pthread_mutex_unlock(&c->qlock);
}

static void client_send(client *c, const char *src, const char *dst, const char *ns,
                        const char *payload)
{
    sb_t sb;
    sb_init(&sb);
    if (cast_msg_encode_framed(&sb, src, dst, ns, CAST_PAYLOAD_STRING, payload,
                               strlen(payload)) == 0)
        client_enqueue(c, &sb);
    sb_free(&sb);
}

static void client_send_binary(client *c, const char *src, const char *dst, const char *ns,
                               const void *payload, size_t len)
{
    sb_t sb;
    sb_init(&sb);
    if (cast_msg_encode_framed(&sb, src, dst, ns, CAST_PAYLOAD_BINARY, payload, len) == 0)
        client_enqueue(c, &sb);
    sb_free(&sb);
}

static int client_flush(client *c)
{
    for (;;) {
        pthread_mutex_lock(&c->qlock);
        outmsg *m = c->qhead;
        if (m) {
            c->qhead = m->next;
            if (!c->qhead)
                c->qtail = NULL;
            c->qbytes -= m->len;
        }
        pthread_mutex_unlock(&c->qlock);
        if (!m)
            return 0;
        int rc = tstream_write(&c->ts, m->data, m->len);
        free(m);
        if (rc != 0)
            return -1;
    }
}

/* Sends payload from `src` to every virtual connection addressed to `src`,
 * skipping (skip_c, skip_dst), which already got a direct reply. */
static void broadcast(const char *src, const char *ns, const char *payload, client *skip_c,
                      const char *skip_dst)
{
    for (client *c = g_clients; c; c = c->next) {
        if (c->dead)
            continue;
        for (int i = 0; i < c->nvc; i++) {
            if (strcmp(c->vc[i].dst, src) != 0)
                continue;
            if (c == skip_c && skip_dst && strcmp(c->vc[i].src, skip_dst) == 0)
                continue;
            client_send(c, src, c->vc[i].src, ns, payload);
        }
    }
}

static client *find_client(int id)
{
    for (client *c = g_clients; c; c = c->next)
        if (c->id == id && !c->dead)
            return c;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* status payloads                                                     */

static void put_volume(sb_t *sb)
{
    sb_puts(sb, "{\"controlType\":\"attenuation\",\"level\":");
    sb_json_num(sb, g_volume);
    sb_printf(sb, ",\"muted\":%s,\"stepInterval\":0.05}", g_muted ? "true" : "false");
}

static char *receiver_status_json(int request_id)
{
    sb_t sb;
    sb_init(&sb);
    sb_printf(&sb, "{\"type\":\"RECEIVER_STATUS\",\"requestId\":%d,\"status\":{", request_id);
    if (g_app_running) {
        char status_text[200];
        if (g_media_session_id && g_title[0])
            snprintf(status_text, sizeof(status_text), "Casting: %s", g_title);
        else
            snprintf(status_text, sizeof(status_text), "Ready To Cast");
        sb_puts(&sb, "\"applications\":[{\"appId\":");
        sb_json_str(&sb, g_app_id);
        sb_puts(&sb, ",\"appType\":\"WEB\",\"displayName\":");
        sb_json_str(&sb, g_app_name);
        sb_puts(&sb, ",\"iconUrl\":\"\",\"isIdleScreen\":false,\"launchedFromCloud\":false,"
                     "\"namespaces\":[{\"name\":\"" NS_MEDIA "\"}],\"sessionId\":");
        sb_json_str(&sb, g_session_id);
        sb_puts(&sb, ",\"statusText\":");
        sb_json_str(&sb, status_text);
        sb_puts(&sb, ",\"transportId\":");
        sb_json_str(&sb, g_transport_id);
        sb_puts(&sb, ",\"universalAppId\":");
        sb_json_str(&sb, g_app_id);
        sb_puts(&sb, "}],");
    }
    sb_puts(&sb, "\"isActiveInput\":true,\"isStandBy\":false,\"volume\":");
    put_volume(&sb);
    sb_puts(&sb, "}}");
    return sb.oom ? (sb_free(&sb), NULL) : sb.buf;
}

static const char *player_state_name(player_state s)
{
    switch (s) {
    case PLAYER_PLAYING: return "PLAYING";
    case PLAYER_PAUSED: return "PAUSED";
    case PLAYER_LOADING:
    case PLAYER_BUFFERING: return "BUFFERING";
    default: return "IDLE";
    }
}

static const char *idle_reason_name(player_idle_reason r)
{
    switch (r) {
    case PLAYER_IDLE_FINISHED: return "FINISHED";
    case PLAYER_IDLE_CANCELLED: return "CANCELLED";
    case PLAYER_IDLE_INTERRUPTED: return "INTERRUPTED";
    case PLAYER_IDLE_ERROR: return "ERROR";
    default: return NULL;
    }
}

static char *media_status_json(int request_id, const player_status *st)
{
    sb_t sb;
    sb_init(&sb);
    sb_printf(&sb, "{\"type\":\"MEDIA_STATUS\",\"requestId\":%d,\"status\":[", request_id);
    if (g_media_session_id) {
        sb_printf(&sb, "{\"mediaSessionId\":%d,\"playbackRate\":1,\"playerState\":\"%s\","
                       "\"currentTime\":",
                  g_media_session_id, player_state_name(st->state));
        sb_json_num(&sb, floor(st->current_time * 1000.0 + 0.5) / 1000.0);
        sb_printf(&sb, ",\"supportedMediaCommands\":%d,\"volume\":{\"level\":", MEDIA_COMMANDS);
        sb_json_num(&sb, g_volume);
        sb_printf(&sb, ",\"muted\":%s},\"currentItemId\":1,\"activeTrackIds\":[],"
                       "\"repeatMode\":\"REPEAT_OFF\"",
                  g_muted ? "true" : "false");
        if (st->state == PLAYER_IDLE) {
            const char *reason = idle_reason_name(st->idle_reason);
            if (reason)
                sb_printf(&sb, ",\"idleReason\":\"%s\"", reason);
        }
        sb_puts(&sb, ",\"media\":");
        int n = json_write_obj_open(&sb, g_media, "duration");
        if (!json_get(g_media, "streamType")) {
            /* Real receivers always report it; senders such as catt only
             * allow seeking in BUFFERED streams. */
            sb_puts(&sb, n++ ? ",\"streamType\":\"BUFFERED\"" : "\"streamType\":\"BUFFERED\"");
        }
        double dur = st->duration > 0 ? st->duration : json_num(json_get(g_media, "duration"), 0);
        if (dur > 0) {
            sb_puts(&sb, n ? ",\"duration\":" : "\"duration\":");
            sb_json_num(&sb, floor(dur * 1000.0 + 0.5) / 1000.0);
        }
        sb_puts(&sb, "}}");
    }
    sb_puts(&sb, "]}");
    return sb.oom ? (sb_free(&sb), NULL) : sb.buf;
}

static void send_receiver_status(client *c, const char *dst, int request_id)
{
    char *s = receiver_status_json(request_id);
    if (!s)
        return;
    if (c)
        client_send(c, RECEIVER_ID, dst, NS_RECEIVER, s);
    free(s);
    if (request_id || !c) {
        s = receiver_status_json(0);
        if (s)
            broadcast(RECEIVER_ID, NS_RECEIVER, s, c, dst);
        free(s);
    }
}

/* Replies to (c, dst) with request_id and pushes the update to everyone else. */
static void send_media_status(client *c, const char *dst, int request_id)
{
    player_status st;
    player_get_status(&st);
    char *s = media_status_json(request_id, &st);
    if (!s)
        return;
    if (c)
        client_send(c, g_transport_id[0] ? g_transport_id : RECEIVER_ID, dst, NS_MEDIA, s);
    free(s);
    if (g_transport_id[0]) {
        s = media_status_json(0, &st);
        if (s)
            broadcast(g_transport_id, NS_MEDIA, s, c, dst);
        free(s);
    }
    g_last = st;
    g_last_valid = 1;
    g_last_tick = plat_ticks_ms();
}

static void send_error(client *c, const char *src, const char *dst, const char *ns,
                       const char *type, int request_id, const char *reason)
{
    sb_t sb;
    sb_init(&sb);
    sb_printf(&sb, "{\"type\":\"%s\",\"requestId\":%d", type, request_id);
    if (reason) {
        sb_puts(&sb, ",\"reason\":");
        sb_json_str(&sb, reason);
    }
    sb_putc(&sb, '}');
    if (!sb.oom)
        client_send(c, src, dst, ns, sb.buf);
    sb_free(&sb);
}

/* Cast DetailedErrorCode values used in LOAD_FAILED. */
#define ERR_MEDIA_NETWORK 103
#define ERR_MEDIA_SRC_NOT_SUPPORTED 104
#define ERR_LOAD_FAILED 905

static void send_load_failed(client *c, const char *src, const char *dst, int request_id,
                             int detailed_code)
{
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"LOAD_FAILED\",\"requestId\":%d,\"itemId\":1,\"detailedErrorCode\":%d}",
             request_id, detailed_code);
    client_send(c, src, dst, NS_MEDIA, buf);
}

/* ------------------------------------------------------------------ */
/* application + media session management                              */

static void random_hex(char *out, size_t nbytes)
{
    unsigned char b[32];
    if (nbytes > sizeof(b))
        nbytes = sizeof(b);
    if (tls_random(b, nbytes) != 0)
        plat_random(b, nbytes);
    for (size_t i = 0; i < nbytes; i++)
        sprintf(out + i * 2, "%02x", b[i]);
}

static void new_session_id(char out[40])
{
    char h[33];
    random_hex(h, 16);
    snprintf(out, 40, "%.8s-%.4s-%.4s-%.4s-%.12s", h, h + 8, h + 12, h + 16, h + 20);
}

static void clear_media(void)
{
    json_free(g_media);
    g_media = NULL;
    g_media_session_id = 0;
    g_title[0] = g_subtitle[0] = '\0';
    g_pending = 0;
    g_last_valid = 0;
}

static void close_app_connections(void)
{
    for (client *c = g_clients; c; c = c->next) {
        for (int i = 0; i < c->nvc;) {
            if (strcmp(c->vc[i].dst, g_transport_id) == 0) {
                client_send(c, g_transport_id, c->vc[i].src, NS_CONNECTION, "{\"type\":\"CLOSE\"}");
                c->vc[i] = c->vc[--c->nvc];
            } else {
                i++;
            }
        }
    }
}

static void stop_app(void)
{
    if (!g_app_running)
        return;
    if (g_media_session_id) {
        player_stop();
        send_media_status(NULL, NULL, 0);
    }
    clear_media();
    close_app_connections();
    LOGI("App stopped: %s", g_app_name);
    g_app_running = 0;
    g_app_id[0] = g_session_id[0] = g_transport_id[0] = '\0';
}

static void start_app(const char *app_id)
{
    g_app_running = 1;
    snprintf(g_app_id, sizeof(g_app_id), "%s", app_id);
    if (strcasecmp(app_id, DEFAULT_APP_ID) == 0)
        snprintf(g_app_name, sizeof(g_app_name), "Default Media Receiver");
    else
        snprintf(g_app_name, sizeof(g_app_name), "VitaCast Media Receiver");
    new_session_id(g_session_id);
    snprintf(g_transport_id, sizeof(g_transport_id), "web-%d", ++g_transport_counter);
    LOGI("App launched: %s (%s)", g_app_name, g_app_id);
}

static int app_supported(const char *app_id)
{
    for (int i = 0; g_unsupported_apps[i]; i++)
        if (strcasecmp(app_id, g_unsupported_apps[i]) == 0)
            return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* namespace handlers                                                  */

static void handle_deviceauth(client *c, const cast_msg *m)
{
    auth_challenge ch;
    sb_t out;
    sb_init(&out);
    if (auth_parse_challenge(m->payload, m->payload_len, &ch) != 0 || !ch.present) {
        auth_build_error(&out, 0);
    } else {
        size_t cert_len;
        const uint8_t *cert = tls_cert_der(&cert_len);
        int hash = ch.hash_algorithm == AUTH_HASH_SHA256 ? AUTH_HASH_SHA256 : AUTH_HASH_SHA1;
        sb_t data;
        uint8_t sig[512];
        size_t sig_len = 0;
        sb_init(&data);
        sb_putn(&data, ch.sender_nonce, ch.sender_nonce_len);
        sb_putn(&data, cert, cert_len);
        if (data.oom || tls_sign(hash, (const uint8_t *)data.buf, data.len, sig, sizeof(sig),
                                 &sig_len) != 0) {
            auth_build_error(&out, 0);
        } else {
            auth_build_response(&out, sig, sig_len, cert, cert_len, AUTH_SIG_RSASSA_PKCS1V15,
                                hash, ch.sender_nonce, ch.sender_nonce_len);
        }
        sb_free(&data);
    }
    if (!out.oom)
        client_send_binary(c, m->destination_id, m->source_id, NS_DEVICEAUTH, out.buf, out.len);
    sb_free(&out);
}

static void handle_connection(client *c, const cast_msg *m, const char *type)
{
    if (strcmp(type, "CONNECT") == 0) {
        for (int i = 0; i < c->nvc; i++)
            if (strcmp(c->vc[i].src, m->source_id) == 0 && strcmp(c->vc[i].dst, m->destination_id) == 0)
                return;
        if (c->nvc >= MAX_VCONNS)
            return;
        snprintf(c->vc[c->nvc].src, sizeof(c->vc[0].src), "%s", m->source_id);
        snprintf(c->vc[c->nvc].dst, sizeof(c->vc[0].dst), "%s", m->destination_id);
        c->nvc++;
    } else if (strcmp(type, "CLOSE") == 0) {
        for (int i = 0; i < c->nvc; i++) {
            if (strcmp(c->vc[i].src, m->source_id) == 0 && strcmp(c->vc[i].dst, m->destination_id) == 0) {
                c->vc[i] = c->vc[--c->nvc];
                break;
            }
        }
    }
}

static void handle_receiver(client *c, const cast_msg *m, json *j, const char *type, int req)
{
    const char *src = m->source_id;

    if (strcmp(type, "GET_STATUS") == 0) {
        send_receiver_status(c, src, req);
    } else if (strcmp(type, "LAUNCH") == 0) {
        const char *app = json_str(json_get(j, "appId"), DEFAULT_APP_ID);
        if (strcasecmp(app, BACKDROP_APP_ID) == 0) {
            stop_app();
            send_receiver_status(c, src, req);
            return;
        }
        if (!app_supported(app)) {
            LOGI("LAUNCH %s rejected: app needs its own receiver", app);
            send_error(c, RECEIVER_ID, src, NS_RECEIVER, "LAUNCH_ERROR", req, "NOT_FOUND");
            return;
        }
        if (!g_app_running || strcasecmp(g_app_id, app) != 0) {
            stop_app();
            start_app(app);
        }
        send_receiver_status(c, src, req);
    } else if (strcmp(type, "STOP") == 0) {
        stop_app();
        send_receiver_status(c, src, req);
    } else if (strcmp(type, "SET_VOLUME") == 0) {
        json *v = json_get(j, "volume");
        json *level = json_get(v, "level");
        json *muted = json_get(v, "muted");
        if (json_is_number(level)) {
            g_volume = level->num < 0 ? 0 : level->num > 1 ? 1 : level->num;
        }
        if (muted)
            g_muted = json_bool(muted, g_muted);
        player_set_volume(g_volume, g_muted);
        send_receiver_status(c, src, req);
    } else if (strcmp(type, "GET_APP_AVAILABILITY") == 0) {
        sb_t sb;
        int n = 0;
        sb_init(&sb);
        sb_printf(&sb, "{\"type\":\"GET_APP_AVAILABILITY\",\"responseType\":\"GET_APP_AVAILABILITY\","
                       "\"requestId\":%d,\"availability\":{", req);
        json *ids = json_get(j, "appId");
        for (json *id = ids ? ids->child : NULL; id; id = id->next) {
            if (id->type != JSON_STRING)
                continue;
            if (n++)
                sb_putc(&sb, ',');
            sb_json_str(&sb, id->str);
            sb_puts(&sb, app_supported(id->str) ? ":\"APP_AVAILABLE\"" : ":\"APP_UNAVAILABLE\"");
        }
        sb_puts(&sb, "}}");
        if (!sb.oom)
            client_send(c, RECEIVER_ID, src, NS_RECEIVER, sb.buf);
        sb_free(&sb);
    } else {
        send_error(c, RECEIVER_ID, src, NS_RECEIVER, "INVALID_REQUEST", req, "INVALID_COMMAND");
    }
}

static int is_http_url(const char *s)
{
    return s && (strncasecmp(s, "http://", 7) == 0 || strncasecmp(s, "https://", 8) == 0);
}

static void handle_load(client *c, const cast_msg *m, json *j, json *media_parent, int req,
                        int autoplay, double start)
{
    const char *src = m->source_id;
    const char *reply_from = m->destination_id;
    json *media = json_get(media_parent, "media");
    const char *url = json_str(json_get(media, "contentUrl"), NULL);
    char err[160] = "";
    (void)j;

    if (!is_http_url(url))
        url = json_str(json_get(media, "contentId"), NULL);
    if (!media || !is_http_url(url)) {
        LOGI("LOAD rejected: no http(s) URL in contentId/contentUrl");
        send_load_failed(c, reply_from, src, req, ERR_MEDIA_SRC_NOT_SUPPORTED);
        return;
    }
    if (!g_app_running) {
        start_app(DEFAULT_APP_ID);
        send_receiver_status(NULL, NULL, 0);
    }
    if (g_pending) {
        client *pc = find_client(g_pending_client);
        if (pc)
            send_error(pc, g_transport_id, g_pending_sender, NS_MEDIA, "LOAD_CANCELLED",
                       g_pending_req, NULL);
        g_pending = 0;
    }

    json *meta = json_get(media, "metadata");
    const char *title = json_str(json_get(meta, "title"), NULL);
    const char *subtitle = json_str(json_get(meta, "subtitle"), NULL);
    if (!subtitle)
        subtitle = json_str(json_get(meta, "artist"), NULL);
    if (!subtitle)
        subtitle = json_str(json_get(meta, "seriesTitle"), NULL);
    if (!subtitle)
        subtitle = json_str(json_get(meta, "albumName"), NULL);

    player_media pm;
    memset(&pm, 0, sizeof(pm));
    pm.url = url;
    pm.content_type = json_str(json_get(media, "contentType"), "");
    pm.title = title ? title : url;
    pm.start_time = start > 0 ? start : 0;
    pm.duration_hint = json_num(json_get(media, "duration"), 0);
    pm.autoplay = autoplay;

    LOGI("LOAD %s (%s)", pm.title, pm.content_type);
    unsigned gen = player_load(&pm, err, (int)sizeof(err));
    if (!gen) {
        LOGI("LOAD failed: %s", err);
        player_stop();
        clear_media();
        send_load_failed(c, reply_from, src, req, ERR_MEDIA_SRC_NOT_SUPPORTED);
        return;
    }

    clear_media();
    snprintf(g_title, sizeof(g_title), "%s", pm.title);
    snprintf(g_subtitle, sizeof(g_subtitle), "%s", subtitle ? subtitle : "");
    g_media = json_detach(media_parent, "media");
    if (json_str(json_get(g_media, "streamType"), NULL) == NULL)
        json_free(json_detach(g_media, "streamType")); /* e.g. null: report BUFFERED */
    g_media_session_id = ++g_media_counter;
    g_player_gen = gen;
    g_pending = 1;
    g_pending_req = req;
    g_pending_client = c->id;
    snprintf(g_pending_sender, sizeof(g_pending_sender), "%s", src);
    g_pending_deadline = plat_ticks_ms() + LOAD_TIMEOUT_MS;
    /* Tell everyone we are buffering; the LOAD reply follows once the
     * player is ready (see receiver_tick). */
    send_media_status(NULL, NULL, 0);
    send_receiver_status(NULL, NULL, 0);
}

static void handle_media(client *c, const cast_msg *m, json *j, const char *type, int req)
{
    const char *src = m->source_id;
    const char *from = m->destination_id;

    if (strcmp(type, "GET_STATUS") == 0) {
        player_status st;
        player_get_status(&st);
        char *s = media_status_json(req, &st);
        if (s)
            client_send(c, from, src, NS_MEDIA, s);
        free(s);
        return;
    }
    if (strcmp(type, "LOAD") == 0) {
        json *ap = json_get(j, "autoplay");
        handle_load(c, m, j, j, req, ap ? json_bool(ap, 1) : 1,
                    json_num(json_get(j, "currentTime"), 0));
        return;
    }
    if (strcmp(type, "QUEUE_LOAD") == 0) {
        json *items = json_get(j, "items");
        int idx = (int)json_num(json_get(j, "startIndex"), 0);
        json *item = items ? items->child : NULL;
        for (int i = 0; item && i < idx; i++)
            item = item->next;
        if (!item)
            item = items ? items->child : NULL;
        if (!item) {
            send_error(c, from, src, NS_MEDIA, "INVALID_REQUEST", req, "INVALID_PARAMS");
            return;
        }
        json *ap = json_get(item, "autoplay");
        handle_load(c, m, j, item, req, ap ? json_bool(ap, 1) : 1,
                    json_num(json_get(item, "startTime"), json_num(json_get(j, "currentTime"), 0)));
        return;
    }

    json *msid = json_get(j, "mediaSessionId");
    if (!g_media_session_id ||
        (json_is_number(msid) && (int)msid->num != g_media_session_id)) {
        send_error(c, from, src, NS_MEDIA, "INVALID_REQUEST", req, "INVALID_MEDIA_SESSION_ID");
        return;
    }

    if (strcmp(type, "PLAY") == 0) {
        player_play();
    } else if (strcmp(type, "PAUSE") == 0) {
        player_pause();
    } else if (strcmp(type, "STOP") == 0) {
        player_stop();
        g_pending = 0;
    } else if (strcmp(type, "SEEK") == 0) {
        player_status st;
        player_get_status(&st);
        double t = st.current_time;
        json *ct = json_get(j, "currentTime");
        json *rel = json_get(j, "relativeTime");
        if (json_is_number(ct))
            t = ct->num;
        else if (json_is_number(rel))
            t += rel->num;
        if (t < 0)
            t = 0;
        if (st.duration > 0 && t > st.duration)
            t = st.duration;
        player_seek(t);
        const char *resume = json_str(json_get(j, "resumeState"), NULL);
        if (resume && strcmp(resume, "PLAYBACK_START") == 0)
            player_play();
        else if (resume && strcmp(resume, "PLAYBACK_PAUSE") == 0)
            player_pause();
    } else if (strcmp(type, "SET_VOLUME") == 0) {
        json *v = json_get(j, "volume");
        json *level = json_get(v, "level");
        json *muted = json_get(v, "muted");
        if (json_is_number(level))
            g_volume = level->num < 0 ? 0 : level->num > 1 ? 1 : level->num;
        if (muted)
            g_muted = json_bool(muted, g_muted);
        player_set_volume(g_volume, g_muted);
        send_receiver_status(NULL, NULL, 0);
    } else {
        send_error(c, from, src, NS_MEDIA, "INVALID_REQUEST", req, "INVALID_COMMAND");
        return;
    }
    send_media_status(c, src, req);
}

static void handle_message(client *c, cast_msg *m)
{
    if (strcmp(m->ns, NS_DEVICEAUTH) == 0) {
        LOGI("Device auth challenge from %s", c->peer);
        handle_deviceauth(c, m);
        return;
    }
    if (m->payload_type != CAST_PAYLOAD_STRING)
        return;
    json *j = json_parse((const char *)m->payload, m->payload_len);
    if (!j)
        return;
    const char *type = json_str(json_get(j, "type"), "");
    int req = (int)json_num(json_get(j, "requestId"), 0);

    if (strcmp(m->ns, NS_HEARTBEAT) == 0) {
        if (strcmp(type, "PING") == 0)
            client_send(c, m->destination_id, m->source_id, NS_HEARTBEAT, "{\"type\":\"PONG\"}");
    } else if (strcmp(m->ns, NS_CONNECTION) == 0) {
        handle_connection(c, m, type);
    } else if (strcmp(m->ns, NS_RECEIVER) == 0) {
        LOGI("%s: receiver %s", c->peer, type);
        handle_receiver(c, m, j, type, req);
    } else if (strcmp(m->ns, NS_MEDIA) == 0) {
        if (strcmp(type, "GET_STATUS") != 0)
            LOGI("%s: media %s", c->peer, type);
        handle_media(c, m, j, type, req);
    }
    json_free(j);
}

/* ------------------------------------------------------------------ */
/* client connections                                                  */

static void client_free(client *c)
{
    outmsg *m = c->qhead;
    while (m) {
        outmsg *n = m->next;
        free(m);
        m = n;
    }
    pthread_mutex_destroy(&c->qlock);
    free(c);
}

static void *client_main(void *arg)
{
    client *c = arg;
    sb_t in;
    uint8_t tmp[8192];

    sb_init(&in);
    if (tstream_accept(&c->ts, c->fd, 1) != 0) {
        c->dead = 1;
        c->ts.fd = -1;
    } else {
        LOGI("Sender connected: %s", c->peer);
    }
    c->last_rx = plat_ticks_ms();

    while (g_running && !c->dead) {
        if (client_flush(c) != 0)
            break;
        int n = tstream_read(&c->ts, tmp, sizeof(tmp), 100);
        uint64_t now = plat_ticks_ms();
        if (n == NET_TIMEOUT) {
            if (now - c->last_rx > CLIENT_TIMEOUT_MS) {
                LOGI("Sender %s timed out", c->peer);
                break;
            }
            if (now - c->last_rx > PING_IDLE_MS && now - c->last_ping > PING_IDLE_MS) {
                c->last_ping = now;
                client_send(c, PLATFORM_ID, PLATFORM_ID, NS_HEARTBEAT, "{\"type\":\"PING\"}");
            }
            continue;
        }
        if (n <= 0)
            break;
        c->last_rx = now;
        sb_putn(&in, tmp, (size_t)n);
        while (in.len >= 4) {
            const uint8_t *p = (const uint8_t *)in.buf;
            uint32_t len = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
            if (len > CAST_MAX_MESSAGE) {
                c->dead = 1;
                break;
            }
            if (in.len < 4 + (size_t)len)
                break;
            cast_msg m;
            if (cast_msg_decode(p + 4, len, &m) == 0) {
                pthread_mutex_lock(&g_lock);
                handle_message(c, &m);
                pthread_mutex_unlock(&g_lock);
                cast_msg_free(&m);
            }
            sb_consume(&in, 4 + (size_t)len);
        }
        if (in.oom)
            break;
    }
    client_flush(c);
    sb_free(&in);

    pthread_mutex_lock(&g_lock);
    for (client **pp = &g_clients; *pp; pp = &(*pp)->next) {
        if (*pp == c) {
            *pp = c->next;
            break;
        }
    }
    g_client_count--;
    pthread_mutex_unlock(&g_lock);

    if (c->ts.fd >= 0)
        tstream_close(&c->ts);
    LOGI("Sender disconnected: %s", c->peer);
    client_free(c);
    return NULL;
}

static int spawn_thread(pthread_t *t, void *(*fn)(void *), void *arg, int detached)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, THREAD_STACK);
    if (detached)
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(t, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    return rc;
}

static void *accept_main(void *arg)
{
    (void)arg;
    while (g_running) {
        if (net_wait_readable(g_listen_fd, 500) <= 0)
            continue;
        char peer[16] = "";
        int fd = net_accept(g_listen_fd, peer);
        if (fd < 0)
            continue;
        pthread_mutex_lock(&g_lock);
        if (g_client_count >= MAX_CLIENTS) {
            pthread_mutex_unlock(&g_lock);
            net_close(fd);
            continue;
        }
        client *c = calloc(1, sizeof(*c));
        if (!c) {
            pthread_mutex_unlock(&g_lock);
            net_close(fd);
            continue;
        }
        c->id = ++g_next_client_id;
        c->fd = fd;
        c->ts.fd = fd;
        snprintf(c->peer, sizeof(c->peer), "%s", peer);
        pthread_mutex_init(&c->qlock, NULL);
        c->next = g_clients;
        g_clients = c;
        g_client_count++;
        pthread_t t;
        if (spawn_thread(&t, client_main, c, 1) != 0) {
            g_clients = c->next;
            g_client_count--;
            pthread_mutex_unlock(&g_lock);
            net_close(fd);
            client_free(c);
            continue;
        }
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* HTTP device info (/setup/eureka_info), used by senders to identify us */

static char *eureka_json(void)
{
    sb_t sb;
    sb_init(&sb);
    double uptime = (double)(plat_ticks_ms() - g_start_ms) / 1000.0;
    sb_puts(&sb, "{\"name\":");
    sb_json_str(&sb, g_cfg.name);
    sb_puts(&sb, ",\"version\":12,\"build_version\":\"1.0\",\"cast_build_revision\":\"1.0.0\","
                 "\"connected\":true,\"ethernet_connected\":false,\"has_update\":false,"
                 "\"ip_address\":");
    sb_json_str(&sb, g_ip);
    sb_puts(&sb, ",\"locale\":\"en-US\",\"setup_state\":60,\"ssdp_udn\":");
    sb_json_str(&sb, g_cfg.uuid);
    sb_puts(&sb, ",\"uptime\":");
    sb_json_num(&sb, floor(uptime));
    sb_puts(&sb, ",\"device_info\":{\"manufacturer\":\"VitaCast\",\"model_name\":");
    sb_json_str(&sb, g_model);
    sb_puts(&sb, ",\"product_name\":\"VitaCast\",\"ssdp_udn\":");
    sb_json_str(&sb, g_cfg.uuid);
    sb_puts(&sb, ",\"cloud_device_id\":");
    sb_json_str(&sb, g_cfg.uuid_hex);
    sb_puts(&sb, ",\"capabilities\":{\"display_supported\":true,\"audio_hdr_supported\":false,"
                 "\"multizone_supported\":false,\"stats_supported\":false}},"
                 "\"multizone\":{\"groups\":[]}}");
    return sb.oom ? (sb_free(&sb), NULL) : sb.buf;
}

static void http_serve_one(int fd, int use_tls)
{
    tstream ts;
    char req[2048];
    size_t len = 0;

    if (tstream_accept(&ts, fd, use_tls) != 0)
        return;
    while (len < sizeof(req) - 1) {
        int n = tstream_read(&ts, req + len, sizeof(req) - 1 - len, 3000);
        if (n <= 0)
            break;
        len += (size_t)n;
        req[len] = '\0';
        if (strstr(req, "\r\n\r\n"))
            break;
    }
    req[len] = '\0';

    char method[8] = "", path[256] = "";
    sscanf(req, "%7s %255s", method, path);
    sb_t out;
    sb_init(&out);
    if (strncmp(path, "/setup/eureka_info", 18) == 0) {
        char *body = eureka_json();
        if (body) {
            sb_printf(&out, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                            "Content-Length: %u\r\nConnection: close\r\n"
                            "Access-Control-Allow-Origin: *\r\n\r\n",
                      (unsigned)strlen(body));
            if (strcmp(method, "HEAD") != 0)
                sb_puts(&out, body);
            free(body);
        }
    } else {
        sb_puts(&out, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    }
    if (!out.oom && out.len)
        tstream_write(&ts, out.buf, out.len);
    sb_free(&out);
    tstream_close(&ts);
}

static void *http_main(void *arg)
{
    (void)arg;
    while (g_running) {
        int fds[2] = { g_http_fd, g_https_fd };
        int served = 0;
        for (int i = 0; i < 2; i++) {
            if (fds[i] < 0)
                continue;
            if (net_wait_readable(fds[i], 0) > 0) {
                int fd = net_accept(fds[i], NULL);
                if (fd >= 0) {
                    http_serve_one(fd, i == 1);
                    served = 1;
                }
            }
        }
        if (!served)
            plat_sleep_ms(50);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */

int receiver_start(const receiver_opts *opts)
{
    g_opts = *opts;
    g_cfg = *opts->cfg;
    snprintf(g_ip, sizeof(g_ip), "%s", opts->ip);
    snprintf(g_model, sizeof(g_model), "%s", opts->model_name ? opts->model_name : "VitaCast");
    g_start_ms = plat_ticks_ms();

    g_listen_fd = net_tcp_listen(opts->cast_port);
    if (g_listen_fd < 0) {
        LOGI("Cannot listen on TCP %u", (unsigned)opts->cast_port);
        return -1;
    }
    g_running = 1;
    if (spawn_thread(&g_accept_thread, accept_main, NULL, 0) != 0) {
        g_running = 0;
        net_close(g_listen_fd);
        g_listen_fd = -1;
        return -1;
    }
    LOGI("Cast channel listening on %s:%u", g_ip, (unsigned)opts->cast_port);

    if (opts->http_port) {
        g_http_fd = net_tcp_listen(opts->http_port);
        if (g_http_fd < 0)
            LOGI("Cannot listen on TCP %u (device info)", (unsigned)opts->http_port);
    }
    if (opts->https_port) {
        g_https_fd = net_tcp_listen(opts->https_port);
        if (g_https_fd < 0)
            LOGI("Cannot listen on TCP %u (device info)", (unsigned)opts->https_port);
    }
    if ((g_http_fd >= 0 || g_https_fd >= 0) && spawn_thread(&g_http_thread, http_main, NULL, 0) == 0)
        g_http_started = 1;

    if (opts->enable_mdns) {
        mdns_info mi;
        memset(&mi, 0, sizeof(mi));
        snprintf(mi.instance, sizeof(mi.instance), "VitaCast-%s", g_cfg.uuid_hex);
        snprintf(mi.hostname, sizeof(mi.hostname), "%s", g_cfg.uuid);
        snprintf(mi.ip, sizeof(mi.ip), "%s", g_ip);
        mi.port = opts->cast_port;
        snprintf(mi.txt[mi.txt_count++], 128, "id=%s", g_cfg.uuid_hex);
        snprintf(mi.txt[mi.txt_count++], 128, "cd=%.32s", g_cfg.uuid_hex);
        snprintf(mi.txt[mi.txt_count++], 128, "rm=");
        snprintf(mi.txt[mi.txt_count++], 128, "ve=05");
        snprintf(mi.txt[mi.txt_count++], 128, "md=%s", g_model);
        snprintf(mi.txt[mi.txt_count++], 128, "ic=/setup/icon.png");
        snprintf(mi.txt[mi.txt_count++], 128, "fn=%s", g_cfg.name);
        snprintf(mi.txt[mi.txt_count++], 128, "ca=4101");
        snprintf(mi.txt[mi.txt_count++], 128, "st=0");
        snprintf(mi.txt[mi.txt_count++], 128, "bs=FA8F%.8s", g_cfg.uuid_hex);
        snprintf(mi.txt[mi.txt_count++], 128, "nf=1");
        snprintf(mi.txt[mi.txt_count++], 128, "rs=");
        mdns_start(&mi);
    }
    return 0;
}

void receiver_stop(void)
{
    if (!g_running)
        return;
    pthread_mutex_lock(&g_lock);
    stop_app();
    pthread_mutex_unlock(&g_lock);
    for (int i = 0; i < 5; i++) {
        /* Give client threads a moment to flush the CLOSE messages. */
        plat_sleep_ms(50);
    }
    g_running = 0;
    if (g_opts.enable_mdns)
        mdns_stop();
    pthread_join(g_accept_thread, NULL);
    if (g_http_started)
        pthread_join(g_http_thread, NULL);
    g_http_started = 0;
    net_close(g_listen_fd);
    net_close(g_http_fd);
    net_close(g_https_fd);
    g_listen_fd = g_http_fd = g_https_fd = -1;

    pthread_mutex_lock(&g_lock);
    for (client *c = g_clients; c; c = c->next) {
        c->dead = 1;
        tstream_abort(&c->ts);
    }
    pthread_mutex_unlock(&g_lock);
    for (int i = 0; i < 100; i++) {
        pthread_mutex_lock(&g_lock);
        int n = g_client_count;
        pthread_mutex_unlock(&g_lock);
        if (n == 0)
            break;
        plat_sleep_ms(20);
    }
}

void receiver_tick(void)
{
    player_status st;
    uint64_t now = plat_ticks_ms();

    pthread_mutex_lock(&g_lock);
    if (!g_media_session_id) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    player_get_status(&st);
    if (st.generation != g_player_gen) {
        pthread_mutex_unlock(&g_lock);
        return;
    }

    if (g_pending) {
        client *pc = find_client(g_pending_client);
        if (st.state == PLAYER_PLAYING || st.state == PLAYER_PAUSED || st.state == PLAYER_BUFFERING) {
            g_pending = 0;
            LOGI("Playback started: %s", g_title);
            send_media_status(pc, g_pending_sender, g_pending_req);
        } else if (st.state == PLAYER_IDLE || now > g_pending_deadline) {
            g_pending = 0;
            if (st.state != PLAYER_IDLE)
                player_stop();
            LOGI("Load failed: %s", st.error[0] ? st.error : "timeout");
            if (pc)
                send_load_failed(pc, g_transport_id, g_pending_sender, g_pending_req,
                                 st.state == PLAYER_IDLE ? ERR_MEDIA_NETWORK : ERR_LOAD_FAILED);
            send_media_status(NULL, NULL, 0);
        }
        pthread_mutex_unlock(&g_lock);
        return;
    }

    int changed = !g_last_valid || st.state != g_last.state || st.idle_reason != g_last.idle_reason;
    if (!changed && st.duration > 0 && fabs(st.duration - g_last.duration) > 0.5)
        changed = 1;
    if (!changed && g_last_valid) {
        double expected = g_last.current_time;
        if (g_last.state == PLAYER_PLAYING)
            expected += (double)(now - g_last_tick) / 1000.0;
        if (fabs(st.current_time - expected) > 3.0)
            changed = 1; /* seek, stall or local skip */
    }
    if (changed) {
        if (st.state == PLAYER_IDLE && st.idle_reason == PLAYER_IDLE_FINISHED)
            LOGI("Playback finished");
        else if (st.state == PLAYER_IDLE && st.idle_reason == PLAYER_IDLE_ERROR)
            LOGI("Playback error: %s", st.error);
        send_media_status(NULL, NULL, 0);
    }
    pthread_mutex_unlock(&g_lock);
}

void receiver_get_ui(receiver_ui *ui)
{
    memset(ui, 0, sizeof(*ui));
    pthread_mutex_lock(&g_lock);
    ui->senders = g_client_count;
    ui->app_running = g_app_running;
    snprintf(ui->app_name, sizeof(ui->app_name), "%s", g_app_name);
    ui->has_media = g_media_session_id != 0;
    snprintf(ui->title, sizeof(ui->title), "%s", g_title);
    snprintf(ui->subtitle, sizeof(ui->subtitle), "%s", g_subtitle);
    ui->volume = g_volume;
    ui->muted = g_muted;
    pthread_mutex_unlock(&g_lock);
    player_get_status(&ui->player);
}

void receiver_local_toggle_pause(void)
{
    player_status st;
    player_get_status(&st);
    if (st.state == PLAYER_PLAYING)
        player_pause();
    else if (st.state == PLAYER_PAUSED)
        player_play();
    receiver_tick();
}

void receiver_local_seek(double delta)
{
    player_status st;
    player_get_status(&st);
    if (st.state != PLAYER_PLAYING && st.state != PLAYER_PAUSED)
        return;
    double t = st.current_time + delta;
    if (t < 0)
        t = 0;
    if (st.duration > 0 && t > st.duration - 1)
        t = st.duration - 1;
    player_seek(t);
    pthread_mutex_lock(&g_lock);
    if (g_media_session_id)
        send_media_status(NULL, NULL, 0);
    pthread_mutex_unlock(&g_lock);
}

void receiver_local_stop(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_media_session_id) {
        player_stop();
        g_pending = 0;
        send_media_status(NULL, NULL, 0);
    }
    pthread_mutex_unlock(&g_lock);
}

void receiver_local_volume(double delta)
{
    pthread_mutex_lock(&g_lock);
    g_volume += delta;
    if (g_volume < 0)
        g_volume = 0;
    if (g_volume > 1)
        g_volume = 1;
    g_volume = floor(g_volume * 20.0 + 0.5) / 20.0;
    player_set_volume(g_volume, g_muted);
    send_receiver_status(NULL, NULL, 0);
    if (g_media_session_id)
        send_media_status(NULL, NULL, 0);
    pthread_mutex_unlock(&g_lock);
}
