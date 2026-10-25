/* Hardware independent parts of the screen mirroring plugin: HTTP request
 * routing, the viewer page and YCbCr layout detection. Kept free of kernel
 * APIs (only memcmp/strlen-style helpers) so it is unit tested on desktop. */
#ifndef VITACAST_MIRROR_LOGIC_H
#define VITACAST_MIRROR_LOGIC_H

#include <stddef.h>
#include <stdint.h>

#define MIRROR_PORT 8080

/* Chroma layouts the JPEG encoder might expect for YCbCr 4:2:0 input. */
typedef enum {
    LAYOUT_UNKNOWN = 0,
    LAYOUT_NV12, /* Y plane, then interleaved Cb,Cr */
    LAYOUT_NV21, /* Y plane, then interleaved Cr,Cb */
    LAYOUT_I420, /* Y plane, Cb plane, Cr plane */
    LAYOUT_YV12  /* Y plane, Cr plane, Cb plane */
} yuv_layout;

const char *layout_name(yuv_layout l);

/* Full range BT.601 (JPEG) conversion of one RGB colour. */
void rgb_to_ycbcr(int r, int g, int b, int *y, int *cb, int *cr);

/* Inspects a w x h YCbCr 4:2:0 buffer produced from a solid colour whose
 * expected chroma is (cb, cr) and returns its layout. */
yuv_layout detect_layout(const uint8_t *buf, int w, int h, int cb, int cr);

typedef enum {
    REQ_BAD = 0,
    REQ_INDEX,
    REQ_STREAM,
    REQ_FRAME,
    REQ_STATUS,
    REQ_CONTROL,
    REQ_NOT_FOUND
} req_kind;

/* Returns 1 once buf holds a complete request head ("\r\n\r\n"). */
int request_complete(const char *buf, size_t len);
/* Routes a GET request; copies the query string (without '?') to query. */
req_kind parse_request(const char *buf, size_t len, char *query, size_t qcap);
/* Reads an integer parameter from a query string. Returns 1 if found. */
int query_int(const char *query, const char *key, int *out);

typedef struct mirror_preset {
    const char *name;
    int width;
    int height;
} mirror_preset;

#define MIRROR_PRESETS 3
extern const mirror_preset mirror_presets[MIRROR_PRESETS];

extern const char mirror_viewer_html[];

#endif
