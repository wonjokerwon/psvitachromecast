#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../platform/platform.h"

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_ring[LOG_RING_LINES][LOG_LINE_MAX];
static int g_ring_head;
static int g_ring_count;

void log_printf(const char *fmt, ...)
{
    char line[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&g_log_lock);
    plat_log_write(line);
    snprintf(g_ring[g_ring_head], LOG_LINE_MAX, "%.*s", LOG_LINE_MAX - 1, line);
    g_ring_head = (g_ring_head + 1) % LOG_RING_LINES;
    if (g_ring_count < LOG_RING_LINES)
        g_ring_count++;
    pthread_mutex_unlock(&g_log_lock);
}

int log_recent(char lines[][LOG_LINE_MAX], int max)
{
    pthread_mutex_lock(&g_log_lock);
    int n = g_ring_count < max ? g_ring_count : max;
    int start = (g_ring_head - n + LOG_RING_LINES) % LOG_RING_LINES;
    for (int i = 0; i < n; i++)
        memcpy(lines[i], g_ring[(start + i) % LOG_RING_LINES], LOG_LINE_MAX);
    pthread_mutex_unlock(&g_log_lock);
    return n;
}
