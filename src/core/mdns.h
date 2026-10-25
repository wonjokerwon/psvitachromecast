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

#endif
