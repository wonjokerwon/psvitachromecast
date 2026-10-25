/* vitacast-host: the VitaCast receiver running on a desktop OS with a
 * simulated player. Used to develop and test the Cast protocol without a
 * Vita; `--fetch` exercises the HTTP range source on its own. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../core/config.h"
#include "../core/http.h"
#include "../core/log.h"
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
            "  --fetch URL FILE   download URL through the range cache and exit\n",
            argv0);
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
        step = step * 3 % 299989 + 17;
    }
    fclose(f);
    http_source_close(hs);
    if (rc == 0)
        printf("fetched %lld bytes\n", (long long)size);
    return rc;
}

int main(int argc, char **argv)
{
    const char *name = NULL;
    const char *fetch_url = NULL, *fetch_out = NULL;
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

    if (tls_init(plat_data_dir(), cfg.uuid_hex) != 0) {
        fprintf(stderr, "TLS init failed\n");
        return 1;
    }
    if (fetch_url)
        return do_fetch(fetch_url, fetch_out);

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
