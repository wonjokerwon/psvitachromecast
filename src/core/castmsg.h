/* CASTV2 wire format: CastMessage and DeviceAuthMessage protobufs.
 *
 * Only the handful of fields the Cast protocol uses are supported, with a
 * minimal hand written protobuf codec so there is no protobuf dependency.
 */
#ifndef VITACAST_CASTMSG_H
#define VITACAST_CASTMSG_H

#include <stddef.h>
#include <stdint.h>

#include "sb.h"

#define CAST_MAX_MESSAGE (64 * 1024)

#define CAST_PAYLOAD_STRING 0
#define CAST_PAYLOAD_BINARY 1

#define NS_CONNECTION "urn:x-cast:com.google.cast.tp.connection"
#define NS_HEARTBEAT  "urn:x-cast:com.google.cast.tp.heartbeat"
#define NS_DEVICEAUTH "urn:x-cast:com.google.cast.tp.deviceauth"
#define NS_RECEIVER   "urn:x-cast:com.google.cast.receiver"
#define NS_MEDIA      "urn:x-cast:com.google.cast.media"

typedef struct cast_msg {
    int protocol_version;
    char *source_id;
    char *destination_id;
    char *ns;
    int payload_type;
    uint8_t *payload; /* NUL terminated for convenience */
    size_t payload_len;
} cast_msg;

/* Decodes a CastMessage body (without the 4 byte length prefix). */
int cast_msg_decode(const uint8_t *data, size_t len, cast_msg *out);
void cast_msg_free(cast_msg *m);

/* Appends a framed CastMessage (4 byte big-endian length + body) to out. */
int cast_msg_encode_framed(sb_t *out, const char *source_id, const char *destination_id,
                           const char *ns, int payload_type, const void *payload,
                           size_t payload_len);

/* --- protobuf primitives (exposed for tests) --- */
typedef struct pb_reader {
    const uint8_t *p;
    const uint8_t *end;
} pb_reader;

typedef struct pb_field {
    uint32_t num;
    uint32_t wire_type;
    uint64_t varint;
    const uint8_t *data;
    size_t len;
} pb_field;

/* Returns 1 when a field was read, 0 at end of input, -1 on malformed input. */
int pb_next(pb_reader *r, pb_field *f);
void pb_put_varint(sb_t *sb, uint64_t v);
void pb_put_uint(sb_t *sb, uint32_t field, uint64_t v);
void pb_put_bytes(sb_t *sb, uint32_t field, const void *data, size_t len);

/* --- device authentication --- */
#define AUTH_SIG_RSASSA_PKCS1V15 1
#define AUTH_SIG_RSASSA_PSS      2
#define AUTH_HASH_SHA1   0
#define AUTH_HASH_SHA256 1

typedef struct auth_challenge {
    int present;
    int signature_algorithm;
    int hash_algorithm;
    const uint8_t *sender_nonce;
    size_t sender_nonce_len;
} auth_challenge;

/* Parses a DeviceAuthMessage; returns 0 on success. Pointers alias data. */
int auth_parse_challenge(const uint8_t *data, size_t len, auth_challenge *out);

/* Builds a DeviceAuthMessage carrying an AuthResponse. */
void auth_build_response(sb_t *out, const uint8_t *sig, size_t sig_len,
                         const uint8_t *cert_der, size_t cert_len,
                         int signature_algorithm, int hash_algorithm,
                         const uint8_t *nonce, size_t nonce_len);

/* Builds a DeviceAuthMessage carrying an AuthError. */
void auth_build_error(sb_t *out, int error_type);

#endif
