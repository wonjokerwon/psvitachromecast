/* VitaCast for PS Vita: turns the handheld into a Cast receiver. */
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include "../core/config.h"
#include "../core/log.h"
#include "../core/receiver.h"
#include "../core/tls.h"
#include "../platform/platform.h"
#include "../platform/player.h"
#include "ui.h"
#include "vita.h"

/* AvPlayer, the TLS stack and the range cache all allocate from newlib. */
int _newlib_heap_size_user = 128 * 1024 * 1024;

#define NET_MEMORY (1024 * 1024)
#define OVERLAY_MS 4000

static vc_config g_cfg;
static atomic_int g_keys_state; /* 0 running, 1 ok, -1 failed */
static char g_ip[16];

static int net_init(void)
{
    if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0)
        return -1;
    SceNetInitParam p;
    p.memory = malloc(NET_MEMORY);
    p.size = NET_MEMORY;
    p.flags = 0;
    int rc = sceNetInit(&p);
    if (rc < 0 && rc != (int)SCE_NET_ERROR_EBUSY) /* EBUSY: already initialised */
        return -1;
    sceNetCtlInit();
    return 0;
}

static void *keys_main(void *arg)
{
    (void)arg;
    g_keys_state = tls_init(plat_data_dir(), g_cfg.uuid_hex) == 0 ? 1 : -1;
    return NULL;
}

static int start_receiver(void)
{
    receiver_opts o;
    memset(&o, 0, sizeof(o));
    o.cfg = &g_cfg;
    o.ip = g_ip;
    o.model_name = "PS Vita";
    o.cast_port = 8009;
    o.http_port = 8008;
    o.https_port = 8443;
    o.enable_mdns = 1;
    return receiver_start(&o);
}

int main(void)
{
    ui_model m;
    SceCtrlData pad, prev;
    uint64_t last_tick = 0, last_net_check = 0, last_frame, overlay_until = 0;
    int running = 0, quit = 0;
    player_state last_state = PLAYER_IDLE;
    char last_title[128] = "";
    pthread_t keys_thread;

    vita_platform_init("log.txt");
    LOGI("VitaCast starting");
    if (ui_init() != 0) {
        sceKernelExitProcess(0);
        return 0;
    }
    sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);

    memset(&m, 0, sizeof(m));
    config_load(plat_data_dir(), "PS Vita", &g_cfg);
    m.name = g_cfg.name;
    m.ip = g_ip;

    if (net_init() != 0) {
        m.phase = UI_PHASE_ERROR;
        m.message = "Network module failed to start.";
    } else {
        m.phase = UI_PHASE_KEYS;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 256 * 1024);
        if (pthread_create(&keys_thread, &attr, keys_main, NULL) != 0)
            g_keys_state = -1;
        else
            pthread_detach(keys_thread);
        pthread_attr_destroy(&attr);
    }
    player_init();

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
    memset(&prev, 0, sizeof(prev));
    last_frame = plat_ticks_ms();

    while (!quit) {
        uint64_t now = plat_ticks_ms();

        /* A long gap means the system suspended us: sockets are dead. */
        if (running && now - last_frame > 5000) {
            LOGI("Resumed from suspend, restarting receiver");
            receiver_stop();
            running = 0;
            m.phase = UI_PHASE_WIFI;
        }
        last_frame = now;

        if (m.phase == UI_PHASE_KEYS && g_keys_state != 0) {
            if (g_keys_state > 0) {
                m.phase = UI_PHASE_WIFI;
            } else {
                m.phase = UI_PHASE_ERROR;
                m.message = "Could not create the device key (see ux0:data/VitaCast/log.txt).";
            }
        }
        if ((m.phase == UI_PHASE_WIFI || m.phase == UI_PHASE_READY) && now - last_net_check > 1500) {
            char ip[16];
            last_net_check = now;
            int have_ip = plat_local_ipv4(ip) == 0;
            if (running && (!have_ip || strcmp(ip, g_ip) != 0)) {
                LOGI("Network changed, restarting receiver");
                receiver_stop();
                running = 0;
                m.phase = UI_PHASE_WIFI;
            }
            if (!running && have_ip) {
                snprintf(g_ip, sizeof(g_ip), "%s", ip);
                if (start_receiver() == 0) {
                    running = 1;
                    m.phase = UI_PHASE_READY;
                    LOGI("Ready: \"%s\" at %s", g_cfg.name, g_ip);
                } else {
                    receiver_stop();
                }
            }
        }

        if (running && now - last_tick >= 200) {
            last_tick = now;
            receiver_tick();
        }
        if (running)
            receiver_get_ui(&m.rx);
        else
            memset(&m.rx, 0, sizeof(m.rx));

        /* input */
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned pressed = pad.buttons & ~prev.buttons;
        prev = pad;
        if ((pad.buttons & SCE_CTRL_START) && (pad.buttons & SCE_CTRL_SELECT))
            quit = 1;
        if (pressed)
            overlay_until = now + OVERLAY_MS;
        if (pressed & SCE_CTRL_TRIANGLE)
            m.show_help = !m.show_help;
        if (running && m.rx.has_media) {
            if (pressed & SCE_CTRL_CROSS)
                receiver_local_toggle_pause();
            if (pressed & SCE_CTRL_CIRCLE)
                receiver_local_stop();
            if (pressed & SCE_CTRL_LEFT)
                receiver_local_seek(-10);
            if (pressed & SCE_CTRL_RIGHT)
                receiver_local_seek(10);
            if (pressed & SCE_CTRL_LTRIGGER)
                receiver_local_seek(-60);
            if (pressed & SCE_CTRL_RTRIGGER)
                receiver_local_seek(60);
        }
        if (running && (pressed & SCE_CTRL_UP))
            receiver_local_volume(0.05);
        if (running && (pressed & SCE_CTRL_DOWN))
            receiver_local_volume(-0.05);

        if (m.rx.player.state != last_state || strcmp(m.rx.title, last_title) != 0) {
            last_state = m.rx.player.state;
            snprintf(last_title, sizeof(last_title), "%s", m.rx.title);
            overlay_until = now + OVERLAY_MS;
        }
        m.show_overlay = now < overlay_until;

        /* Stay awake while waiting for senders; keep the screen on while
         * something is playing. */
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        if (m.rx.has_media && m.rx.player.state == PLAYER_PLAYING)
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_OFF);

        ui_draw(&m);
    }

    LOGI("VitaCast exiting");
    if (running)
        receiver_stop();
    player_shutdown();
    if (g_keys_state > 0)
        tls_shutdown();
    ui_shutdown();
    sceNetCtlTerm();
    sceNetTerm();
    vita_platform_shutdown();
    sceKernelExitProcess(0);
    return 0;
}
