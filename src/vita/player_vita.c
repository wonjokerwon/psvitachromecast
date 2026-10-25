/* Media player backend for the PS Vita, built on the system's hardware
 * accelerated sceAvPlayer (MP4 / H.264 / AAC). Media bytes come from the
 * HTTP range cache through AvPlayer's file replacement callbacks. */
#include "../platform/player.h"

#include <malloc.h>
#include <math.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <psp2/audioout.h>
#include <psp2/avplayer.h>
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "../core/http.h"
#include "../core/log.h"
#include "../platform/platform.h"
#include "vita.h"

/* AvPlayer state events (not named in the vitasdk header). */
#define AVP_EVENT_STOP 0x01
#define AVP_EVENT_READY 0x02
#define AVP_EVENT_PLAY 0x03
#define AVP_EVENT_PAUSE 0x04
#define AVP_EVENT_BUFFERING 0x05

#define START_TIMEOUT_MS 30000
#define AUDIO_MAX_BYTES (8192 * 2 * 2)

typedef struct load_request {
    int pending;
    unsigned gen;
    char *url;
    double start;
    double duration_hint;
    int autoplay;
} load_request;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;    /* status + requests */
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_av_lock = PTHREAD_MUTEX_INITIALIZER; /* AvPlayer handle */

static load_request g_req;
static int g_close_req;
static atomic_int g_quit;
static unsigned g_gen_counter;
static pthread_t g_worker, g_audio;
static int g_threads;

/* current playback */
static SceAvPlayerHandle g_h;
static http_source *g_src;
static player_status g_st;
static unsigned g_cur_gen;
static int g_started;
static int g_user_paused;
static int g_autoplay;
static double g_start_at;
static double g_duration_hint;
static double g_seek_target;
static uint64_t g_seek_until;
static uint64_t g_open_tick;
static atomic_int g_last_event;
static atomic_int g_ev_ready, g_ev_play, g_ev_stop;
static atomic_int g_read_error;

/* volume */
static atomic_int g_vol_dirty = 1;
static atomic_int g_vol_value = SCE_AUDIO_VOLUME_0DB;

/* video */
static vita2d_texture g_tex;
static uint8_t *g_frame;
static int g_fw, g_fh;
static int g_has_video;
static atomic_uint g_video_epoch, g_drawn_epoch;

/* --- AvPlayer callbacks --- */

static void *av_alloc(void *arg, uint32_t alignment, uint32_t size)
{
    (void)arg;
    return memalign(alignment < 16 ? 16 : alignment, size);
}

static void av_free(void *arg, void *ptr)
{
    (void)arg;
    free(ptr);
}

static void *av_gpu_alloc(void *arg, uint32_t alignment, uint32_t size)
{
    SceKernelAllocMemBlockOpt opt;
    void *base = NULL;
    (void)arg;
    if (alignment < 0x40000)
        alignment = 0x40000;
    size = (size + alignment - 1) & ~(alignment - 1);
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
    opt.alignment = alignment;
    SceUID uid = sceKernelAllocMemBlock("vitacast_frame", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                        size, &opt);
    if (uid < 0)
        return NULL;
    sceKernelGetMemBlockBase(uid, &base);
    if (sceGxmMapMemory(base, size,
                        (SceGxmMemoryAttribFlags)(SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE)) < 0) {
        sceKernelFreeMemBlock(uid);
        return NULL;
    }
    return base;
}

static void av_gpu_free(void *arg, void *ptr)
{
    (void)arg;
    if (!ptr)
        return;
    SceUID uid = sceKernelFindMemBlockByAddr(ptr, 0);
    sceGxmUnmapMemory(ptr);
    if (uid >= 0)
        sceKernelFreeMemBlock(uid);
}

static int file_open(void *p, const char *filename)
{
    (void)p;
    (void)filename;
    return 0;
}

static int file_close(void *p)
{
    (void)p;
    return 0;
}

static int file_read(void *p, uint8_t *buffer, uint64_t position, uint32_t length)
{
    http_source *src = p;
    int n = http_source_read(src, buffer, position, length);
    if (n < 0)
        g_read_error = 1;
    return n;
}

static uint64_t file_size(void *p)
{
    return (uint64_t)http_source_size((http_source *)p);
}

static void on_event(void *p, int32_t event, int32_t source, void *data)
{
    (void)p;
    (void)source;
    (void)data;
    g_last_event = event;
    if (event == AVP_EVENT_READY)
        g_ev_ready = 1;
    else if (event == AVP_EVENT_PLAY)
        g_ev_play = 1;
    else if (event == AVP_EVENT_STOP)
        g_ev_stop = 1;
}

/* --- helpers --- */

static void set_error(unsigned gen, const char *msg)
{
    pthread_mutex_lock(&g_lock);
    if (g_st.generation == gen && g_st.state != PLAYER_IDLE) {
        g_st.state = PLAYER_IDLE;
        g_st.idle_reason = PLAYER_IDLE_ERROR;
        snprintf(g_st.error, sizeof(g_st.error), "%s", msg);
    }
    pthread_mutex_unlock(&g_lock);
}

static void close_current(void)
{
    http_source *src;
    SceAvPlayerHandle h;

    pthread_mutex_lock(&g_av_lock);
    src = g_src;
    h = g_h;
    g_h = 0;
    g_src = NULL;
    g_frame = NULL;
    g_has_video = 0;
    unsigned target = ++g_video_epoch;
    pthread_mutex_unlock(&g_av_lock);

    if (src)
        http_source_abort(src);
    /* Wait until the GPU no longer samples the last frame before the
     * player frees its frame buffers. */
    uint64_t t0 = plat_ticks_ms();
    while (g_drawn_epoch != target && plat_ticks_ms() - t0 < 300)
        plat_sleep_ms(5);
    if (h) {
        sceAvPlayerStop(h);
        sceAvPlayerClose(h);
    }
    if (src)
        http_source_close(src);
    g_started = 0;
    g_ev_ready = g_ev_play = g_ev_stop = 0;
    g_last_event = 0;
    g_read_error = 0;
}

static int unsupported_type(const char *url, const char *type)
{
    static const char *const bad_types[] = {
        "application/x-mpegurl", "application/vnd.apple.mpegurl", "audio/mpegurl",
        "application/dash+xml", "video/webm", "audio/webm", "application/vnd.ms-sstr+xml", NULL,
    };
    static const char *const bad_ext[] = { ".m3u8", ".mpd", ".webm", ".ism", ".mkv", NULL };
    for (int i = 0; type && bad_types[i]; i++)
        if (strncasecmp(type, bad_types[i], strlen(bad_types[i])) == 0)
            return 1;
    const char *q = strchr(url, '?');
    size_t len = q ? (size_t)(q - url) : strlen(url);
    for (int i = 0; bad_ext[i]; i++) {
        size_t n = strlen(bad_ext[i]);
        if (len >= n && strncasecmp(url + len - n, bad_ext[i], n) == 0)
            return 1;
    }
    return 0;
}

static void open_media(load_request *rq)
{
    char err[160] = "";
    http_source *src = http_source_open(rq->url, err, sizeof(err));
    if (!src) {
        LOGI("player: %s", err);
        set_error(rq->gen, err);
        return;
    }

    SceAvPlayerInitData init;
    memset(&init, 0, sizeof(init));
    init.memoryReplacement.allocate = av_alloc;
    init.memoryReplacement.deallocate = av_free;
    init.memoryReplacement.allocateTexture = av_gpu_alloc;
    init.memoryReplacement.deallocateTexture = av_gpu_free;
    init.fileReplacement.objectPointer = src;
    init.fileReplacement.open = file_open;
    init.fileReplacement.close = file_close;
    init.fileReplacement.readOffset = file_read;
    init.fileReplacement.size = file_size;
    init.eventReplacement.eventCallback = on_event;
    init.basePriority = 0xA0;
    init.numOutputVideoFrameBuffers = 2;
    init.autoStart = 1;
    init.debugLevel = 0;

    g_ev_ready = g_ev_play = g_ev_stop = 0;
    g_last_event = 0;
    g_read_error = 0;
    g_started = 0;

    SceAvPlayerHandle h = sceAvPlayerInit(&init);
    if (h <= 0) {
        LOGI("player: sceAvPlayerInit failed (0x%08x)", (unsigned)h);
        http_source_close(src);
        set_error(rq->gen, "player init failed");
        return;
    }
    pthread_mutex_lock(&g_av_lock);
    g_h = h;
    g_src = src;
    pthread_mutex_unlock(&g_av_lock);

    pthread_mutex_lock(&g_lock);
    g_cur_gen = rq->gen;
    g_autoplay = rq->autoplay;
    g_start_at = rq->start;
    g_duration_hint = rq->duration_hint;
    g_user_paused = 0;
    g_open_tick = plat_ticks_ms();
    pthread_mutex_unlock(&g_lock);

    /* The name only has to look like a file; data comes from file_read. */
    int rc = sceAvPlayerAddSource(h, "ux0:data/VitaCast/stream.mp4");
    if (rc < 0) {
        LOGI("player: AddSource failed (0x%08x)", (unsigned)rc);
        set_error(rq->gen, "unsupported media");
        close_current();
    }
}

static double query_duration(SceAvPlayerHandle h)
{
    double best = 0;
    for (uint32_t id = 0; id < 4; id++) {
        SceAvPlayerStreamInfo info;
        memset(&info, 0, sizeof(info));
        if (sceAvPlayerGetStreamInfo(h, id, &info) < 0)
            continue;
        double d = (double)info.duration / 1000.0;
        if (d > best)
            best = d;
    }
    return best;
}

/* Runs every ~100ms on the worker thread while something is loaded. */
static void monitor(void)
{
    pthread_mutex_lock(&g_av_lock);
    SceAvPlayerHandle h = g_h;
    if (!h) {
        pthread_mutex_unlock(&g_av_lock);
        return;
    }
    int active = sceAvPlayerIsActive(h);
    double cur = (double)sceAvPlayerCurrentTime(h) / 1000.0;
    pthread_mutex_unlock(&g_av_lock);

    uint64_t now = plat_ticks_ms();
    pthread_mutex_lock(&g_lock);
    unsigned gen = g_cur_gen;
    if (g_st.generation != gen || g_st.state == PLAYER_IDLE) {
        pthread_mutex_unlock(&g_lock);
        return;
    }

    if (!g_started && (g_ev_play || g_ev_ready || cur > 0)) {
        g_started = 1;
        double start = g_start_at;
        int autoplay = g_autoplay;
        pthread_mutex_unlock(&g_lock);
        pthread_mutex_lock(&g_av_lock);
        double dur = g_h ? query_duration(g_h) : 0;
        if (g_h && start > 0.5)
            sceAvPlayerJumpToTime(g_h, (uint64_t)(start * 1000.0));
        if (g_h && !autoplay)
            sceAvPlayerPause(g_h);
        pthread_mutex_unlock(&g_av_lock);
        pthread_mutex_lock(&g_lock);
        g_st.duration = dur > 0 ? dur : g_duration_hint;
        if (start > 0.5) {
            g_seek_target = start;
            g_seek_until = now + 3000;
        }
        if (!autoplay)
            g_user_paused = 1;
        LOGI("player: started (duration %.1fs)", g_st.duration);
    }

    if (g_read_error) {
        pthread_mutex_unlock(&g_lock);
        set_error(gen, "network error while streaming");
        return;
    }
    if (!g_started) {
        if (now - g_open_tick > START_TIMEOUT_MS ||
            (!active && now - g_open_tick > 3000)) {
            pthread_mutex_unlock(&g_lock);
            set_error(gen, "unsupported or unreadable media (MP4/H.264/AAC required)");
            return;
        }
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (!active || (g_ev_stop && !g_user_paused)) {
        g_st.state = PLAYER_IDLE;
        g_st.idle_reason = PLAYER_IDLE_FINISHED;
        if (g_st.duration > 0)
            g_st.current_time = g_st.duration;
        g_close_req = 1;
        pthread_mutex_unlock(&g_lock);
        return;
    }

    /* AvPlayer reports the old position until the jump completes. */
    if (g_seek_until && now < g_seek_until && fabs(cur - g_seek_target) > 1.0)
        cur = g_seek_target;
    else
        g_seek_until = 0;
    g_st.current_time = cur;
    if (g_user_paused)
        g_st.state = PLAYER_PAUSED;
    else if (g_last_event == AVP_EVENT_BUFFERING)
        g_st.state = PLAYER_BUFFERING;
    else
        g_st.state = PLAYER_PLAYING;
    pthread_mutex_unlock(&g_lock);
}

static void *worker_main(void *arg)
{
    (void)arg;
    while (!g_quit) {
        pthread_mutex_lock(&g_lock);
        if (!g_req.pending && !g_close_req) {
            pthread_mutex_unlock(&g_lock);
            monitor();
            plat_sleep_ms(100);
            continue;
        }
        if (g_req.pending) {
            load_request rq = g_req;
            g_req.pending = 0;
            g_req.url = NULL;
            g_close_req = 0;
            pthread_mutex_unlock(&g_lock);
            close_current();
            open_media(&rq);
            free(rq.url);
            continue;
        }
        g_close_req = 0;
        pthread_mutex_unlock(&g_lock);
        close_current();
    }
    close_current();
    return NULL;
}

static void *audio_main(void *arg)
{
    static uint8_t pcm[AUDIO_MAX_BYTES];
    int grain = 1024, rate = 48000, channels = 2;
    (void)arg;

    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, grain, rate, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) {
        LOGI("player: cannot open audio port (0x%08x)", (unsigned)port);
        return NULL;
    }
    while (!g_quit) {
        SceAvPlayerFrameInfo fi;
        int got = 0;
        uint32_t size = 0;
        pthread_mutex_lock(&g_av_lock);
        if (g_h) {
            memset(&fi, 0, sizeof(fi));
            if (sceAvPlayerGetAudioData(g_h, &fi) && fi.pData) {
                size = fi.details.audio.size;
                if (size > sizeof(pcm))
                    size = sizeof(pcm);
                memcpy(pcm, fi.pData, size);
                got = 1;
            }
        }
        pthread_mutex_unlock(&g_av_lock);
        if (!got || size == 0) {
            sceKernelDelayThread(2000);
            continue;
        }
        int ch = fi.details.audio.channelCount == 1 ? 1 : 2;
        int r = fi.details.audio.sampleRate ? (int)fi.details.audio.sampleRate : 48000;
        int g = (int)(size / (uint32_t)(ch * 2));
        if (g != grain || r != rate || ch != channels) {
            if (sceAudioOutSetConfig(port, (SceSize)g, r,
                                     ch == 1 ? SCE_AUDIO_OUT_MODE_MONO : SCE_AUDIO_OUT_MODE_STEREO) < 0) {
                continue;
            }
            grain = g;
            rate = r;
            channels = ch;
            g_vol_dirty = 1;
        }
        if (g_vol_dirty) {
            int vols[2] = { g_vol_value, g_vol_value };
            sceAudioOutSetVolume(port, (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), vols);
            g_vol_dirty = 0;
        }
        sceAudioOutOutput(port, pcm);
    }
    sceAudioOutReleasePort(port);
    return NULL;
}

/* --- public API --- */

static int spawn(pthread_t *t, void *(*fn)(void *))
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    int rc = pthread_create(t, &attr, fn, NULL);
    pthread_attr_destroy(&attr);
    return rc;
}

int player_init(void)
{
    memset(&g_st, 0, sizeof(g_st));
    memset(&g_tex, 0, sizeof(g_tex));
    g_quit = 0;
    if (spawn(&g_worker, worker_main) != 0)
        return -1;
    if (spawn(&g_audio, audio_main) != 0) {
        g_quit = 1;
        pthread_join(g_worker, NULL);
        return -1;
    }
    g_threads = 1;
    return 0;
}

void player_shutdown(void)
{
    if (!g_threads)
        return;
    g_quit = 1;
    pthread_join(g_worker, NULL);
    pthread_join(g_audio, NULL);
    g_threads = 0;
    free(g_req.url);
    g_req.url = NULL;
}

unsigned player_load(const player_media *m, char *err, int errcap)
{
    if (unsupported_type(m->url, m->content_type)) {
        snprintf(err, (size_t)errcap, "format not supported on PS Vita (use MP4/H.264/AAC)");
        return 0;
    }
    char *url = strdup(m->url);
    if (!url) {
        snprintf(err, (size_t)errcap, "out of memory");
        return 0;
    }
    pthread_mutex_lock(&g_lock);
    unsigned gen = ++g_gen_counter;
    free(g_req.url);
    g_req.pending = 1;
    g_req.gen = gen;
    g_req.url = url;
    g_req.start = m->start_time;
    g_req.duration_hint = m->duration_hint;
    g_req.autoplay = m->autoplay;
    memset(&g_st, 0, sizeof(g_st));
    g_st.generation = gen;
    g_st.state = PLAYER_LOADING;
    g_st.duration = m->duration_hint;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
    return gen;
}

void player_play(void)
{
    int resume = 0;
    pthread_mutex_lock(&g_lock);
    if (g_st.state == PLAYER_PAUSED) {
        g_user_paused = 0;
        g_st.state = PLAYER_PLAYING;
        resume = 1;
    } else if (g_st.state == PLAYER_LOADING) {
        g_req.autoplay = 1;
        g_autoplay = 1;
    }
    pthread_mutex_unlock(&g_lock);
    if (resume) {
        pthread_mutex_lock(&g_av_lock);
        if (g_h)
            sceAvPlayerResume(g_h);
        pthread_mutex_unlock(&g_av_lock);
    }
}

void player_pause(void)
{
    int pause = 0;
    pthread_mutex_lock(&g_lock);
    if (g_st.state == PLAYER_PLAYING || g_st.state == PLAYER_BUFFERING) {
        g_user_paused = 1;
        g_st.state = PLAYER_PAUSED;
        pause = 1;
    } else if (g_st.state == PLAYER_LOADING) {
        g_req.autoplay = 0;
        g_autoplay = 0;
    }
    pthread_mutex_unlock(&g_lock);
    if (pause) {
        pthread_mutex_lock(&g_av_lock);
        if (g_h)
            sceAvPlayerPause(g_h);
        pthread_mutex_unlock(&g_av_lock);
    }
}

void player_seek(double seconds)
{
    int jump = 0;
    if (seconds < 0)
        seconds = 0;
    pthread_mutex_lock(&g_lock);
    if (g_st.state == PLAYER_LOADING) {
        g_req.start = seconds;
        g_start_at = seconds;
    } else if (g_st.state != PLAYER_IDLE && g_started) {
        g_seek_target = seconds;
        g_seek_until = plat_ticks_ms() + 3000;
        g_st.current_time = seconds;
        jump = 1;
    }
    pthread_mutex_unlock(&g_lock);
    if (jump) {
        pthread_mutex_lock(&g_av_lock);
        if (g_h)
            sceAvPlayerJumpToTime(g_h, (uint64_t)(seconds * 1000.0));
        pthread_mutex_unlock(&g_av_lock);
    }
}

void player_stop(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_req.pending) {
        g_req.pending = 0;
        free(g_req.url);
        g_req.url = NULL;
    }
    if (g_st.state != PLAYER_IDLE) {
        g_st.state = PLAYER_IDLE;
        g_st.idle_reason = PLAYER_IDLE_CANCELLED;
    }
    g_close_req = 1;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
}

void player_set_volume(double level, int muted)
{
    if (level < 0)
        level = 0;
    if (level > 1)
        level = 1;
    g_vol_value = muted ? 0 : (int)(level * SCE_AUDIO_VOLUME_0DB);
    g_vol_dirty = 1;
}

void player_get_status(player_status *out)
{
    pthread_mutex_lock(&g_lock);
    *out = g_st;
    pthread_mutex_unlock(&g_lock);
}

/* --- video for the UI --- */

const vita2d_texture *player_vita_video_frame(int *width, int *height)
{
    const vita2d_texture *tex = NULL;
    pthread_mutex_lock(&g_av_lock);
    if (g_h) {
        SceAvPlayerFrameInfo fi;
        memset(&fi, 0, sizeof(fi));
        if (sceAvPlayerGetVideoData(g_h, &fi) && fi.pData && fi.details.video.width &&
            fi.details.video.height) {
            g_frame = fi.pData;
            g_fw = (int)fi.details.video.width;
            g_fh = (int)fi.details.video.height;
            g_has_video = 1;
            sceGxmTextureInitLinear(&g_tex.gxm_tex, g_frame, SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1,
                                    (unsigned)g_fw, (unsigned)g_fh, 0);
            sceGxmTextureSetMinFilter(&g_tex.gxm_tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
            sceGxmTextureSetMagFilter(&g_tex.gxm_tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
        }
    }
    if (g_frame) {
        tex = &g_tex;
        *width = g_fw;
        *height = g_fh;
    }
    pthread_mutex_unlock(&g_av_lock);
    return tex;
}

int player_vita_has_video(void)
{
    return g_has_video;
}

void player_vita_frame_done(void)
{
    unsigned e = g_video_epoch;
    if (e != g_drawn_epoch) {
        vita2d_wait_rendering_done();
        g_drawn_epoch = e;
    }
}
