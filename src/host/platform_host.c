/* Platform layer for Linux/macOS: used for development and automated tests. */
#include "../platform/platform.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "host.h"

static char g_data_dir[512] = "./vitacast-data";
static char g_ip_override[16];
static int g_quiet;

void host_set_data_dir(const char *dir)
{
    snprintf(g_data_dir, sizeof(g_data_dir), "%s", dir);
}

void host_set_ip(const char *ip)
{
    snprintf(g_ip_override, sizeof(g_ip_override), "%s", ip);
}

void host_set_quiet(int quiet)
{
    g_quiet = quiet;
}

uint64_t plat_ticks_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void plat_sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

const char *plat_data_dir(void)
{
    return g_data_dir;
}

int plat_local_ipv4(char out[16])
{
    struct ifaddrs *ifs, *i;
    if (g_ip_override[0]) {
        snprintf(out, 16, "%s", g_ip_override);
        return 0;
    }
    if (getifaddrs(&ifs) != 0)
        return -1;
    int found = -1;
    for (i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET)
            continue;
        if (i->ifa_flags & IFF_LOOPBACK)
            continue;
        if (!(i->ifa_flags & IFF_UP))
            continue;
        struct sockaddr_in *sin = (struct sockaddr_in *)i->ifa_addr;
        if (inet_ntop(AF_INET, &sin->sin_addr, out, 16)) {
            found = 0;
            break;
        }
    }
    freeifaddrs(ifs);
    if (found != 0)
        snprintf(out, 16, "127.0.0.1");
    return 0;
}

int plat_random(void *buf, size_t len)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return -1;
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, (char *)buf + got, len - got);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        got += (size_t)n;
    }
    close(fd);
    return 0;
}

void plat_log_write(const char *line)
{
    if (g_quiet)
        return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    fprintf(stderr, "[%02d:%02d:%02d.%03ld] %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
            ts.tv_nsec / 1000000L, line);
}
