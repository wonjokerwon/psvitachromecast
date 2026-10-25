/* Simulated player for the host build. It really opens the media URL through
 * the same HTTP range source the Vita uses (so URL, redirect and range
 * handling are exercised), then plays a virtual timeline. */
#include "../platform/player.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/http.h"
#include "../core/log.h"
#include "../platform/platform.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static player_status g_st;
static double g_base_time;
static uint64_t g_base_tick;
static unsigned g_gen;
static int g_autoplay;
static double g_start;
static double g_duration_hint;

typedef struct load_job {
    unsigned gen;
    char *url;
} load_job;

static double now_s(void)
{
    return (double)plat_ticks_ms() / 1000.0;
}

/* Caller holds g_lock. */
static void update_time(void)
{
    if (g_st.state != PLAYER_PLAYING)
        return;
    double t = g_base_time + (now_s() - (double)g_base_tick / 1000.0);
    if (g_st.duration > 0 && t >= g_st.duration) {
        t = g_st.duration;
        g_st.state = PLAYER_IDLE;
        g_st.idle_reason = PLAYER_IDLE_FINISHED;
    }
    g_st.current_time = t;
}

static void set_base(double t)
{
    g_base_time = t;
    g_base_tick = plat_ticks_ms();
    g_st.current_time = t;
}

static void *load_main(void *arg)
{
    load_job *job = arg;
    char err[128] = "";
    double duration = 0;
    int ok = 0;

    const char *delay = getenv("VITACAST_SIM_LOAD_DELAY_MS");
    if (delay)
        plat_sleep_ms(atoi(delay));

    http_source *hs = http_source_open(job->url, err, sizeof(err));
    if (hs) {
        /* Probe like a demuxer would: the head and the tail of the file. */
        uint8_t buf[4096];
        int64_t size = http_source_size(hs);
        int a = http_source_read(hs, buf, 0, sizeof(buf));
        int b = http_source_read(hs, buf, size > 4096 ? (uint64_t)(size - 4096) : 0, sizeof(buf));
        if (a > 0 && b > 0) {
            ok = 1;
            LOGI("sim: opened %lld bytes (%s)", (long long)size, http_source_content_type(hs));
        } else {
            snprintf(err, sizeof(err), "read failed");
        }
        http_source_close(hs);
    }

    const char *env = getenv("VITACAST_SIM_DURATION");
    duration = env ? atof(env) : 30.0;

    pthread_mutex_lock(&g_lock);
    if (job->gen == g_gen && g_st.state == PLAYER_LOADING) {
        if (ok) {
            g_st.duration = g_duration_hint > 0 ? g_duration_hint : duration;
            g_st.state = g_autoplay ? PLAYER_PLAYING : PLAYER_PAUSED;
            set_base(g_start);
        } else {
            g_st.state = PLAYER_IDLE;
            g_st.idle_reason = PLAYER_IDLE_ERROR;
            snprintf(g_st.error, sizeof(g_st.error), "%s", err);
        }
    }
    pthread_mutex_unlock(&g_lock);
    free(job->url);
    free(job);
    return NULL;
}

int player_init(void)
{
    memset(&g_st, 0, sizeof(g_st));
    return 0;
}

void player_shutdown(void)
{
}

unsigned player_load(const player_media *m, char *err, int errcap)
{
    load_job *job = calloc(1, sizeof(*job));
    if (!job || !(job->url = strdup(m->url))) {
        free(job);
        snprintf(err, (size_t)errcap, "out of memory");
        return 0;
    }
    pthread_mutex_lock(&g_lock);
    unsigned gen = ++g_gen;
    memset(&g_st, 0, sizeof(g_st));
    g_st.generation = gen;
    g_st.state = PLAYER_LOADING;
    g_autoplay = m->autoplay;
    g_start = m->start_time;
    g_duration_hint = m->duration_hint;
    job->gen = gen;
    pthread_mutex_unlock(&g_lock);

    pthread_t t;
    if (pthread_create(&t, NULL, load_main, job) != 0) {
        free(job->url);
        free(job);
        snprintf(err, (size_t)errcap, "cannot start loader");
        return 0;
    }
    pthread_detach(t);
    LOGI("sim: load %s", m->url);
    return gen;
}

void player_play(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_st.state == PLAYER_PAUSED) {
        g_st.state = PLAYER_PLAYING;
        set_base(g_st.current_time);
    }
    pthread_mutex_unlock(&g_lock);
}

void player_pause(void)
{
    pthread_mutex_lock(&g_lock);
    update_time();
    if (g_st.state == PLAYER_PLAYING)
        g_st.state = PLAYER_PAUSED;
    pthread_mutex_unlock(&g_lock);
}

void player_seek(double seconds)
{
    pthread_mutex_lock(&g_lock);
    if (g_st.state == PLAYER_PLAYING || g_st.state == PLAYER_PAUSED) {
        if (g_st.duration > 0 && seconds > g_st.duration)
            seconds = g_st.duration;
        set_base(seconds);
    } else if (g_st.state == PLAYER_LOADING) {
        g_start = seconds;
    }
    pthread_mutex_unlock(&g_lock);
}

void player_stop(void)
{
    pthread_mutex_lock(&g_lock);
    update_time();
    if (g_st.state != PLAYER_IDLE) {
        g_st.state = PLAYER_IDLE;
        g_st.idle_reason = PLAYER_IDLE_CANCELLED;
    }
    pthread_mutex_unlock(&g_lock);
}

void player_set_volume(double level, int muted)
{
    LOGI("sim: volume %.2f%s", level, muted ? " (muted)" : "");
}

void player_get_status(player_status *out)
{
    pthread_mutex_lock(&g_lock);
    update_time();
    *out = g_st;
    pthread_mutex_unlock(&g_lock);
}
