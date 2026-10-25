/* vita2d user interface: idle "ready to cast" screen, video, playback overlay. */
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <vita2d.h>

#include "../core/log.h"
#include "vita.h"

#define SCREEN_W 960
#define SCREEN_H 544

#define COL_BG RGBA8(14, 17, 22, 255)
#define COL_PANEL RGBA8(28, 33, 42, 255)
#define COL_TEXT RGBA8(240, 242, 245, 255)
#define COL_DIM RGBA8(150, 158, 170, 255)
#define COL_FAINT RGBA8(95, 102, 114, 255)
#define COL_ACCENT RGBA8(92, 156, 255, 255)
#define COL_OK RGBA8(80, 200, 120, 255)
#define COL_WARN RGBA8(240, 180, 60, 255)
#define COL_ERR RGBA8(235, 87, 87, 255)
#define COL_SHADE RGBA8(0, 0, 0, 170)

static vita2d_pgf *g_font;

int ui_init(void)
{
    if (vita2d_init() < 0)
        return -1;
    vita2d_set_clear_color(COL_BG);
    vita2d_set_vblank_wait(1);
    g_font = vita2d_load_default_pgf();
    return g_font ? 0 : -1;
}

void ui_shutdown(void)
{
    vita2d_wait_rendering_done();
    if (g_font)
        vita2d_free_pgf(g_font);
    g_font = NULL;
    vita2d_fini();
}

static void text(int x, int y, unsigned color, float scale, const char *s)
{
    vita2d_pgf_draw_text(g_font, x, y, color, scale, s);
}

static int text_w(float scale, const char *s)
{
    return vita2d_pgf_text_width(g_font, scale, s);
}

static void text_center(int y, unsigned color, float scale, const char *s)
{
    text((SCREEN_W - text_w(scale, s)) / 2, y, color, scale, s);
}

/* Truncates s with "..." so it fits in max_w pixels. */
static void fit(char *out, size_t cap, const char *s, float scale, int max_w)
{
    snprintf(out, cap, "%s", s);
    if (text_w(scale, out) <= max_w)
        return;
    size_t n = strlen(out);
    while (n > 0) {
        n--;
        /* do not cut inside a UTF-8 sequence */
        while (n > 0 && ((unsigned char)out[n] & 0xC0) == 0x80)
            n--;
        out[n] = '\0';
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "%s...", out);
        if (text_w(scale, tmp) <= max_w) {
            snprintf(out, cap, "%s", tmp);
            return;
        }
    }
}

static void fmt_time(char *out, size_t cap, double t)
{
    if (t < 0 || isnan(t))
        t = 0;
    int s = (int)t;
    if (s >= 3600)
        snprintf(out, cap, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else
        snprintf(out, cap, "%d:%02d", s / 60, s % 60);
}

/* A Cast-style glyph: a screen outline with three arcs in its corner. */
static void draw_cast_icon(float cx, float cy, float size, unsigned color, float pulse)
{
    float w = size, h = size * 0.72f;
    float x = cx - w / 2, y = cy - h / 2;
    float t = size * 0.07f;
    vita2d_draw_rectangle(x, y, w, t, color);
    vita2d_draw_rectangle(x + w - t, y, t, h, color);
    vita2d_draw_rectangle(x + w * 0.45f, y + h - t, w * 0.55f, t, color);
    vita2d_draw_rectangle(x, y, t, h * 0.35f, color);
    /* arcs from the bottom-left corner, drawn as dotted quarter circles */
    float ox = x, oy = y + h;
    for (int ring = 0; ring < 3; ring++) {
        float r = h * (0.22f + 0.24f * (float)ring);
        unsigned a = (unsigned)(140 + 115 * fabsf(sinf(pulse + (float)ring * 0.6f)));
        unsigned col = (color & 0x00FFFFFF) | (a << 24);
        for (int i = 0; i <= 24; i++) {
            float ang = (float)M_PI * 0.5f * (float)i / 24.0f;
            vita2d_draw_fill_circle(ox + cosf(ang) * r, oy - sinf(ang) * r, t * 0.55f, col);
        }
    }
    vita2d_draw_fill_circle(ox + t * 0.6f, oy - t * 0.6f, t * 1.1f, color);
}

static void draw_header(const ui_model *m)
{
    text(40, 62, COL_TEXT, 1.6f, "VitaCast");
    const char *status;
    unsigned col;
    switch (m->phase) {
    case UI_PHASE_READY: status = "Online"; col = COL_OK; break;
    case UI_PHASE_WIFI: status = "Waiting for Wi-Fi"; col = COL_WARN; break;
    case UI_PHASE_KEYS: status = "Starting"; col = COL_WARN; break;
    default: status = "Error"; col = COL_ERR; break;
    }
    int w = text_w(1.0f, status);
    vita2d_draw_fill_circle((float)(SCREEN_W - 52 - w), 54, 6, col);
    text(SCREEN_W - 40 - w, 62, COL_DIM, 1.0f, status);
}

static void draw_idle(const ui_model *m)
{
    char buf[256];
    float pulse = (float)(sceKernelGetProcessTimeWide() % 10000000ULL) / 1000000.0f * 1.2f;

    draw_header(m);
    draw_cast_icon(SCREEN_W / 2.0f, 170, 120, m->phase == UI_PHASE_READY ? COL_ACCENT : COL_FAINT, pulse);

    switch (m->phase) {
    case UI_PHASE_KEYS:
        text_center(290, COL_TEXT, 1.4f, "Setting up...");
        text_center(325, COL_DIM, 1.0f, "Creating this device's encryption key. First start only,");
        text_center(350, COL_DIM, 1.0f, "this can take up to a minute.");
        break;
    case UI_PHASE_WIFI:
        text_center(290, COL_TEXT, 1.4f, "Connect to Wi-Fi");
        text_center(325, COL_DIM, 1.0f, "Join the same network as your phone or computer.");
        text_center(350, COL_DIM, 1.0f, "Settings > Network > Wi-Fi Settings");
        break;
    case UI_PHASE_ERROR:
        text_center(290, COL_ERR, 1.4f, "Something went wrong");
        text_center(325, COL_DIM, 1.0f, m->message ? m->message : "");
        break;
    case UI_PHASE_READY: {
        snprintf(buf, sizeof(buf), "Ready to cast to \"%s\"", m->name);
        char fitted[256];
        fit(fitted, sizeof(fitted), buf, 1.4f, SCREEN_W - 80);
        text_center(290, COL_TEXT, 1.4f, fitted);
        text_center(325, COL_DIM, 1.0f, "Pick this device in a Cast sender on the same Wi-Fi network.");

        /* info panel */
        int px = 150, py = 352, pw = SCREEN_W - 300, ph = 76;
        vita2d_draw_rectangle(px, py, pw, ph, COL_PANEL);
        text(px + 20, py + 30, COL_FAINT, 0.9f, "IP ADDRESS");
        text(px + 20, py + 58, COL_TEXT, 1.1f, m->ip ? m->ip : "-");
        text(px + 250, py + 30, COL_FAINT, 0.9f, "SENDERS");
        snprintf(buf, sizeof(buf), "%d connected", m->rx.senders);
        text(px + 250, py + 58, COL_TEXT, 1.1f, buf);
        text(px + 450, py + 30, COL_FAINT, 0.9f, "PORT");
        text(px + 450, py + 58, COL_TEXT, 1.1f, "8009");
        break;
    }
    }

    if (m->show_help || m->phase != UI_PHASE_READY) {
        char lines[LOG_RING_LINES][LOG_LINE_MAX];
        int n = log_recent(lines, 3);
        for (int i = 0; i < n; i++) {
            char fitted[LOG_LINE_MAX + 4];
            fit(fitted, sizeof(fitted), lines[i], 0.8f, SCREEN_W - 80);
            text(40, 462 + i * 20, COL_FAINT, 0.8f, fitted);
        }
    }
    text(40, 528, COL_FAINT, 0.85f, "TRIANGLE: show log      START + SELECT: exit");
}

static void draw_overlay(const ui_model *m, int audio_only)
{
    const player_status *st = &m->rx.player;
    char buf[256], fitted[256], t1[16], t2[16];

    vita2d_draw_rectangle(0, SCREEN_H - 124, SCREEN_W, 124, COL_SHADE);
    fit(fitted, sizeof(fitted), m->rx.title[0] ? m->rx.title : "Casting", 1.2f, SCREEN_W - 260);
    if (!audio_only)
        text(32, SCREEN_H - 88, COL_TEXT, 1.2f, fitted);
    if (m->rx.subtitle[0] && !audio_only) {
        fit(fitted, sizeof(fitted), m->rx.subtitle, 0.9f, SCREEN_W - 260);
        text(32, SCREEN_H - 64, COL_DIM, 0.9f, fitted);
    }

    const char *state = "";
    switch (st->state) {
    case PLAYER_PAUSED: state = "Paused"; break;
    case PLAYER_LOADING: state = "Loading..."; break;
    case PLAYER_BUFFERING: state = "Buffering..."; break;
    case PLAYER_IDLE:
        state = st->idle_reason == PLAYER_IDLE_ERROR ? "Playback error" : "Stopped";
        break;
    default: break;
    }
    if (m->rx.muted)
        snprintf(buf, sizeof(buf), "%s   Muted", state);
    else
        snprintf(buf, sizeof(buf), "%s   Vol %d%%", state, (int)(m->rx.volume * 100 + 0.5));
    text(SCREEN_W - 32 - text_w(0.9f, buf), SCREEN_H - 88, COL_DIM, 0.9f, buf);

    /* progress */
    float bx = 32, by = SCREEN_H - 46, bw = SCREEN_W - 64;
    vita2d_draw_rectangle(bx, by, bw, 5, RGBA8(255, 255, 255, 60));
    if (st->duration > 0) {
        float p = (float)(st->current_time / st->duration);
        if (p < 0)
            p = 0;
        if (p > 1)
            p = 1;
        vita2d_draw_rectangle(bx, by, bw * p, 5, COL_ACCENT);
        vita2d_draw_fill_circle(bx + bw * p, by + 2.5f, 7, COL_TEXT);
    }
    fmt_time(t1, sizeof(t1), st->current_time);
    fmt_time(t2, sizeof(t2), st->duration);
    snprintf(buf, sizeof(buf), "%s / %s", t1, st->duration > 0 ? t2 : "--:--");
    text(32, SCREEN_H - 14, COL_DIM, 0.85f, buf);
    const char *help = "X play/pause   O stop   Left/Right seek   Up/Down volume";
    text(SCREEN_W - 32 - text_w(0.85f, help), SCREEN_H - 14, COL_FAINT, 0.85f, help);

    if (st->state == PLAYER_IDLE && st->idle_reason == PLAYER_IDLE_ERROR && st->error[0]) {
        fit(fitted, sizeof(fitted), st->error, 1.0f, SCREEN_W - 80);
        text_center(SCREEN_H / 2, COL_ERR, 1.0f, fitted);
    }
}

static void draw_playing(const ui_model *m)
{
    int fw = 0, fh = 0;
    const vita2d_texture *tex = player_vita_video_frame(&fw, &fh);
    int audio_only = !tex;

    if (tex) {
        float sx = (float)SCREEN_W / (float)fw, sy = (float)SCREEN_H / (float)fh;
        float s = sx < sy ? sx : sy;
        float dw = (float)fw * s, dh = (float)fh * s;
        vita2d_draw_texture_scale(tex, (SCREEN_W - dw) / 2.0f, (SCREEN_H - dh) / 2.0f, s, s);
    } else {
        char fitted[256];
        float pulse = (float)(sceKernelGetProcessTimeWide() % 10000000ULL) / 1000000.0f * 1.2f;
        draw_cast_icon(SCREEN_W / 2.0f, 150, 90, COL_ACCENT, pulse);
        fit(fitted, sizeof(fitted), m->rx.title[0] ? m->rx.title : "Casting", 1.5f, SCREEN_W - 80);
        text_center(270, COL_TEXT, 1.5f, fitted);
        if (m->rx.subtitle[0]) {
            fit(fitted, sizeof(fitted), m->rx.subtitle, 1.1f, SCREEN_W - 80);
            text_center(305, COL_DIM, 1.1f, fitted);
        }
    }
    if (m->show_overlay || audio_only || m->rx.player.state != PLAYER_PLAYING)
        draw_overlay(m, audio_only);
}

void ui_draw(const ui_model *m)
{
    vita2d_start_drawing();
    vita2d_clear_screen();
    if (m->phase == UI_PHASE_READY && m->rx.has_media &&
        !(m->rx.player.state == PLAYER_IDLE && m->rx.player.idle_reason != PLAYER_IDLE_ERROR))
        draw_playing(m);
    else
        draw_idle(m);
    vita2d_end_drawing();
    player_vita_frame_done();
    vita2d_swap_buffers();
}
