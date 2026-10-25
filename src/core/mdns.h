/* Minimal mDNS/DNS-SD responder advertising _googlecast._tcp. */
#ifndef VITACAST_MDNS_H
#define VITACAST_MDNS_H

#include <stddef.h>
#include <stdint.h>

#define MDNS_MAX_TXT 16

typedef struct mdns_info {
    char instance[96];  /* e.g. "PSVita-<id>" -> <instance>._googlecast._tcp.local */
    char hostname[96];  /* e.g. "<uuid>" -> <hostname>.local */
    char ip[16];
    uint16_t port;
    int txt_count;
    char txt[MDNS_MAX_TXT][128]; /* "key=value" */
} mdns_info;

/* Builds a response with all records. ttl_scale 0 builds a goodbye packet.
 * Returns the packet length or 0 on overflow. */
size_t mdns_build_response(uint8_t *buf, size_t cap, const mdns_info *info, uint16_t id,
                           int ttl_scale, int services_only);

#define MDNS_Q_NONE 0
#define MDNS_Q_SERVICE 1   /* asks about our service, instance or host */
#define MDNS_Q_SERVICES 2  /* asks for the DNS-SD service list */

/* Classifies an incoming packet. *unicast is set when a QU question was seen. */
int mdns_classify_query(const uint8_t *pkt, size_t len, const mdns_info *info, int *unicast,
                        uint16_t *id);

int mdns_start(const mdns_info *info);
void mdns_stop(void);

/* --- browsing for Cast devices --- */

typedef struct cast_device {
    char name[64];  /* friendly name (TXT fn) */
    char model[64]; /* TXT md */
    char id[48];    /* TXT id */
    char ip[16];
    uint16_t port;
} cast_device;

#define MDNS_MAX_SEEN 24

typedef struct mdns_seen_instance {
    char instance[256];
    char target[256];
    char from_ip[16];
    uint16_t port;
    int capabilities; /* TXT ca, -1 when absent */
    char name[64];
    char model[64];
    char id[48];
} mdns_seen_instance;

typedef struct mdns_seen_host {
    char host[256];
    char ip[16];
} mdns_seen_host;

typedef struct mdns_collector {
    int ninst, nhost;
    mdns_seen_instance inst[MDNS_MAX_SEEN];
    mdns_seen_host host[MDNS_MAX_SEEN];
} mdns_collector;

/* Builds a PTR query for _googlecast._tcp.local. */
size_t mdns_build_query(uint8_t *buf, size_t cap);
/* Feeds one received packet into the collector. */
void mdns_collect(mdns_collector *c, const uint8_t *pkt, size_t len, const char *from_ip);
/* Turns what was collected into devices that have a screen. */
int mdns_collector_devices(const mdns_collector *c, cast_device *out, int max);
/* Queries the network for timeout_ms; returns the number of devices. */
int mdns_browse(const char *iface_ip, int timeout_ms, cast_device *out, int max);

#endif
