#include "cast_sender.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../platform/platform.h"
#include "castmsg.h"
#include "json.h"
#include "log.h"
#include "net.h"
#include "sb.h"
#include "tls.h"

#define SENDER_ID "sender-vitacast"
#define RECEIVER_ID "receiver-0"
#define CONNECT_TIMEOUT_MS 8000
#define REPLY_TIMEOUT_MS 20000

typedef struct session {
    tstream ts;
    sb_t in;
    int next_request;
} session;

static int send_json(session *s, const char *dst, const char *ns, const char *payload)
{
    sb_t out;
    sb_init(&out);
    int rc = cast_msg_encode_framed(&out, SENDER_ID, dst, ns, CAST_PAYLOAD_STRING, payload, strlen(payload));
    if (rc == 0)
        rc = tstream_write(&s->ts, out.buf, out.len);
    sb_free(&out);
    return rc;
}

/* Reads one message (answering heartbeats). 1 = got one, 0 = timeout, -1 = error. */
static int read_message(session *s, cast_msg *m, int timeout_ms)
{
    uint8_t tmp[4096];
    uint64_t end = plat_ticks_ms() + (uint64_t)timeout_ms;
    for (;;) {
        while (s->in.len >= 4) {
            const uint8_t *p = (const uint8_t *)s->in.buf;
            uint32_t len = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
            if (len > CAST_MAX_MESSAGE)
                return -1;
            if (s->in.len < 4 + (size_t)len)
                break;
            int rc = cast_msg_decode(p + 4, len, m);
            sb_consume(&s->in, 4 + (size_t)len);
            if (rc != 0)
                continue;
            if (strcmp(m->ns, NS_HEARTBEAT) == 0) {
                if (strstr((const char *)m->payload, "\"PING\""))
                    send_json(s, m->source_id, NS_HEARTBEAT, "{\"type\":\"PONG\"}");
                cast_msg_free(m);
                continue;
            }
            return 1;
        }
        uint64_t now = plat_ticks_ms();
        if (now >= end)
            return 0;
        int n = tstream_read(&s->ts, tmp, sizeof(tmp), (int)(end - now));
        if (n == NET_TIMEOUT)
            continue;
        if (n <= 0)
            return -1;
        sb_putn(&s->in, tmp, (size_t)n);
    }
}

/* Waits for a receiver-namespace reply of the given type. */
static json *wait_receiver(session *s, const char *type, int request_id, char *err, size_t errcap)
{
    uint64_t end = plat_ticks_ms() + REPLY_TIMEOUT_MS;
    for (;;) {
        cast_msg m;
        uint64_t now = plat_ticks_ms();
        int rc = now < end ? read_message(s, &m, (int)(end - now)) : 0;
        if (rc <= 0) {
            snprintf(err, errcap, rc == 0 ? "the TV did not answer" : "connection to the TV was lost");
            return NULL;
        }
        json *j = NULL;
        if (strcmp(m.ns, NS_RECEIVER) == 0)
            j = json_parse((const char *)m.payload, m.payload_len);
        cast_msg_free(&m);
        if (!j)
            continue;
        const char *t = json_str(json_get(j, "type"), "");
        int rid = (int)json_num(json_get(j, "requestId"), 0);
        if (strcmp(t, "LAUNCH_ERROR") == 0 && rid == request_id) {
            snprintf(err, errcap, "the TV refused to start the viewer (%s)",
                     json_str(json_get(j, "reason"), "unknown reason"));
            json_free(j);
            return NULL;
        }
        if (strcmp(t, type) == 0 && (rid == request_id || rid == 0))
            return j;
        json_free(j);
    }
}

/* First running application in a RECEIVER_STATUS, or NULL. */
static json *first_app(json *status)
{
    json *apps = json_get(json_get(status, "status"), "applications");
    return apps && apps->type == JSON_ARRAY ? apps->child : NULL;
}

static int open_session(session *s, const char *ip, uint16_t port, char *err, size_t errcap)
{
    memset(s, 0, sizeof(*s));
    sb_init(&s->in);
    s->next_request = 1;
    if (tstream_connect(&s->ts, ip, port, 1, CONNECT_TIMEOUT_MS) != 0) {
        snprintf(err, errcap, "cannot connect to the TV at %s", ip);
        return -1;
    }
    if (send_json(s, RECEIVER_ID, NS_CONNECTION,
                  "{\"type\":\"CONNECT\",\"origin\":{},\"userAgent\":\"VitaCast\"}") != 0) {
        snprintf(err, errcap, "connection to the TV was lost");
        tstream_close(&s->ts);
        return -1;
    }
    return 0;
}

static void close_session(session *s)
{
    send_json(s, RECEIVER_ID, NS_CONNECTION, "{\"type\":\"CLOSE\"}");
    tstream_close(&s->ts);
    sb_free(&s->in);
}

static json *get_status(session *s, char *err, size_t errcap)
{
    char buf[96];
    int id = s->next_request++;
    snprintf(buf, sizeof(buf), "{\"type\":\"GET_STATUS\",\"requestId\":%d}", id);
    if (send_json(s, RECEIVER_ID, NS_RECEIVER, buf) != 0) {
        snprintf(err, errcap, "connection to the TV was lost");
        return NULL;
    }
    return wait_receiver(s, "RECEIVER_STATUS", id, err, errcap);
}

static int stop_running(session *s, json *status, char *err, size_t errcap)
{
    json *app = first_app(status);
    const char *sid = json_str(json_get(app, "sessionId"), NULL);
    if (!app || !sid || json_bool(json_get(app, "isIdleScreen"), 0))
        return 0;
    sb_t sb;
    sb_init(&sb);
    int id = s->next_request++;
    sb_printf(&sb, "{\"type\":\"STOP\",\"requestId\":%d,\"sessionId\":", id);
    sb_json_str(&sb, sid);
    sb_putc(&sb, '}');
    int rc = sb.oom ? -1 : send_json(s, RECEIVER_ID, NS_RECEIVER, sb.buf);
    sb_free(&sb);
    if (rc != 0) {
        snprintf(err, errcap, "connection to the TV was lost");
        return -1;
    }
    json *st = wait_receiver(s, "RECEIVER_STATUS", id, err, errcap);
    if (!st)
        return -1;
    json_free(st);
    return 0;
}

int cast_open_url(const char *ip, uint16_t port, const char *url, char *err, size_t errcap)
{
    session s;
    char buf[160];
    if (open_session(&s, ip, port, err, errcap) != 0)
        return -1;

    json *st = get_status(&s, err, errcap);
    /* DashCast must start fresh: once it navigated to a page it no longer
     * listens for new URLs, so stop whatever is running first. */
    if (!st || stop_running(&s, st, err, errcap) != 0) {
        json_free(st);
        close_session(&s);
        return -1;
    }
    json_free(st);

    int id = s.next_request++;
    snprintf(buf, sizeof(buf), "{\"type\":\"LAUNCH\",\"appId\":\"" DASHCAST_APP_ID "\",\"requestId\":%d}", id);
    if (send_json(&s, RECEIVER_ID, NS_RECEIVER, buf) != 0) {
        snprintf(err, errcap, "connection to the TV was lost");
        close_session(&s);
        return -1;
    }
    /* Wait until the status shows DashCast running with a transport id. */
    char transport[64] = "", session_id[64] = "";
    uint64_t end = plat_ticks_ms() + REPLY_TIMEOUT_MS;
    while (!transport[0] && plat_ticks_ms() < end) {
        json *j = wait_receiver(&s, "RECEIVER_STATUS", id, err, errcap);
        if (!j) {
            close_session(&s);
            return -1;
        }
        json *app = first_app(j);
        if (strcmp(json_str(json_get(app, "appId"), ""), DASHCAST_APP_ID) == 0) {
            snprintf(transport, sizeof(transport), "%s", json_str(json_get(app, "transportId"), ""));
            snprintf(session_id, sizeof(session_id), "%s", json_str(json_get(app, "sessionId"), ""));
        }
        json_free(j);
    }
    if (!transport[0]) {
        snprintf(err, errcap, "the viewer app did not start on the TV");
        close_session(&s);
        return -1;
    }

    sb_t sb;
    sb_init(&sb);
    sb_puts(&sb, "{\"url\":");
    sb_json_str(&sb, url);
    sb_printf(&sb, ",\"force\":true,\"reload\":false,\"reload_time\":0,\"requestId\":%d,\"sessionId\":",
              s.next_request++);
    sb_json_str(&sb, session_id);
    sb_putc(&sb, '}');
    int rc = send_json(&s, transport, NS_CONNECTION, "{\"type\":\"CONNECT\",\"origin\":{}}");
    if (rc == 0 && !sb.oom)
        rc = send_json(&s, transport, DASHCAST_NAMESPACE, sb.buf);
    sb_free(&sb);
    if (rc != 0) {
        snprintf(err, errcap, "connection to the TV was lost");
        close_session(&s);
        return -1;
    }
    /* Give the TV a moment to take the message before hanging up. */
    cast_msg m;
    if (read_message(&s, &m, 1500) == 1)
        cast_msg_free(&m);
    send_json(&s, transport, NS_CONNECTION, "{\"type\":\"CLOSE\"}");
    close_session(&s);
    LOGI("TV at %s is opening %s", ip, url);
    return 0;
}

int cast_stop_app(const char *ip, uint16_t port, char *err, size_t errcap)
{
    session s;
    if (open_session(&s, ip, port, err, errcap) != 0)
        return -1;
    json *st = get_status(&s, err, errcap);
    int rc = st ? stop_running(&s, st, err, errcap) : -1;
    json_free(st);
    close_session(&s);
    return rc;
}
