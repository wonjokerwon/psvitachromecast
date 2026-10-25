/* HTTP(S) client and a random-access, read-ahead cached media source on top
 * of Range requests. The Vita's AvPlayer reads media through file callbacks
 * ("read N bytes at offset X"); http_source turns those into HTTP ranges. */
#ifndef VITACAST_HTTP_H
#define VITACAST_HTTP_H

#include <stddef.h>
#include <stdint.h>

typedef struct http_url {
    int tls;
    char host[256];
    uint16_t port;
    char path[3072];
} http_url;

int http_parse_url(const char *url, http_url *out);

typedef struct http_source http_source;

/* Opens url (following redirects) and determines its size.
 * On failure returns NULL and writes a reason to err. */
http_source *http_source_open(const char *url, char *err, size_t errcap);
int64_t http_source_size(const http_source *hs);
const char *http_source_content_type(const http_source *hs);
/* Reads up to len bytes at pos. Returns bytes read (0 at EOF) or -1. */
int http_source_read(http_source *hs, uint8_t *buf, uint64_t pos, uint32_t len);
/* Makes current and future reads fail quickly. Thread safe. */
void http_source_abort(http_source *hs);
void http_source_close(http_source *hs);

#endif
