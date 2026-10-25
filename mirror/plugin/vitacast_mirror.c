/* VitaCast Mirror: kernel plugin that streams the PS Vita screen as MJPEG
 * over HTTP (port 8080), for Chromecasts (through DashCast) and browsers.
 *
 * Pipeline per frame, with almost no CPU work:
 *   framebuffer (physical address) --IFTU--> scaled YCbCr 4:2:0
 *   --hardware JPEG encoder--> JPEG --> every connected viewer.
 *
 * The IFTU usage follows xerpi's vita-udcd-uvc, which established the plane
 * state layout and colour conversion setup on real hardware. The encoder's
 * YCbCr input layout is undocumented, so a self-test converts a solid colour
 * with the encoder's own ARGB->YCbCr converter, detects the layout it
 * produced, and configures the IFTU to match (see self_test()). */
#include <psp2kern/ctrl.h>
#include <psp2kern/display.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/jpegenc.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/lowio/iftu.h>
#include <psp2kern/netps.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "mirror_logic.h"

#define MIRROR_VERSION "1.0"
#define LOG_PATH "ux0:data/VitaCast/mirror_log.txt"

#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

#define MAX_CLIENTS 4
#define MAX_STREAMS 2
#define FRAME_SLOTS (MAX_STREAMS + 1)
#define SLOT_SIZE (512 * 1024)
#define YCBCR_SIZE (960 * 544 * 3 / 2) /* largest preset, 256 byte multiple */
#define JPEG_OUT_SIZE (512 * 1024)
#define WORK_SIZE (YCBCR_SIZE + JPEG_OUT_SIZE)

#define MEMBLOCK_KERNEL_CDRAM_RW 0x40408006
#define MEMBLOCK_KERNEL_PHYCONT_RW 0x10208006

#define TEST_R 30
#define TEST_G 60
#define TEST_B 220

#define TOGGLE_BUTTONS (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_SELECT)

typedef struct block {
    SceUID uid;
    uint8_t *va;
    uintptr_t pa;
    SceSize size;
    int cdram;
} block;

/* IFTU plane state with the field layout found by xerpi (vita-udcd-uvc);
 * the vitasdk header's SceIftuPlaneState predates that research. */
typedef struct iftu_plane {
    SceIftuFrameBuf fb;
    unsigned int unk20;
    unsigned int unk24;
    unsigned int unk28;
    unsigned int src_w; /* inverse scale factor, 16.16 fixed point */
    unsigned int src_h;
    unsigned int dst_x;
    unsigned int dst_y;
    unsigned int src_x; /* 8.8 fixed point */
    unsigned int src_y;
    unsigned int crop_top;
    unsigned int crop_bot;
    unsigned int crop_left;
    unsigned int crop_right;
} iftu_plane;
_Static_assert(sizeof(iftu_plane) == sizeof(SceIftuPlaneState), "IFTU plane state size");

typedef struct frame_slot {
    uint32_t seq;
    int len;
    int readers;
} frame_slot;

static struct {
    volatile int running;
    SceUID lock;
    SceUID server_thread;
    SceUID capture_thread;
    int listen_fd;

    block work;  /* YCbCr input followed by the JPEG output */
    block slots; /* published JPEG frames */
    block ctx_mem;
    void *ctx;
    int enc_w, enc_h, enc_ratio, enc_ready;

    /* settings (changed through /control) */
    volatile int preset;
    volatile int ratio;
    volatile int fps;
    volatile int paused;

    /* self-test */
    int selftest_done;
    int encoder_ok;
    yuv_layout layout;
    char selftest_msg[96];

    frame_slot slot[FRAME_SLOTS];
    uint32_t seq;

    volatile int clients;
    volatile int streams;
    int fps_measured;
    int last_frame_bytes;
} g;

/* ------------------------------------------------------------------ */
/* helpers                                                             */

static void log_write(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(buf) - 2)
        n = (int)sizeof(buf) - 2;
    buf[n++] = '\n';
    SceUID fd = ksceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd >= 0) {
        ksceIoWrite(fd, buf, (SceSize)n);
        ksceIoClose(fd);
    }
}

static void lock(void)
{
    ksceKernelLockMutex(g.lock, 1, NULL);
}

static void unlock(void)
{
    ksceKernelUnlockMutex(g.lock, 1);
}

static uint64_t now_us(void)
{
    return (uint64_t)ksceKernelGetSystemTimeWide();
}

static int block_alloc(block *b, const char *name, SceSize size, int want_cdram)
{
    SceKernelAllocMemBlockKernelOpt opt;
    memset(b, 0, sizeof(*b));
    if (want_cdram) {
        b->size = ALIGN(size, 256 * 1024);
        b->uid = ksceKernelAllocMemBlock(name, MEMBLOCK_KERNEL_CDRAM_RW, b->size, NULL);
        b->cdram = 1;
    }
    if (!want_cdram || b->uid < 0) {
        memset(&opt, 0, sizeof(opt));
        opt.size = sizeof(opt);
        opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_PHYCONT | SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
        opt.alignment = 4096;
        b->size = ALIGN(size, 4096);
        b->uid = ksceKernelAllocMemBlock(name, MEMBLOCK_KERNEL_PHYCONT_RW, b->size, &opt);
        b->cdram = 0;
    }
    if (b->uid < 0)
        return b->uid;
    ksceKernelGetMemBlockBase(b->uid, (void **)&b->va);
    ksceKernelVAtoPA(b->va, &b->pa);
    return 0;
}

static void block_free(block *b)
{
    if (b->uid > 0)
        ksceKernelFreeMemBlock(b->uid);
    memset(b, 0, sizeof(*b));
}

/* ------------------------------------------------------------------ */
/* IFTU scale + colour conversion                                      */

static unsigned int iftu_format(unsigned int display_format, unsigned int *bpp)
{
    if (display_format == SCE_DISPLAY_PIXELFORMAT_A8B8G8R8) {
        *bpp = 4;
        return SCE_IFTU_PIXELFORMAT_BGRX8888; /* IFTU names channels the other way round */
    }
    /* Anything else (rare 16 bit framebuffers) */
    *bpp = 2;
    return SCE_IFTU_PIXELFORMAT_BGRA5551;
}

static int iftu_convert(uintptr_t src_pa, unsigned int src_w, unsigned int src_h,
                        unsigned int src_pitch, unsigned int src_format, int dst_w, int dst_h,
                        uintptr_t dst_pa, yuv_layout layout)
{
    /* Full range BT.601 (JPEG) RGB -> YCbCr. Coefficients in S3.9 fixed
     * point (two's complement in 12 bits), offsets in 10 bit units. */
    SceIftuCscParams csc = {
        0, 0x202, 0x3FF, 0, 0x3FF, 0,
        {
            { 0x099, 0x12C, 0x03A },  /* Y  =  0.299 R + 0.587 G + 0.114 B */
            { 0xFAA, 0xF57, 0x100 },  /* Cb = -0.169 R - 0.331 G + 0.500 B */
            { 0x100, 0xF2A, 0xFD7 },  /* Cr =  0.500 R - 0.419 G - 0.081 B */
        },
    };
    if (layout == LAYOUT_NV21) {
        /* Emit Cr before Cb in the interleaved plane. */
        for (int i = 0; i < 3; i++) {
            unsigned int t = csc.ctm[1][i];
            csc.ctm[1][i] = csc.ctm[2][i];
            csc.ctm[2][i] = t;
        }
    }

    SceIftuConvParams params;
    memset(&params, 0, sizeof(params));
    params.size = sizeof(params);
    params.csc_params1 = &csc;
    params.csc_control = 1;
    params.alpha = 0xFF;

    unsigned int bpp;
    unsigned int width_aligned = ALIGN(src_w, 16);
    iftu_plane src;
    memset(&src, 0, sizeof(src));
    src.fb.pixelformat = iftu_format(src_format, &bpp);
    src.fb.width = width_aligned;
    src.fb.height = src_h;
    src.fb.leftover_stride = (src_pitch - width_aligned) * bpp;
    src.fb.paddr0 = (unsigned int)src_pa;
    src.src_w = (src_w * 0x10000) / (unsigned int)dst_w;
    src.src_h = (src_h * 0x10000) / (unsigned int)dst_h;
    /* Values used by vita-udcd-uvc; their exact meaning is unknown. */
    src.dst_x = 245760 / 512 - src_w / 512;
    src.dst_y = 139264 / 512 - src_h / 512;

    SceIftuFrameBuf dst;
    memset(&dst, 0, sizeof(dst));
    unsigned int luma = (unsigned int)(dst_w * dst_h);
    dst.width = (unsigned int)dst_w;
    dst.height = (unsigned int)dst_h;
    dst.paddr0 = (unsigned int)dst_pa;
    if (layout == LAYOUT_I420 || layout == LAYOUT_YV12) {
        unsigned int cb = (unsigned int)dst_pa + luma;
        unsigned int cr = cb + luma / 4;
        dst.pixelformat = SCE_IFTU_PIXELFORMAT_YUV420;
        dst.leftover_align = ((unsigned int)dst_w >> 1) & 0xF;
        dst.paddr1 = layout == LAYOUT_I420 ? cb : cr;
        dst.paddr2 = layout == LAYOUT_I420 ? cr : cb;
    } else {
        dst.pixelformat = SCE_IFTU_PIXELFORMAT_NV12;
        dst.paddr1 = (unsigned int)dst_pa + luma;
    }
    return ksceIftuCsc(&dst, (SceIftuPlaneState *)&src, &params);
}

/* ------------------------------------------------------------------ */
/* JPEG encoder                                                        */

static uint8_t *jpeg_out(void)
{
    return g.work.va + YCBCR_SIZE;
}

static int encoder_setup(int w, int h, int ratio)
{
    if (g.enc_ready) {
        ksceJpegEncoderEnd(g.ctx);
        g.enc_ready = 0;
    }
    int rc = ksceJpegEncoderInit(g.ctx, w, h,
                                 SCE_JPEGENC_PIXELFORMAT_YCBCR420 | SCE_JPEGENC_PIXELFORMAT_CSC_ARGB_YCBCR,
                                 jpeg_out(), JPEG_OUT_SIZE);
    if (rc < 0) {
        log_write("encoder init %dx%d failed: 0x%08X", w, h, rc);
        return rc;
    }
    ksceJpegEncoderSetValidRegion(g.ctx, w, h);
    ksceJpegEncoderSetCompressionRatio(g.ctx, ratio);
    ksceJpegEncoderSetOutputAddr(g.ctx, jpeg_out(), JPEG_OUT_SIZE);
    g.enc_w = w;
    g.enc_h = h;
    g.enc_ratio = ratio;
    g.enc_ready = 1;
    return 0;
}

/* Learns the encoder's YCbCr layout and checks the whole pipeline once. */
static void self_test(void)
{
    block img;
    const int w = 960, h = 544;
    int y, cb, cr;

    g.selftest_done = 1;
    g.layout = LAYOUT_NV12;
    if (block_alloc(&img, "vcm_test", (SceSize)(w * h * 4), g.work.cdram) < 0) {
        snprintf(g.selftest_msg, sizeof(g.selftest_msg), "no memory for self-test");
        log_write("self-test: %s", g.selftest_msg);
        return;
    }
    /* Solid colour in the framebuffer's byte order (R, G, B, A). */
    uint32_t px = (uint32_t)TEST_R | ((uint32_t)TEST_G << 8) | ((uint32_t)TEST_B << 16) | 0xFF000000u;
    uint32_t *p = (uint32_t *)img.va;
    for (int i = 0; i < w * h; i++)
        p[i] = px;
    ksceKernelDcacheCleanRange(img.va, img.size);

    if (encoder_setup(w, h, 128) < 0) {
        snprintf(g.selftest_msg, sizeof(g.selftest_msg), "JPEG encoder unavailable");
        block_free(&img);
        return;
    }

    /* 1. The encoder's own converter shows which layout it expects. */
    memset(g.work.va, 0, YCBCR_SIZE);
    ksceKernelDcacheCleanRange(g.work.va, YCBCR_SIZE);
    int rc = ksceJpegEncoderCsc(g.ctx, g.work.va, img.va, w, SCE_JPEGENC_PIXELFORMAT_ARGB8888);
    ksceKernelDcacheInvalidateRange(g.work.va, YCBCR_SIZE);
    const uint8_t *yuv = g.work.va;
    size_t c = (size_t)w * h;
    log_write("self-test: csc rc=0x%08X Y=%d c0=%d c1=%d q0=%d", rc, yuv[0], yuv[c], yuv[c + 1],
              yuv[c + c / 4]);
    rgb_to_ycbcr(TEST_R, TEST_G, TEST_B, &y, &cb, &cr);
    yuv_layout layout = rc >= 0 ? detect_layout(yuv, w, h, cb, cr) : LAYOUT_UNKNOWN;
    if (layout == LAYOUT_UNKNOWN && rc >= 0) {
        /* Maybe the encoder reads the bytes as B, G, R: then the chroma
         * values swap roles but the layout logic is the same. */
        int y2, cb2, cr2;
        rgb_to_ycbcr(TEST_B, TEST_G, TEST_R, &y2, &cb2, &cr2);
        layout = detect_layout(yuv, w, h, cb2, cr2);
        if (layout != LAYOUT_UNKNOWN)
            log_write("self-test: encoder reads ARGB8888 as BGR");
    }
    if (layout != LAYOUT_UNKNOWN)
        g.layout = layout;

    /* 2. Run the real pipeline on the test image and check the IFTU
     *    output matches what the encoder produced itself. */
    int dw = 480, dh = 272;
    int ref_y = yuv[0], ref_c0 = yuv[c], ref_c1 = yuv[c + 1];
    if (g.layout == LAYOUT_I420 || g.layout == LAYOUT_YV12)
        ref_c1 = yuv[c + c / 4];
    memset(g.work.va, 0, YCBCR_SIZE);
    ksceKernelDcacheCleanRange(g.work.va, YCBCR_SIZE);
    rc = iftu_convert(img.pa, (unsigned)w, (unsigned)h, (unsigned)w, SCE_DISPLAY_PIXELFORMAT_A8B8G8R8,
                      dw, dh, g.work.pa, g.layout);
    ksceKernelDcacheInvalidateRange(g.work.va, YCBCR_SIZE);
    /* Sample the centre pixel, away from any edge artefacts. */
    size_t dc = (size_t)dw * dh;
    size_t cx = (size_t)dw / 2, cy = (size_t)dh / 2;
    int got_y = yuv[cy * dw + cx];
    int got_c0, got_c1;
    if (g.layout == LAYOUT_I420 || g.layout == LAYOUT_YV12) {
        size_t off = (cy / 2) * (dw / 2) + cx / 2;
        got_c0 = yuv[dc + off];
        got_c1 = yuv[dc + dc / 4 + off];
    } else {
        size_t off = (cy / 2) * dw + (cx / 2) * 2;
        got_c0 = yuv[dc + off];
        got_c1 = yuv[dc + off + 1];
    }
    int d0 = got_y - ref_y, d1 = got_c0 - ref_c0, d2 = got_c1 - ref_c1;
    int iftu_ok = rc >= 0 && d0 > -16 && d0 < 16 && d1 > -16 && d1 < 16 && d2 > -16 && d2 < 16;
    log_write("self-test: iftu rc=0x%08X Y=%d/%d c0=%d/%d c1=%d/%d", rc, got_y, ref_y, got_c0,
              ref_c0, got_c1, ref_c1);

    /* 3. Encode it and check we get a JPEG. */
    int n = -1;
    if (encoder_setup(dw, dh, 128) == 0) {
        n = ksceJpegEncoderEncode(g.ctx, g.work.va);
        if (n > 4) {
            ksceKernelDcacheInvalidateRange(jpeg_out(), JPEG_OUT_SIZE);
            if (!(jpeg_out()[0] == 0xFF && jpeg_out()[1] == 0xD8))
                n = -2;
        }
    }
    g.encoder_ok = n > 4;
    snprintf(g.selftest_msg, sizeof(g.selftest_msg), "layout %s%s, iftu %s, jpeg %s (%d bytes)",
             layout_name(g.layout), layout == LAYOUT_UNKNOWN ? " (guessed)" : "",
             iftu_ok ? "ok" : "mismatch", g.encoder_ok ? "ok" : "failed", n);
    log_write("self-test: %s", g.selftest_msg);
    block_free(&img);
}

/* ------------------------------------------------------------------ */
/* frames                                                              */

static void publish(const uint8_t *data, int len)
{
    int best = -1;
    lock();
    for (int i = 0; i < FRAME_SLOTS; i++) {
        if (g.slot[i].readers)
            continue;
        if (best < 0 || g.slot[i].seq < g.slot[best].seq)
            best = i;
    }
    if (best < 0 || len > SLOT_SIZE) {
        unlock();
        return;
    }
    g.slot[best].seq = 0; /* invisible while we write it */
    g.slot[best].len = 0;
    g.slot[best].readers = 1;
    unlock();

    memcpy(g.slots.va + (size_t)best * SLOT_SIZE, data, (size_t)len);

    lock();
    g.slot[best].seq = ++g.seq;
    g.slot[best].len = len;
    g.slot[best].readers = 0;
    unlock();
}

/* Grabs the newest frame after `after`; returns its slot or -1. */
static int frame_acquire(uint32_t after, uint32_t *seq_out)
{
    int best = -1;
    lock();
    for (int i = 0; i < FRAME_SLOTS; i++) {
        if (g.slot[i].len <= 0 || g.slot[i].seq <= after)
            continue;
        if (best < 0 || g.slot[i].seq > g.slot[best].seq)
            best = i;
    }
    if (best >= 0) {
        g.slot[best].readers++;
        *seq_out = g.slot[best].seq;
    }
    unlock();
    return best;
}

static void frame_release(int i)
{
    lock();
    g.slot[i].readers--;
    unlock();
}

static int capture_frame(void)
{
    SceDisplayFrameBufInfo fb;
    int head = ksceDisplayGetPrimaryHead();
    memset(&fb, 0, sizeof(fb));
    fb.size = sizeof(fb);
    int rc = ksceDisplayGetProcFrameBufInternal(-1, head, 0, &fb);
    if (rc < 0 || fb.paddr == 0) {
        memset(&fb, 0, sizeof(fb));
        fb.size = sizeof(fb);
        rc = ksceDisplayGetProcFrameBufInternal(-1, head, 1, &fb);
    }
    if (rc < 0 || fb.paddr == 0 || fb.framebuf.width == 0 || fb.framebuf.height == 0)
        return -1;

    int p = g.preset;
    if (p < 0 || p >= MIRROR_PRESETS)
        p = 1;
    int w = mirror_presets[p].width, h = mirror_presets[p].height;
    if (!g.enc_ready || w != g.enc_w || h != g.enc_h || g.ratio != g.enc_ratio)
        if (encoder_setup(w, h, g.ratio) < 0)
            return -1;

    rc = iftu_convert(fb.paddr, fb.framebuf.width, fb.framebuf.height, fb.framebuf.pitch,
                      fb.framebuf.pixelformat, w, h, g.work.pa, g.layout);
    if (rc < 0)
        return rc;
    int n = ksceJpegEncoderEncode(g.ctx, g.work.va);
    if (n <= 0 || n > JPEG_OUT_SIZE)
        return n < 0 ? n : -1;
    ksceKernelDcacheInvalidateRange(jpeg_out(), (unsigned)ALIGN(n, 64));
    g.last_frame_bytes = n;
    publish(jpeg_out(), n);
    return 0;
}

static int capture_main(SceSize args, void *argp)
{
    uint64_t combo_since = 0, second_start = now_us();
    int combo_fired = 0, frames_this_second = 0, errors = 0;
    (void)args;
    (void)argp;

    while (g.running) {
        uint64_t t0 = now_us();

        /* L + R + SELECT held for a second pauses/resumes mirroring. */
        SceCtrlData pad;
        memset(&pad, 0, sizeof(pad));
        if (ksceCtrlPeekBufferPositive(0, &pad, 1) > 0 && (pad.buttons & TOGGLE_BUTTONS) == TOGGLE_BUTTONS) {
            if (!combo_since)
                combo_since = t0;
            if (!combo_fired && t0 - combo_since > 1000000) {
                g.paused = !g.paused;
                combo_fired = 1;
                log_write("mirroring %s from the button combo", g.paused ? "paused" : "resumed");
            }
        } else {
            combo_since = 0;
            combo_fired = 0;
        }

        if (t0 - second_start >= 1000000) {
            g.fps_measured = frames_this_second;
            frames_this_second = 0;
            second_start = t0;
        }

        if (g.streams == 0 || g.paused) {
            ksceKernelDelayThread(50 * 1000);
            continue;
        }
        if (!g.selftest_done)
            self_test();
        if (!g.encoder_ok) {
            ksceKernelDelayThread(500 * 1000);
            continue;
        }
        int rc = capture_frame();
        if (rc == 0) {
            frames_this_second++;
            errors = 0;
        } else if (++errors == 1 || errors % 300 == 0) {
            log_write("capture failed: 0x%08X", rc);
        }

        int fps = g.fps < 1 ? 1 : g.fps > 60 ? 60 : g.fps;
        uint64_t frame_us = 1000000u / (unsigned)fps;
        uint64_t spent = now_us() - t0;
        ksceKernelDelayThread(spent < frame_us ? (SceUInt32)(frame_us - spent) : 1000);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* HTTP server                                                         */

static int send_all(int fd, const void *buf, int len)
{
    const uint8_t *p = buf;
    while (len > 0) {
        int n = ksceNetSend(fd, p, (unsigned)len, 0);
        if (n <= 0)
            return -1;
        p += n;
        len -= n;
    }
    return 0;
}

static int send_str(int fd, const char *s)
{
    return send_all(fd, s, (int)strlen(s));
}

static void send_response(int fd, const char *status, const char *type, const char *body)
{
    char head[256];
    snprintf(head, sizeof(head),
             "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
             "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
             status, type, (int)strlen(body));
    if (send_str(fd, head) == 0)
        send_str(fd, body);
}

static void status_json(char *out, size_t cap)
{
    int p = g.preset;
    if (p < 0 || p >= MIRROR_PRESETS)
        p = 1;
    snprintf(out, cap,
             "{\"version\":\"" MIRROR_VERSION "\",\"paused\":%s,\"preset\":%d,\"presetName\":\"%s\","
             "\"width\":%d,\"height\":%d,\"ratio\":%d,\"fpsTarget\":%d,\"fps\":%d,\"frameBytes\":%d,"
             "\"clients\":%d,\"streams\":%d,\"selftest\":\"%s\",\"encoder\":%s}",
             g.paused ? "true" : "false", p, mirror_presets[p].name, mirror_presets[p].width,
             mirror_presets[p].height, g.ratio, g.fps, g.fps_measured, g.last_frame_bytes, g.clients,
             g.streams, g.selftest_done ? g.selftest_msg : "pending",
             !g.selftest_done ? "null" : g.encoder_ok ? "true" : "false");
}

static void serve_stream(int fd)
{
    char head[160];
    uint32_t last = 0;

    lock();
    int allowed = g.streams < MAX_STREAMS;
    if (allowed)
        g.streams++;
    unlock();
    if (!allowed) {
        send_response(fd, "503 Service Unavailable", "text/plain", "too many viewers\n");
        return;
    }
    log_write("viewer connected (%d streaming)", g.streams);
    if (send_str(fd, "HTTP/1.1 200 OK\r\n"
                     "Content-Type: multipart/x-mixed-replace; boundary=vitacastframe\r\n"
                     "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n"
                     "Connection: close\r\n\r\n") == 0) {
        while (g.running) {
            uint32_t seq;
            int i = frame_acquire(last, &seq);
            if (i < 0) {
                ksceKernelDelayThread(5 * 1000);
                continue;
            }
            int len = g.slot[i].len;
            snprintf(head, sizeof(head),
                     "--vitacastframe\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n", len);
            int rc = send_str(fd, head);
            if (rc == 0)
                rc = send_all(fd, g.slots.va + (size_t)i * SLOT_SIZE, len);
            frame_release(i);
            if (rc == 0)
                rc = send_str(fd, "\r\n");
            if (rc != 0)
                break;
            last = seq;
        }
    }
    lock();
    g.streams--;
    unlock();
    log_write("viewer disconnected (%d streaming)", g.streams);
}

static void serve_frame(int fd)
{
    char head[200];
    uint32_t seq;
    int i = frame_acquire(0, &seq);
    if (i < 0) {
        send_response(fd, "503 Service Unavailable", "text/plain", "no frame yet\n");
        return;
    }
    int len = g.slot[i].len;
    snprintf(head, sizeof(head),
             "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n"
             "Cache-Control: no-store\r\nConnection: close\r\n\r\n", len);
    if (send_str(fd, head) == 0)
        send_all(fd, g.slots.va + (size_t)i * SLOT_SIZE, len);
    frame_release(i);
}

static void serve_control(int fd, const char *query, int from_localhost)
{
    char body[512];
    int v;
    if (!from_localhost) {
        send_response(fd, "403 Forbidden", "text/plain", "control is local only\n");
        return;
    }
    if (query_int(query, "preset", &v) && v >= 0 && v < MIRROR_PRESETS)
        g.preset = v;
    if (query_int(query, "ratio", &v) && v >= 0 && v <= 255)
        g.ratio = v;
    if (query_int(query, "fps", &v) && v >= 1 && v <= 60)
        g.fps = v;
    if (query_int(query, "paused", &v))
        g.paused = v != 0;
    status_json(body, sizeof(body));
    send_response(fd, "200 OK", "application/json", body);
}

typedef struct client_arg {
    int fd;
    unsigned int peer;
} client_arg;

static int client_main(SceSize args, void *argp)
{
    client_arg a = *(client_arg *)argp;
    char req[1024];
    char query[256];
    int len = 0;
    (void)args;

    while (len < (int)sizeof(req) - 1) {
        int n = ksceNetRecv(a.fd, req + len, (unsigned)(sizeof(req) - 1 - (size_t)len), 0);
        if (n <= 0)
            break;
        len += n;
        if (request_complete(req, (size_t)len))
            break;
    }
    if (len > 0) {
        req[len] = '\0';
        /* 127.0.0.1 in network byte order, as stored in s_addr */
        int local = a.peer == 0x0100007Fu;
        switch (parse_request(req, (size_t)len, query, sizeof(query))) {
        case REQ_INDEX:
            send_response(a.fd, "200 OK", "text/html; charset=utf-8", mirror_viewer_html);
            break;
        case REQ_STREAM:
            serve_stream(a.fd);
            break;
        case REQ_FRAME:
            serve_frame(a.fd);
            break;
        case REQ_STATUS: {
            char body[512];
            status_json(body, sizeof(body));
            send_response(a.fd, "200 OK", "application/json", body);
            break;
        }
        case REQ_CONTROL:
            serve_control(a.fd, query, local);
            break;
        case REQ_NOT_FOUND:
            send_response(a.fd, "404 Not Found", "text/plain", "not found\n");
            break;
        default:
            send_response(a.fd, "400 Bad Request", "text/plain", "bad request\n");
            break;
        }
    }
    ksceNetClose(a.fd);
    lock();
    g.clients--;
    unlock();
    return ksceKernelExitDeleteThread(0);
}

static void set_int_opt(int fd, int level, int name, int value)
{
    ksceNetSetsockopt(fd, level, name, &value, sizeof(value));
}

static int open_listener(void)
{
    SceNetSockaddrIn addr;
    int fd = ksceNetSocket("vcm_listen", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (fd < 0)
        return fd;
    set_int_opt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR, 1);
    set_int_opt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, 1);
    memset(&addr, 0, sizeof(addr));
    addr.sin_len = sizeof(addr);
    addr.sin_family = SCE_NET_AF_INET;
    addr.sin_port = (unsigned short)((MIRROR_PORT >> 8) | ((MIRROR_PORT & 0xFF) << 8));
    addr.sin_addr.s_addr = SCE_NET_INADDR_ANY;
    int rc = ksceNetBind(fd, (SceNetSockaddr *)&addr, sizeof(addr));
    if (rc >= 0)
        rc = ksceNetListen(fd, 4);
    if (rc < 0) {
        ksceNetClose(fd);
        return rc;
    }
    return fd;
}

static int server_main(SceSize args, void *argp)
{
    int fd = -1, reported = 0;
    (void)args;
    (void)argp;

    while (g.running) {
        if (fd < 0) {
            fd = open_listener();
            if (fd < 0) {
                if (!reported++)
                    log_write("cannot listen on port %d (0x%08X), retrying", MIRROR_PORT, fd);
                ksceKernelDelayThread(1000 * 1000);
                continue;
            }
            log_write("listening on port %d", MIRROR_PORT);
            reported = 0;
        }
        SceNetSockaddrIn peer;
        unsigned int plen = sizeof(peer);
        memset(&peer, 0, sizeof(peer));
        int c = ksceNetAccept(fd, (SceNetSockaddr *)&peer, &plen);
        if (c == (int)SCE_NET_ERROR_EAGAIN || c == (int)SCE_NET_ERROR_EINTR) {
            ksceKernelDelayThread(20 * 1000);
            continue;
        }
        if (c < 0) {
            /* Network went away (sleep, Wi-Fi off): start over. */
            log_write("accept failed (0x%08X), reopening", c);
            ksceNetClose(fd);
            fd = -1;
            ksceKernelDelayThread(1000 * 1000);
            continue;
        }
        set_int_opt(c, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, 0);
        set_int_opt(c, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, 5 * 1000 * 1000);
        set_int_opt(c, SCE_NET_SOL_SOCKET, SCE_NET_SO_SNDTIMEO, 5 * 1000 * 1000);
        set_int_opt(c, SCE_NET_IPPROTO_TCP, SCE_NET_TCP_NODELAY, 1);

        lock();
        int ok = g.clients < MAX_CLIENTS;
        if (ok)
            g.clients++;
        unlock();
        if (!ok) {
            ksceNetClose(c);
            continue;
        }
        client_arg a = { c, peer.sin_addr.s_addr };
        SceUID t = ksceKernelCreateThread("vcm_client", client_main, 0x50, 0x4000, 0, 0x10000, NULL);
        if (t < 0 || ksceKernelStartThread(t, sizeof(a), &a) < 0) {
            if (t >= 0)
                ksceKernelDeleteThread(t);
            ksceNetClose(c);
            lock();
            g.clients--;
            unlock();
        }
    }
    if (fd >= 0)
        ksceNetClose(fd);
    return 0;
}

/* ------------------------------------------------------------------ */
/* module entry points                                                 */

static void cleanup(void)
{
    if (g.enc_ready)
        ksceJpegEncoderEnd(g.ctx);
    g.enc_ready = 0;
    block_free(&g.ctx_mem);
    block_free(&g.slots);
    block_free(&g.work);
    if (g.lock > 0)
        ksceKernelDeleteMutex(g.lock);
    g.lock = 0;
}

void _start() __attribute__((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
    (void)argc;
    (void)args;
    memset(&g, 0, sizeof(g));
    g.preset = 1;
    g.ratio = 128;
    g.fps = 30;

    SceUID fd = ksceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0)
        ksceIoClose(fd);
    log_write("VitaCast Mirror " MIRROR_VERSION " starting");

    g.lock = ksceKernelCreateMutex("vcm_lock", 0, 0, NULL);
    int ctx_size = ksceJpegEncoderGetContextSize();
    if (g.lock < 0 || ctx_size <= 0 ||
        block_alloc(&g.work, "vcm_work", WORK_SIZE, 1) < 0 ||
        block_alloc(&g.slots, "vcm_slots", FRAME_SLOTS * SLOT_SIZE, 0) < 0 ||
        block_alloc(&g.ctx_mem, "vcm_ctx", (SceSize)ctx_size, 0) < 0) {
        log_write("startup failed (lock 0x%08X, ctx %d)", g.lock, ctx_size);
        cleanup();
        return SCE_KERNEL_START_FAILED;
    }
    g.ctx = g.ctx_mem.va;
    log_write("buffers: work %s at 0x%08X", g.work.cdram ? "CDRAM" : "LPDDR", (unsigned)g.work.pa);

    g.running = 1;
    g.capture_thread = ksceKernelCreateThread("vcm_capture", capture_main, 0x40, 0x4000, 0, 0x10000, NULL);
    g.server_thread = ksceKernelCreateThread("vcm_server", server_main, 0x50, 0x4000, 0, 0x10000, NULL);
    if (g.capture_thread < 0 || g.server_thread < 0) {
        log_write("thread creation failed");
        g.running = 0;
        if (g.capture_thread >= 0)
            ksceKernelDeleteThread(g.capture_thread);
        if (g.server_thread >= 0)
            ksceKernelDeleteThread(g.server_thread);
        cleanup();
        return SCE_KERNEL_START_FAILED;
    }
    ksceKernelStartThread(g.capture_thread, 0, NULL);
    ksceKernelStartThread(g.server_thread, 0, NULL);
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
    (void)argc;
    (void)args;
    g.running = 0;
    ksceKernelWaitThreadEnd(g.capture_thread, NULL, NULL);
    ksceKernelWaitThreadEnd(g.server_thread, NULL, NULL);
    ksceKernelDeleteThread(g.capture_thread);
    ksceKernelDeleteThread(g.server_thread);
    for (int i = 0; i < 100 && g.clients > 0; i++)
        ksceKernelDelayThread(50 * 1000);
    cleanup();
    return 0;
}
