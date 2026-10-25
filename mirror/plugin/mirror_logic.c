#include "mirror_logic.h"

#include <string.h>

/* Widths and heights must be multiples of 16 for the JPEG encoder. */
const mirror_preset mirror_presets[MIRROR_PRESETS] = {
    { "Smooth", 480, 272 },
    { "Balanced", 640, 368 },
    { "Sharp", 960, 544 },
};

const char *layout_name(yuv_layout l)
{
    switch (l) {
    case LAYOUT_NV12: return "NV12";
    case LAYOUT_NV21: return "NV21";
    case LAYOUT_I420: return "I420";
    case LAYOUT_YV12: return "YV12";
    default: return "unknown";
    }
}

static int clamp8(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

void rgb_to_ycbcr(int r, int g, int b, int *y, int *cb, int *cr)
{
    /* Fixed point (x1024) to stay away from floating point in the kernel. */
    *y = clamp8((306 * r + 601 * g + 117 * b + 512) >> 10);
    *cb = clamp8(128 + ((-173 * r - 339 * g + 512 * b + 512) >> 10));
    *cr = clamp8(128 + ((512 * r - 429 * g - 83 * b + 512) >> 10));
}

static int near(int a, int b)
{
    int d = a - b;
    return d >= -12 && d <= 12;
}

/* True if every sample at base + i*step (i < n) is close to v. */
static int run_is(const uint8_t *buf, size_t base, size_t step, int n, int v)
{
    for (int i = 0; i < n; i++)
        if (!near(buf[base + (size_t)i * step], v))
            return 0;
    return 1;
}

yuv_layout detect_layout(const uint8_t *buf, int w, int h, int cb, int cr)
{
    size_t c = (size_t)w * (size_t)h;
    size_t quarter = c / 4;

    if (near(cb, cr))
        return LAYOUT_UNKNOWN; /* the test colour cannot tell them apart */
    /* Interleaved: chroma pairs alternate. */
    if (run_is(buf, c, 2, 16, cb) && run_is(buf, c + 1, 2, 16, cr))
        return LAYOUT_NV12;
    if (run_is(buf, c, 2, 16, cr) && run_is(buf, c + 1, 2, 16, cb))
        return LAYOUT_NV21;
    /* Planar: a run of one component, then the other plane. */
    if (run_is(buf, c, 1, 32, cb) && run_is(buf, c + quarter, 1, 32, cr))
        return LAYOUT_I420;
    if (run_is(buf, c, 1, 32, cr) && run_is(buf, c + quarter, 1, 32, cb))
        return LAYOUT_YV12;
    return LAYOUT_UNKNOWN;
}

int request_complete(const char *buf, size_t len)
{
    for (size_t i = 0; i + 3 < len; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return 1;
    return 0;
}

static int path_is(const char *path, size_t plen, const char *want)
{
    size_t n = strlen(want);
    return plen == n && memcmp(path, want, n) == 0;
}

req_kind parse_request(const char *buf, size_t len, char *query, size_t qcap)
{
    if (qcap)
        query[0] = '\0';
    if (len < 6 || memcmp(buf, "GET ", 4) != 0)
        return REQ_BAD;
    const char *path = buf + 4;
    const char *end = buf + len;
    const char *p = path;
    while (p < end && *p != ' ' && *p != '\r' && *p != '\n')
        p++;
    const char *q = memchr(path, '?', (size_t)(p - path));
    size_t plen = (size_t)((q ? q : p) - path);
    if (q && qcap) {
        size_t n = (size_t)(p - (q + 1));
        if (n >= qcap)
            n = qcap - 1;
        memcpy(query, q + 1, n);
        query[n] = '\0';
    }
    if (path_is(path, plen, "/") || path_is(path, plen, "/index.html"))
        return REQ_INDEX;
    if (path_is(path, plen, "/stream.mjpg"))
        return REQ_STREAM;
    if (path_is(path, plen, "/frame.jpg"))
        return REQ_FRAME;
    if (path_is(path, plen, "/status"))
        return REQ_STATUS;
    if (path_is(path, plen, "/control"))
        return REQ_CONTROL;
    return REQ_NOT_FOUND;
}

int query_int(const char *query, const char *key, int *out)
{
    size_t klen = strlen(key);
    const char *p = query;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            int neg = 0, val = 0, digits = 0;
            if (*v == '-') {
                neg = 1;
                v++;
            }
            while (*v >= '0' && *v <= '9' && digits < 9) {
                val = val * 10 + (*v - '0');
                v++;
                digits++;
            }
            if (!digits)
                return 0;
            *out = neg ? -val : val;
            return 1;
        }
        p = strchr(p, '&');
        if (p)
            p++;
    }
    return 0;
}

/* The page senders open. On a Chromecast (reached through DashCast) it also
 * starts the Cast receiver SDK so the TV does not close an "idle" app. */
const char mirror_viewer_html[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>PS Vita</title><style>"
    "html,body{margin:0;height:100%;background:#000;overflow:hidden}"
    "img{width:100%;height:100%;object-fit:contain;display:block}"
    "#s{position:fixed;left:0;right:0;bottom:7%;text-align:center;color:#9aa;"
    "font:600 3vh sans-serif;text-shadow:0 0 6px #000}"
    "</style></head><body><img id=v alt=\"\"><div id=s>Connecting to PS Vita...</div><script>\n"
    "var v=document.getElementById('v'),s=document.getElementById('s'),bad=0,live=0;\n"
    "function start(){v.src='/stream.mjpg?t='+Date.now();}\n"
    "v.onload=function(){live=1;s.textContent='';};\n"
    "v.onerror=function(){live=0;s.textContent='Reconnecting...';setTimeout(start,1000);};\n"
    "function check(){var c=new AbortController();var t=setTimeout(function(){c.abort();},3000);\n"
    " fetch('/status',{signal:c.signal,cache:'no-store'}).then(function(r){return r.json();})\n"
    " .then(function(j){clearTimeout(t);if(bad>1){start();}bad=0;\n"
    "  s.textContent=j.paused?'Paused on PS Vita':(live?'':s.textContent);})\n"
    " .catch(function(){clearTimeout(t);bad++;if(bad>1){live=0;s.textContent='Waiting for PS Vita...';}});}\n"
    "setInterval(check,4000);start();\n"
    "if(/CrKey/.test(navigator.userAgent)){var e=document.createElement('script');"
    "e.src='https://www.gstatic.com/cast/sdk/libs/caf_receiver/v3/cast_receiver_framework.js';"
    "e.onload=function(){try{var o=new cast.framework.CastReceiverOptions();"
    "o.disableIdleTimeout=true;o.skipPlayersLoad=true;"
    "cast.framework.CastReceiverContext.getInstance().start(o);}catch(x){}};"
    "document.head.appendChild(e);}\n"
    "</script></body></html>";
