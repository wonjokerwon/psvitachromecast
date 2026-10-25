/* Unit tests for the portable core (JSON, protobuf, Cast framing, mDNS). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/castmsg.h"
#include "../src/core/http.h"
#include "../src/core/json.h"
#include "../src/core/mdns.h"
#include "../src/core/sb.h"
#include "../mirror/plugin/mirror_logic.h"

static int g_failures;
static int g_checks;

#define CHECK(cond)                                                       \
    do {                                                                  \
        g_checks++;                                                       \
        if (!(cond)) {                                                    \
            g_failures++;                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

static void test_json(void)
{
    const char *text =
        "{\"type\":\"LOAD\",\"requestId\":7,\"autoplay\":true,\"currentTime\":12.5,"
        "\"media\":{\"contentId\":\"http://x/a b.mp4\",\"metadata\":{\"title\":\"T\\u00e9st \\ud83d\\ude00\"},"
        "\"duration\":null},\"list\":[1,2,{\"a\":[]}],\"esc\":\"q\\\"\\\\\\n\"}";
    json *j = json_parse(text, strlen(text));
    CHECK(j != NULL);
    if (!j)
        return;
    CHECK(strcmp(json_str(json_get(j, "type"), ""), "LOAD") == 0);
    CHECK(json_num(json_get(j, "requestId"), 0) == 7);
    CHECK(json_bool(json_get(j, "autoplay"), 0) == 1);
    CHECK(json_num(json_get(j, "currentTime"), 0) == 12.5);
    json *media = json_get(j, "media");
    CHECK(strcmp(json_str(json_get(json_get(media, "metadata"), "title"), ""),
                 "T\xc3\xa9st \xf0\x9f\x98\x80") == 0);
    CHECK(strcmp(json_str(json_get(j, "esc"), ""), "q\"\\\n") == 0);

    sb_t sb;
    sb_init(&sb);
    json_write(&sb, j);
    json *again = json_parse(sb.buf, sb.len);
    CHECK(again != NULL);
    sb_t sb2;
    sb_init(&sb2);
    json_write(&sb2, again);
    CHECK(sb.len == sb2.len && memcmp(sb.buf, sb2.buf, sb.len) == 0);
    sb_free(&sb2);
    json_free(again);

    sb_reset(&sb);
    int n = json_write_obj_open(&sb, media, "duration");
    sb_putc(&sb, '}');
    CHECK(n == 2);
    CHECK(strstr(sb.buf, "duration") == NULL);

    json *detached = json_detach(j, "media");
    CHECK(detached == media);
    CHECK(json_get(j, "media") == NULL);
    json_free(detached);
    json_free(j);
    sb_free(&sb);

    const char *bad[] = { "", "{", "{\"a\":}", "[1,]", "{\"a\" 1}", "\"\\x\"", "tru", "{} x", NULL };
    for (int i = 0; bad[i]; i++) {
        json *b = json_parse(bad[i], strlen(bad[i]));
        CHECK(b == NULL);
        json_free(b);
    }

    char deep[300];
    memset(deep, '[', 200);
    memset(deep + 200, ']', 99);
    deep[299] = '\0';
    CHECK(json_parse(deep, strlen(deep)) == NULL);
}

static void test_castmsg(void)
{
    sb_t sb;
    sb_init(&sb);
    const char *payload = "{\"type\":\"PING\"}";
    CHECK(cast_msg_encode_framed(&sb, "sender-0", "receiver-0", NS_HEARTBEAT, CAST_PAYLOAD_STRING,
                                 payload, strlen(payload)) == 0);
    CHECK(sb.len > 4);
    const uint8_t *p = (const uint8_t *)sb.buf;
    uint32_t len = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    CHECK(len == sb.len - 4);
    cast_msg m;
    CHECK(cast_msg_decode(p + 4, len, &m) == 0);
    CHECK(strcmp(m.source_id, "sender-0") == 0);
    CHECK(strcmp(m.destination_id, "receiver-0") == 0);
    CHECK(strcmp(m.ns, NS_HEARTBEAT) == 0);
    CHECK(m.payload_type == CAST_PAYLOAD_STRING);
    CHECK(m.payload_len == strlen(payload) && memcmp(m.payload, payload, m.payload_len) == 0);
    cast_msg_free(&m);

    /* Truncated input must fail cleanly; a cut on a field boundary before
     * the payload is a valid message with an empty payload. */
    for (uint32_t cut = 0; cut < len; cut++) {
        cast_msg t;
        int rc = cast_msg_decode(p + 4, cut, &t);
        CHECK(rc != 0 || t.payload_len == 0);
        if (rc == 0)
            cast_msg_free(&t);
    }
    sb_free(&sb);

    /* Device auth challenge: { challenge: { sig_alg: 1, nonce: "abc", hash: 1 } } */
    sb_t ch, outer;
    sb_init(&ch);
    sb_init(&outer);
    pb_put_uint(&ch, 1, 1);
    pb_put_bytes(&ch, 2, "abc", 3);
    pb_put_uint(&ch, 3, 1);
    pb_put_bytes(&outer, 1, ch.buf, ch.len);
    auth_challenge ac;
    CHECK(auth_parse_challenge((const uint8_t *)outer.buf, outer.len, &ac) == 0);
    CHECK(ac.present);
    CHECK(ac.signature_algorithm == 1 && ac.hash_algorithm == 1);
    CHECK(ac.sender_nonce_len == 3 && memcmp(ac.sender_nonce, "abc", 3) == 0);

    /* Empty DeviceAuthMessage -> no challenge. */
    CHECK(auth_parse_challenge((const uint8_t *)"", 0, &ac) == 0 && !ac.present);

    sb_t resp;
    sb_init(&resp);
    auth_build_response(&resp, (const uint8_t *)"SIG", 3, (const uint8_t *)"CERT", 4, 1, 1,
                        (const uint8_t *)"abc", 3);
    pb_reader r = { (const uint8_t *)resp.buf, (const uint8_t *)resp.buf + resp.len };
    pb_field f;
    CHECK(pb_next(&r, &f) == 1 && f.num == 2 && f.wire_type == 2);
    pb_reader rr = { f.data, f.data + f.len };
    int seen = 0;
    while (pb_next(&rr, &f) == 1) {
        if (f.num == 1)
            seen |= (f.len == 3 && memcmp(f.data, "SIG", 3) == 0) ? 1 : 0;
        if (f.num == 2)
            seen |= (f.len == 4 && memcmp(f.data, "CERT", 4) == 0) ? 2 : 0;
        if (f.num == 5)
            seen |= (f.len == 3) ? 4 : 0;
    }
    CHECK(seen == 7);
    sb_free(&resp);
    sb_free(&ch);
    sb_free(&outer);

    sb_t big;
    sb_init(&big);
    pb_put_varint(&big, 300);
    CHECK(big.len == 2 && (uint8_t)big.buf[0] == 0xAC && (uint8_t)big.buf[1] == 0x02);
    sb_free(&big);
}

static size_t build_query(uint8_t *buf, const char *name, uint16_t qtype, int qu)
{
    size_t o = 0;
    memset(buf, 0, 12);
    buf[5] = 1; /* qdcount */
    o = 12;
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t n = dot ? (size_t)(dot - p) : strlen(p);
        buf[o++] = (uint8_t)n;
        memcpy(buf + o, p, n);
        o += n;
        p = dot ? dot + 1 : p + n;
    }
    buf[o++] = 0;
    buf[o++] = (uint8_t)(qtype >> 8);
    buf[o++] = (uint8_t)qtype;
    buf[o++] = qu ? 0x80 : 0;
    buf[o++] = 1;
    return o;
}

static void test_mdns(void)
{
    mdns_info info;
    memset(&info, 0, sizeof(info));
    snprintf(info.instance, sizeof(info.instance), "VitaCast-abc");
    snprintf(info.hostname, sizeof(info.hostname), "1234-host");
    snprintf(info.ip, sizeof(info.ip), "192.168.1.20");
    info.port = 8009;
    snprintf(info.txt[info.txt_count++], 128, "fn=My Vita");
    snprintf(info.txt[info.txt_count++], 128, "md=PS Vita");

    uint8_t buf[1500];
    size_t n = mdns_build_response(buf, sizeof(buf), &info, 0, 1, 0);
    CHECK(n > 12);
    CHECK(buf[2] == 0x84 && buf[7] == 1 && buf[11] == 3);
    CHECK(mdns_build_response(buf, 40, &info, 0, 1, 0) == 0); /* overflow detected */

    uint8_t q[512];
    int unicast = 0;
    size_t ql = build_query(q, "_googlecast._tcp.local", 12, 0);
    CHECK(mdns_classify_query(q, ql, &info, &unicast, NULL) == MDNS_Q_SERVICE && !unicast);
    ql = build_query(q, "_GoogleCast._TCP.local", 12, 1);
    CHECK(mdns_classify_query(q, ql, &info, &unicast, NULL) == MDNS_Q_SERVICE && unicast);
    ql = build_query(q, "VitaCast-abc._googlecast._tcp.local", 33, 0);
    CHECK(mdns_classify_query(q, ql, &info, &unicast, NULL) == MDNS_Q_SERVICE);
    ql = build_query(q, "1234-host.local", 1, 0);
    CHECK(mdns_classify_query(q, ql, &info, &unicast, NULL) == MDNS_Q_SERVICE);
    ql = build_query(q, "_services._dns-sd._udp.local", 12, 0);
    CHECK(mdns_classify_query(q, ql, &info, &unicast, NULL) == MDNS_Q_SERVICES);
    ql = build_query(q, "_airplay._tcp.local", 12, 0);
    CHECK(mdns_classify_query(q, ql, &info, &unicast, NULL) == MDNS_Q_NONE);
    /* Responses are ignored. */
    CHECK(mdns_classify_query(buf, n, &info, &unicast, NULL) == MDNS_Q_NONE);
    /* Compression pointer loop must not hang. */
    uint8_t loop[] = { 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xC0, 12, 0, 12, 0, 1 };
    CHECK(mdns_classify_query(loop, sizeof(loop), &info, &unicast, NULL) == MDNS_Q_NONE);
    /* Fuzz-ish: random garbage must not crash. */
    srand(1);
    for (int i = 0; i < 2000; i++) {
        size_t len = (size_t)(rand() % 200);
        for (size_t k = 0; k < len; k++)
            q[k] = (uint8_t)rand();
        if (len > 2)
            q[2] &= 0x7F;
        mdns_classify_query(q, len, &info, &unicast, NULL);
    }
}

static void test_url(void)
{
    http_url u;
    CHECK(http_parse_url("http://example.com/a/b.mp4?x=1#frag", &u) == 0);
    CHECK(!u.tls && u.port == 80 && strcmp(u.host, "example.com") == 0);
    CHECK(strcmp(u.path, "/a/b.mp4?x=1") == 0);
    CHECK(http_parse_url("https://user:pw@cdn.example.org:8443", &u) == 0);
    CHECK(u.tls && u.port == 8443 && strcmp(u.host, "cdn.example.org") == 0 && strcmp(u.path, "/") == 0);
    CHECK(http_parse_url("http://10.0.0.2:8000/my movie.mp4", &u) == 0);
    CHECK(strcmp(u.path, "/my%20movie.mp4") == 0);
    CHECK(http_parse_url("http://h?q=1", &u) == 0 && strcmp(u.path, "/?q=1") == 0);
    CHECK(http_parse_url("ftp://example.com/x", &u) != 0);
    CHECK(http_parse_url("http://:80/x", &u) != 0);
    CHECK(http_parse_url("http://h:99999/x", &u) != 0);
}

static void fill_layout(uint8_t *buf, int w, int h, yuv_layout l, int y, int cb, int cr)
{
    size_t c = (size_t)w * h, q = c / 4;
    memset(buf, y, c);
    for (size_t i = 0; i < q; i++) {
        switch (l) {
        case LAYOUT_NV12: buf[c + 2 * i] = (uint8_t)cb; buf[c + 2 * i + 1] = (uint8_t)cr; break;
        case LAYOUT_NV21: buf[c + 2 * i] = (uint8_t)cr; buf[c + 2 * i + 1] = (uint8_t)cb; break;
        case LAYOUT_I420: buf[c + i] = (uint8_t)cb; buf[c + q + i] = (uint8_t)cr; break;
        case LAYOUT_YV12: buf[c + i] = (uint8_t)cr; buf[c + q + i] = (uint8_t)cb; break;
        default: break;
        }
    }
}

static void test_mirror_logic(void)
{
    int y, cb, cr;
    rgb_to_ycbcr(255, 255, 255, &y, &cb, &cr);
    CHECK(y == 255 && cb == 128 && cr == 128);
    rgb_to_ycbcr(0, 0, 0, &y, &cb, &cr);
    CHECK(y == 0 && cb == 128 && cr == 128);
    rgb_to_ycbcr(30, 60, 220, &y, &cb, &cr); /* the plugin's self-test colour */
    CHECK(y > 60 && y < 75 && cb > 205 && cb < 220 && cr > 92 && cr < 108);

    enum { W = 64, H = 32 };
    static uint8_t buf[W * H * 3 / 2];
    yuv_layout all[] = { LAYOUT_NV12, LAYOUT_NV21, LAYOUT_I420, LAYOUT_YV12 };
    for (int i = 0; i < 4; i++) {
        fill_layout(buf, W, H, all[i], y, cb, cr);
        CHECK(detect_layout(buf, W, H, cb, cr) == all[i]);
        /* small encoder rounding differences are tolerated */
        fill_layout(buf, W, H, all[i], y, cb - 5, cr + 4);
        CHECK(detect_layout(buf, W, H, cb, cr) == all[i]);
    }
    memset(buf, 0, sizeof(buf));
    CHECK(detect_layout(buf, W, H, cb, cr) == LAYOUT_UNKNOWN);
    CHECK(detect_layout(buf, W, H, 128, 130) == LAYOUT_UNKNOWN); /* ambiguous colour */

    char q[64];
    const char *r1 = "GET /stream.mjpg?t=123 HTTP/1.1\r\nHost: x\r\n\r\n";
    CHECK(request_complete(r1, strlen(r1)));
    CHECK(!request_complete(r1, strlen(r1) - 2));
    CHECK(parse_request(r1, strlen(r1), q, sizeof(q)) == REQ_STREAM && strcmp(q, "t=123") == 0);
    CHECK(parse_request("GET / HTTP/1.1\r\n\r\n", 18, q, sizeof(q)) == REQ_INDEX && q[0] == 0);
    CHECK(parse_request("GET /status HTTP/1.1\r\n", 22, q, sizeof(q)) == REQ_STATUS);
    CHECK(parse_request("GET /frame.jpg HTTP/1.1\r\n", 25, q, sizeof(q)) == REQ_FRAME);
    CHECK(parse_request("GET /control?preset=2&ratio=90 HTTP/1.1", 39, q, sizeof(q)) == REQ_CONTROL);
    int v = -1;
    CHECK(query_int(q, "preset", &v) && v == 2);
    CHECK(query_int(q, "ratio", &v) && v == 90);
    CHECK(!query_int(q, "fps", &v));
    CHECK(!query_int("xpreset=1", "preset", &v));
    CHECK(query_int("paused=0", "paused", &v) && v == 0);
    CHECK(parse_request("GET /nope HTTP/1.1", 18, q, sizeof(q)) == REQ_NOT_FOUND);
    CHECK(parse_request("POST / HTTP/1.1", 15, q, sizeof(q)) == REQ_BAD);
    CHECK(parse_request("GET", 3, q, sizeof(q)) == REQ_BAD);
    for (int i = 0; i < MIRROR_PRESETS; i++)
        CHECK(mirror_presets[i].width % 16 == 0 && mirror_presets[i].height % 16 == 0);
    CHECK(strstr(mirror_viewer_html, "/stream.mjpg") != NULL);
}

static void test_mdns_browse_parse(void)
{
    mdns_info info;
    memset(&info, 0, sizeof(info));
    snprintf(info.instance, sizeof(info.instance), "Chromecast-abc");
    snprintf(info.hostname, sizeof(info.hostname), "abc-host");
    snprintf(info.ip, sizeof(info.ip), "10.1.2.3");
    info.port = 8009;
    snprintf(info.txt[info.txt_count++], 128, "fn=Living Room TV");
    snprintf(info.txt[info.txt_count++], 128, "md=Chromecast");
    snprintf(info.txt[info.txt_count++], 128, "ca=4101");

    uint8_t pkt[1500];
    size_t n = mdns_build_response(pkt, sizeof(pkt), &info, 0, 1, 0);
    mdns_collector c;
    memset(&c, 0, sizeof(c));
    mdns_collect(&c, pkt, n, "10.9.9.9");
    cast_device d[4];
    CHECK(mdns_collector_devices(&c, d, 4) == 1);
    CHECK(strcmp(d[0].name, "Living Room TV") == 0);
    CHECK(strcmp(d[0].ip, "10.1.2.3") == 0); /* A record wins over the sender address */
    CHECK(d[0].port == 8009 && strcmp(d[0].model, "Chromecast") == 0);

    /* Duplicate answers do not duplicate devices. */
    mdns_collect(&c, pkt, n, "10.9.9.9");
    CHECK(mdns_collector_devices(&c, d, 4) == 1);

    /* A speaker (no video out bit) is ignored. */
    mdns_info spk = info;
    snprintf(spk.instance, sizeof(spk.instance), "Speaker-1");
    snprintf(spk.txt[2], 128, "ca=2052");
    n = mdns_build_response(pkt, sizeof(pkt), &spk, 0, 1, 0);
    mdns_collect(&c, pkt, n, "10.9.9.8");
    CHECK(mdns_collector_devices(&c, d, 4) == 1);

    /* Queries are not mistaken for answers; garbage does not crash. */
    uint8_t q[512];
    size_t ql = mdns_build_query(q, sizeof(q));
    CHECK(ql > 12);
    mdns_collect(&c, q, ql, "10.0.0.1");
    srand(7);
    for (int i = 0; i < 2000; i++) {
        size_t len = (size_t)(rand() % 300);
        for (size_t k = 0; k < len; k++)
            pkt[k] = (uint8_t)rand();
        if (len > 2)
            pkt[2] |= 0x80;
        mdns_collect(&c, pkt, len, "10.0.0.2");
    }
    CHECK(c.ninst <= MDNS_MAX_SEEN && c.nhost <= MDNS_MAX_SEEN);
}

int main(void)
{
    test_json();
    test_castmsg();
    test_mdns();
    test_url();
    test_mirror_logic();
    test_mdns_browse_parse();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
