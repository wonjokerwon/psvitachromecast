/* The Cast receiver: TLS channel on port 8009 speaking CASTV2, an mDNS
 * advertisement and the small HTTP API senders use to identify devices. */
#ifndef VITACAST_RECEIVER_H
#define VITACAST_RECEIVER_H

#include <stdint.h>

#include "../platform/player.h"
#include "config.h"

typedef struct receiver_opts {
    const vc_config *cfg;
    const char *ip;
    const char *model_name;
    uint16_t cast_port;  /* 8009 */
    uint16_t http_port;  /* 8008, 0 disables */
    uint16_t https_port; /* 8443, 0 disables */
    int enable_mdns;
} receiver_opts;

typedef struct receiver_ui {
    int senders;
    int app_running;
    char app_name[64];
    int has_media;
    char title[128];
    char subtitle[128];
    player_status player;
    double volume;
    int muted;
} receiver_ui;

int receiver_start(const receiver_opts *opts);
void receiver_stop(void);
/* Call a few times per second: pushes MEDIA_STATUS updates to senders. */
void receiver_tick(void);
void receiver_get_ui(receiver_ui *ui);

/* Local (on-device) controls; senders are notified like a real Chromecast. */
void receiver_local_toggle_pause(void);
void receiver_local_seek(double delta_seconds);
void receiver_local_stop(void);
void receiver_local_volume(double delta);

#endif
