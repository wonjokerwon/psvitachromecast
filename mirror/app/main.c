/* VitaCast Mirror: shows the PS Vita screen on a Chromecast / Google TV.
 *
 * The app loads the mirroring kernel plugin (which keeps running after the
 * app closes), finds TVs on the network and tells the chosen one to open
 * the plugin's live view through DashCast. */
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <taihen.h>
#include <vita2d.h>

#include "../../mirror/plugin/mirror_logic.h"
#include "../../src/core/cast_sender.h"
#include "../../src/core/json.h"
#include "../../src/core/log.h"
#include "../../src/core/mdns.h"
#include "../../src/core/net.h"
#include "../../src/core/sb.h"
#include "../../src/core/tls.h"
#include "../../src/platform/platform.h"
#include "../../src/vita/vita.h"

#define PLUGIN_PATH "ux0:app/VCST00002/vitacast_mirror.skprx"
#define MAX_TVS 16

#define SCREEN_W 960
#define SCREEN_H 544
#define COL_BG RGBA8(14, 17, 22, 255)
#define COL_PANEL RGBA8(28, 33, 42, 255)
#define COL_SEL RGBA8(44, 64, 100, 255)
#define COL_TEXT RGBA8(240, 242, 245, 255)
#define COL_DIM RGBA8(150, 158, 170, 255)
#define COL_FAINT RGBA8(95, 102, 114, 255)
#define COL_ACCENT RGBA8(92, 156, 255, 255)
#define COL_OK RGBA8(80, 200, 120, 255)
#define COL_ERR RGBA8(235, 87, 87, 255)

int _newlib_heap_size_user = 32 * 1024 * 1024;

typedef enum { JOB_NONE, JOB_SCAN, JOB_CAST, JOB_STOP } job_kind;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int g_job;          /* job_kind being processed */
static atomic_int g_quit;
static cast_device g_tvs[MAX_TVS];
static int g_ntvs;
static int g_scanned;
static cast_device g_target;      /* argument of JOB_CAST / JOB_STOP */
static cast_device g_active;      /* TV we are mirroring to */
static int g_mirroring;
static char g_message[160];
static int g_message_error;
static char g_ip[16];
static char g_plugin_error[160];
static atomic_int g_plugin_ok;

/* mirror status from the plugin */
static int g_fps, g_streams, g_paused, g_preset = 1;
static char g_selftest[100];

static void set_message(int error, const char *fmt, ...)
{
    va_list ap;
    pthread_mutex_lock(&g_lock);
    va_start(ap, fmt);
    vsnprintf(g_message, sizeof(g_message), fmt, ap);
    va_end(ap);
    g_message_error = error;
    pthread_mutex_unlock(&g_lock);
    LOGI("%s", g_message);
}

/* --- plugin --- */

/* GET http://127.0.0.1:8080<path>; parses a JSON body. */
static json *plugin_get(const char *path)
{
    tstream ts;
    char req[256], buf[2048];
    int len = 0;
    if (tstream_connect(&ts, "127.0.0.1", MIRROR_PORT, 0, 1000) != 0)
        return NULL;
    snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n", path);
    if (tstream_write(&ts, req, strlen(req)) == 0) {
        for (;;) {
            int n = tstream_read(&ts, buf + len, sizeof(buf) - 1 - (size_t)len, 2000);
            if (n <= 0)
                break;
            len += n;
            if (len >= (int)sizeof(buf) - 1)
                break;
        }
    }
    tstream_close(&ts);
    buf[len] = '\0';
    char *body = strstr(buf, "\r\n\r\n");
    return body ? json_parse(body + 4, strlen(body + 4)) : NULL;
}

static void read_plugin_status(json *j)
{
    if (!j)
        return;
    pthread_mutex_lock(&g_lock);
    g_fps = (int)json_num(json_get(j, "fps"), 0);
    g_streams = (int)json_num(json_get(j, "streams"), 0);
    g_paused = json_bool(json_get(j, "paused"), 0);
    g_preset = (int)json_num(json_get(j, "preset"), 1);
    snprintf(g_selftest, sizeof(g_selftest), "%s", json_str(json_get(j, "selftest"), ""));
    pthread_mutex_unlock(&g_lock);
}

static void load_plugin(void)
{
    json *st = plugin_get("/status");
    if (!st) {
        SceUID id = taiLoadStartKernelModule(PLUGIN_PATH, 0, NULL, 0);
        if (id < 0) {
            snprintf(g_plugin_error, sizeof(g_plugin_error),
                     "Could not start the mirroring plugin (0x%08X). Turn on Settings > HENkaku "
                     "Settings > Enable Unsafe Homebrew.", (unsigned)id);
            LOGI("%s", g_plugin_error);
            return;
        }
        LOGI("mirroring plugin loaded (module 0x%08X)", (unsigned)id);
        for (int i = 0; i < 30 && !st; i++) {
            plat_sleep_ms(100);
            st = plugin_get("/status");
        }
        if (!st) {
            snprintf(g_plugin_error, sizeof(g_plugin_error),
                     "The mirroring plugin is not answering (see ux0:data/VitaCast/mirror_log.txt).");
            LOGI("%s", g_plugin_error);
            return;
        }
    }
    read_plugin_status(st);
    json_free(st);
    g_plugin_ok = 1;
}

/* --- background work --- */

static void *worker_main(void *arg)
{
    (void)arg;
    uint64_t last_status = 0;
    load_plugin();
    atomic_store(&g_job, JOB_SCAN);
    while (!atomic_load(&g_quit)) {
        int job = atomic_load(&g_job);
        if (job == JOB_SCAN) {
            cast_device found[MAX_TVS];
            int n = mdns_browse(g_ip, 2500, found, MAX_TVS);
            pthread_mutex_lock(&g_lock);
            g_ntvs = n > 0 ? n : 0;
            memcpy(g_tvs, found, sizeof(cast_device) * (size_t)g_ntvs);
            g_scanned = 1;
            pthread_mutex_unlock(&g_lock);
            if (n < 0)
                set_message(1, "Network error while searching for TVs.");
            atomic_store(&g_job, JOB_NONE);
        } else if (job == JOB_CAST || job == JOB_STOP) {
            char err[160] = "", url[64];
            cast_device tv;
            pthread_mutex_lock(&g_lock);
            tv = g_target;
            pthread_mutex_unlock(&g_lock);
            snprintf(url, sizeof(url), "http://%s:%d/", g_ip, MIRROR_PORT);
            int rc = job == JOB_CAST ? cast_open_url(tv.ip, tv.port, url, err, sizeof(err))
                                     : cast_stop_app(tv.ip, tv.port, err, sizeof(err));
            pthread_mutex_lock(&g_lock);
            if (rc == 0 && job == JOB_CAST) {
                g_active = tv;
                g_mirroring = 1;
            } else if (rc == 0) {
                g_mirroring = 0;
            }
            pthread_mutex_unlock(&g_lock);
            if (rc != 0)
                set_message(1, "%s: %s", tv.name, err);
            else if (job == JOB_CAST)
                set_message(0, "Mirroring to %s. You can close this app and play.", tv.name);
            else
                set_message(0, "Stopped mirroring to %s.", tv.name);
            atomic_store(&g_job, JOB_NONE);
        } else {
            uint64_t now = plat_ticks_ms();
            if (g_plugin_ok && now - last_status > 1000) {
                last_status = now;
                json *st = plugin_get("/status");
                read_plugin_status(st);
                json_free(st);
            }
            plat_sleep_ms(50);
        }
    }
    return NULL;
}

static void plugin_control(const char *query)
{
    char path[96];
    snprintf(path, sizeof(path), "/control?%s", query);
    json *st = plugin_get(path);
    read_plugin_status(st);
    json_free(st);
}

/* --- UI --- */

static vita2d_pgf *g_font;

static void text(int x, int y, unsigned color, float scale, const char *s)
{
    vita2d_pgf_draw_text(g_font, x, y, color, scale, s);
}

static int text_w(float scale, const char *s)
{
    return vita2d_pgf_text_width(g_font, scale, s);
}

/* Draws text wrapped at word boundaries to max_w pixels; returns the next y. */
static int text_wrapped(int x, int y, int max_w, unsigned color, float scale, const char *s)
{
    char line[256];
    int line_h = (int)(28 * scale);
    while (*s) {
        size_t n = 0, fit = 0;
        while (s[n] && n < sizeof(line) - 1) {
            size_t next = n;
            while (s[next] && s[next] != ' ' && next < sizeof(line) - 1)
                next++;
            memcpy(line, s, next);
            line[next] = '\0';
            if (fit && text_w(scale, line) > max_w)
                break;
            fit = next;
            n = next;
            if (s[n] == ' ')
                n++;
        }
        if (!fit)
            fit = n ? n : 1; /* a single word wider than the line */
        memcpy(line, s, fit);
        line[fit] = '\0';
        text(x, y, color, scale, line);
        y += line_h;
        s += fit;
        while (*s == ' ')
            s++;
    }
    return y;
}

static void draw(int selected)
{
    char buf[200];
    vita2d_start_drawing();
    vita2d_clear_screen();

    text(40, 62, COL_TEXT, 1.6f, "VitaCast Mirror");
    snprintf(buf, sizeof(buf), "This Vita: %s", g_ip[0] ? g_ip : "no Wi-Fi");
    text(SCREEN_W - 40 - text_w(1.0f, buf), 62, COL_DIM, 1.0f, buf);

    pthread_mutex_lock(&g_lock);
    int job = atomic_load(&g_job);
    if (!g_plugin_ok && g_plugin_error[0]) {
        text(40, 130, COL_ERR, 1.1f, "Mirroring is unavailable");
        text_wrapped(40, 165, SCREEN_W - 80, COL_DIM, 0.95f, g_plugin_error);
    } else {
        text(40, 120, COL_DIM, 1.0f, "Pick a TV or Chromecast on this Wi-Fi network:");
        int y = 150;
        if (!g_scanned || (job == JOB_SCAN && g_ntvs == 0)) {
            text(60, y + 30, COL_FAINT, 1.0f, "Searching for TVs...");
        } else if (g_ntvs == 0) {
            text(60, y + 30, COL_FAINT, 1.0f, "No TVs found. Press TRIANGLE to search again.");
        }
        for (int i = 0; i < g_ntvs && i < 6; i++) {
            int active = g_mirroring && strcmp(g_tvs[i].ip, g_active.ip) == 0;
            vita2d_draw_rectangle(40, (float)y, SCREEN_W - 80, 44, i == selected ? COL_SEL : COL_PANEL);
            text(60, y + 30, COL_TEXT, 1.1f, g_tvs[i].name);
            snprintf(buf, sizeof(buf), "%s%s", g_tvs[i].model, active ? "   - mirroring" : "");
            text(SCREEN_W - 60 - text_w(0.9f, buf), y + 29, active ? COL_OK : COL_DIM, 0.9f, buf);
            y += 50;
        }

        /* status line */
        int sy = 470;
        if (job == JOB_CAST || job == JOB_STOP) {
            snprintf(buf, sizeof(buf), "%s %s...", job == JOB_CAST ? "Connecting to" : "Stopping",
                     g_target.name);
            text(40, sy, COL_ACCENT, 1.0f, buf);
        } else if (g_message[0]) {
            text_wrapped(40, sy - 14, SCREEN_W - 80, g_message_error ? COL_ERR : COL_OK, 0.9f, g_message);
        }
        if (g_plugin_ok) {
            snprintf(buf, sizeof(buf), "Quality: %s   %s   %d fps   %d viewer%s   Browser: http://%s:%d/",
                     mirror_presets[g_preset >= 0 && g_preset < MIRROR_PRESETS ? g_preset : 1].name,
                     g_paused ? "PAUSED" : "live", g_fps, g_streams, g_streams == 1 ? "" : "s", g_ip,
                     MIRROR_PORT);
            text(40, 498, COL_DIM, 0.85f, buf);
        }
    }
    pthread_mutex_unlock(&g_lock);

    text(40, 528, COL_FAINT, 0.8f,
         "X mirror   O stop   TRIANGLE search   L/R quality   SQUARE pause   START exit (keeps mirroring)");
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

static int net_init(void)
{
    if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0)
        return -1;
    SceNetInitParam p;
    p.memory = malloc(1024 * 1024);
    p.size = 1024 * 1024;
    p.flags = 0;
    int rc = sceNetInit(&p);
    if (rc < 0 && rc != (int)SCE_NET_ERROR_EBUSY)
        return -1;
    sceNetCtlInit();
    return 0;
}

int main(void)
{
    SceCtrlData pad, prev;
    int selected = 0;
    pthread_t worker;

    vita_platform_init("mirror_app_log.txt");
    LOGI("VitaCast Mirror starting");
    vita2d_init();
    vita2d_set_clear_color(COL_BG);
    g_font = vita2d_load_default_pgf();

    if (net_init() != 0 || plat_local_ipv4(g_ip) != 0) {
        g_ip[0] = '\0';
        snprintf(g_plugin_error, sizeof(g_plugin_error),
                 "Connect to Wi-Fi (on the same network as your TV), then start again.");
    } else if (tls_init_client() != 0) {
        snprintf(g_plugin_error, sizeof(g_plugin_error), "Could not set up encryption (see the log).");
    } else {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 256 * 1024);
        pthread_create(&worker, &attr, worker_main, NULL);
        pthread_attr_destroy(&attr);
    }

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
    memset(&prev, 0, sizeof(prev));
    while (!atomic_load(&g_quit)) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned pressed = pad.buttons & ~prev.buttons;
        prev = pad;
        int busy = atomic_load(&g_job) != JOB_NONE;

        pthread_mutex_lock(&g_lock);
        int ntvs = g_ntvs;
        pthread_mutex_unlock(&g_lock);
        if (selected >= ntvs)
            selected = ntvs > 0 ? ntvs - 1 : 0;

        if (pressed & SCE_CTRL_START)
            atomic_store(&g_quit, 1);
        if (pressed & SCE_CTRL_UP && selected > 0)
            selected--;
        if (pressed & SCE_CTRL_DOWN && selected + 1 < ntvs && selected < 5)
            selected++;
        if (g_plugin_ok && !busy) {
            if ((pressed & SCE_CTRL_CROSS) && ntvs > 0) {
                pthread_mutex_lock(&g_lock);
                g_target = g_tvs[selected];
                pthread_mutex_unlock(&g_lock);
                atomic_store(&g_job, JOB_CAST);
            } else if ((pressed & SCE_CTRL_CIRCLE) && g_mirroring) {
                pthread_mutex_lock(&g_lock);
                g_target = g_active;
                pthread_mutex_unlock(&g_lock);
                atomic_store(&g_job, JOB_STOP);
            } else if (pressed & SCE_CTRL_TRIANGLE) {
                atomic_store(&g_job, JOB_SCAN);
            }
        }
        if (g_plugin_ok && (pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER))) {
            char q[32];
            int p = g_preset + ((pressed & SCE_CTRL_RTRIGGER) ? 1 : -1);
            if (p < 0)
                p = 0;
            if (p >= MIRROR_PRESETS)
                p = MIRROR_PRESETS - 1;
            snprintf(q, sizeof(q), "preset=%d", p);
            plugin_control(q);
        }
        if (g_plugin_ok && (pressed & SCE_CTRL_SQUARE))
            plugin_control(g_paused ? "paused=0" : "paused=1");

        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        draw(selected);
    }

    /* The plugin keeps mirroring after we exit. */
    vita2d_wait_rendering_done();
    vita2d_fini();
    vita_platform_shutdown();
    sceKernelExitProcess(0);
    return 0;
}
