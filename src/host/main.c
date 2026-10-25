/* vitacast-host: the VitaCast receiver running on a desktop OS with a
 * simulated player. Used to develop and test the Cast protocol without a
 * Vita; `--fetch` exercises the HTTP range source on its own. */
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../core/cast_sender.h"
#include "../core/config.h"
#include "../core/http.h"
#include "../core/log.h"
#include "../core/mdns.h"
#include "../core/receiver.h"
#include "../core/tls.h"
#include "../platform/platform.h"
#include "../platform/player.h"
#include "host.h"

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  --name NAME        friendly name (default: from config, else \"VitaCast Host\")\n"
            "  --data DIR         data directory (default ./vitacast-data)\n"
            "  --ip ADDR          IPv4 address to advertise\n"
            "  --port N           Cast port (default 8009)\n"
            "  --http-port N      device info port (default 8008, 0 = off)\n"
            "  --https-port N     device info TLS port (default 8443, 0 = off)\n"
            "  --no-mdns          do not advertise via mDNS\n"
            "  --quiet            no log output\n"
            "  --fetch URL FILE   download URL through the range cache and exit\n"
            "  --discover         list Cast devices with a screen and exit\n"
            "  --cast-url HOST[:PORT] URL   show URL on a Cast device (via DashCast)\n"
            "  --stop-cast HOST[:PORT]      stop the app running on a Cast device\n",
            argv0);
}

/* --fetch watchdog: if no progress for 30s, describe where everything is
 * stuck and exit, so test logs show the cause instead of a bare timeout. */
static http_source *g_fetch_hs;
static atomic_llong g_fetch_pos;
static atomic_int g_fetch_done;

static void *fetch_watchdog(void *arg)
{
    (void)arg;
    long long last = -1;
    int still = 0;
    while (!g_fetch_done) {
        plat_sleep_ms(1000);
        long long pos = g_fetch_pos;
        still = pos == last ? still + 1 : 0;
        last = pos;
        if (still >= 30) {
            char state[1024];
            http_source_debug(g_fetch_hs, state, sizeof(state));
            fprintf(stderr, "watchdog: no progress for 30s at offset %lld\nwatchdog: %s\n", pos, state);
            fflush(stderr);
            _exit(3);
        }
    }
    return NULL;
}

static int do_fetch(const char *url, const char *out_path)
{
    char err[160];
    http_source *hs = http_source_open(url, err, sizeof(err));
    if (!hs) {
        fprintf(stderr, "open failed: %s\n", err);
        return 1;
    }
    FILE *f = fopen(out_path, "wb");
    if (!f) {
        http_source_close(hs);
        return 1;
    }
    int64_t size = http_source_size(hs);
    fprintf(stderr, "fetch: opened, %lld bytes\n", (long long)size);
    pthread_t wd;
    g_fetch_hs = hs;
    pthread_create(&wd, NULL, fetch_watchdog, NULL);
    /* Odd, varying read sizes plus a few backward jumps, like a demuxer. */
    static uint8_t buf[300000];
    uint64_t pos = 0;
    uint32_t step = 1000;
    int rc = 0;
    while ((int64_t)pos < size) {
        uint32_t want = step;
        int n = http_source_read(hs, buf, pos, want);
        if (n <= 0) {
            fprintf(stderr, "read failed at %llu\n", (unsigned long long)pos);
            rc = 1;
            break;
        }
        if (pos > 600000 && pos < 700000) {
            /* re-read an earlier range and compare */
            static uint8_t again[4096];
            int m = http_source_read(hs, again, pos - 500000, sizeof(again));
            if (m <= 0) {
                rc = 1;
                break;
            }
        }
        fwrite(buf, 1, (size_t)n, f);
        pos += (uint64_t)n;
        g_fetch_pos = (long long)pos;
        step = step * 3 % 299989 + 17;
    }
    fclose(f);
    fprintf(stderr, "fetch: read done (rc=%d), closing\n", rc);
    g_fetch_pos = -2; /* closing counts as progress */
    http_source_close(hs);
    g_fetch_done = 1;
    pthread_join(wd, NULL);
    fprintf(stderr, "fetch: closed, exiting\n");
    if (rc == 0)
        printf("fetched %lld bytes\n", (long long)size);
    return rc;
}

/* Splits "host[:port]" (default port 8009). */
static void split_host(const char *arg, char *host, size_t cap, uint16_t *port)
{
    snprintf(host, cap, "%s", arg);
    *port = 8009;
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = '\0';
        *port = (uint16_t)atoi(colon + 1);
    }
}

static int do_discover(void)
{
    cast_device devs[16];
    char ip[16];
    plat_local_ipv4(ip);
    int n = mdns_browse(ip, 3000, devs, 16);
    for (int i = 0; i < n; i++)
        printf("%s\t%s:%u\t%s\t%s\n", devs[i].name, devs[i].ip, (unsigned)devs[i].port,
               devs[i].model, devs[i].id);
    return n < 0 ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *name = NULL;
    const char *fetch_url = NULL, *fetch_out = NULL;
    const char *cast_target = NULL, *cast_url = NULL, *stop_target = NULL;
    int discover = 0;
    int port = 8009, http_port = 8008, https_port = 8443, mdns = 1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(a, "--name") == 0 && v) {
            name = v;
            i++;
        } else if (strcmp(a, "--data") == 0 && v) {
            host_set_data_dir(v);
            i++;
        } else if (strcmp(a, "--ip") == 0 && v) {
            host_set_ip(v);
            i++;
        } else if (strcmp(a, "--port") == 0 && v) {
            port = atoi(v);
            i++;
        } else if (strcmp(a, "--http-port") == 0 && v) {
            http_port = atoi(v);
            i++;
        } else if (strcmp(a, "--https-port") == 0 && v) {
            https_port = atoi(v);
            i++;
        } else if (strcmp(a, "--no-mdns") == 0) {
            mdns = 0;
        } else if (strcmp(a, "--quiet") == 0) {
            host_set_quiet(1);
        } else if (strcmp(a, "--discover") == 0) {
            discover = 1;
        } else if (strcmp(a, "--cast-url") == 0 && v && i + 2 < argc) {
            cast_target = v;
            cast_url = argv[i + 2];
            i += 2;
        } else if (strcmp(a, "--stop-cast") == 0 && v) {
            stop_target = v;
            i++;
        } else if (strcmp(a, "--fetch") == 0 && v && i + 2 < argc) {
            fetch_url = v;
            fetch_out = argv[i + 2];
            i += 2;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    mkdir(plat_data_dir(), 0755);

    vc_config cfg;
    config_load(plat_data_dir(), "VitaCast Host", &cfg);
    if (name)
        snprintf(cfg.name, sizeof(cfg.name), "%s", name);

    /* Commands that only act as a client skip the device key. */
    int client_only = fetch_url || discover || cast_target || stop_target;
    if ((client_only ? tls_init_client() : tls_init(plat_data_dir(), cfg.uuid_hex)) != 0) {
        fprintf(stderr, "TLS init failed\n");
        return 1;
    }
    if (fetch_url)
        return do_fetch(fetch_url, fetch_out);
    if (discover)
        return do_discover();
    if (cast_target || stop_target) {
        char host[256], err[160] = "";
        uint16_t p;
        split_host(cast_target ? cast_target : stop_target, host, sizeof(host), &p);
        int rc = cast_target ? cast_open_url(host, p, cast_url, err, sizeof(err))
                             : cast_stop_app(host, p, err, sizeof(err));
        if (rc != 0)
            fprintf(stderr, "error: %s\n", err);
        return rc ? 1 : 0;
    }

    char ip[16];
    plat_local_ipv4(ip);
    player_init();

    receiver_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.cfg = &cfg;
    opts.ip = ip;
    opts.model_name = "VitaCast Host";
    opts.cast_port = (uint16_t)port;
    opts.http_port = (uint16_t)http_port;
    opts.https_port = (uint16_t)https_port;
    opts.enable_mdns = mdns;
    if (receiver_start(&opts) != 0)
        return 1;
    LOGI("\"%s\" ready (uuid %s)", cfg.name, cfg.uuid);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    while (!g_stop) {
        receiver_tick();
        plat_sleep_ms(100);
    }
    LOGI("Shutting down");
    receiver_stop();
    player_shutdown();
    tls_shutdown();
    return 0;
}
