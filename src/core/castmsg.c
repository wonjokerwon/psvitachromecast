#include "castmsg.h"

#include <stdlib.h>
#include <string.h>

#define WT_VARINT 0
#define WT_FIXED64 1
#define WT_LEN 2
#define WT_FIXED32 5

static int read_varint(pb_reader *r, uint64_t *out)
{
    uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (r->p >= r->end)
            return -1;
        uint8_t b = *r->p++;
        v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) {
            *out = v;
            return 0;
        }
    }
    return -1;
}

int pb_next(pb_reader *r, pb_field *f)
{
    uint64_t key;

    if (r->p >= r->end)
        return 0;
    if (read_varint(r, &key) < 0)
        return -1;
    memset(f, 0, sizeof(*f));
    f->num = (uint32_t)(key >> 3);
    f->wire_type = (uint32_t)(key & 7);
    switch (f->wire_type) {
    case WT_VARINT:
        return read_varint(r, &f->varint) < 0 ? -1 : 1;
    case WT_FIXED64:
        if (r->end - r->p < 8)
            return -1;
        f->data = r->p;
        f->len = 8;
        r->p += 8;
        return 1;
    case WT_FIXED32:
        if (r->end - r->p < 4)
            return -1;
        f->data = r->p;
        f->len = 4;
        r->p += 4;
        return 1;
    case WT_LEN: {
        uint64_t n;
        if (read_varint(r, &n) < 0 || n > (uint64_t)(r->end - r->p))
            return -1;
        f->data = r->p;
        f->len = (size_t)n;
        r->p += n;
        return 1;
    }
    default:
        return -1;
    }
}

void pb_put_varint(sb_t *sb, uint64_t v)
{
    uint8_t tmp[10];
    size_t n = 0;
    do {
        uint8_t b = v & 0x7F;
        v >>= 7;
        if (v)
            b |= 0x80;
        tmp[n++] = b;
    } while (v);
    sb_putn(sb, tmp, n);
}

void pb_put_uint(sb_t *sb, uint32_t field, uint64_t v)
{
    pb_put_varint(sb, ((uint64_t)field << 3) | WT_VARINT);
    pb_put_varint(sb, v);
}

void pb_put_bytes(sb_t *sb, uint32_t field, const void *data, size_t len)
{
    pb_put_varint(sb, ((uint64_t)field << 3) | WT_LEN);
    pb_put_varint(sb, len);
    sb_putn(sb, data, len);
}

static char *dup_bytes(const uint8_t *d, size_t n)
{
    char *s = malloc(n + 1);
    if (!s)
        return NULL;
    if (n)
        memcpy(s, d, n);
    s[n] = '\0';
    return s;
}

void cast_msg_free(cast_msg *m)
{
    free(m->source_id);
    free(m->destination_id);
    free(m->ns);
    free(m->payload);
    memset(m, 0, sizeof(*m));
}

int cast_msg_decode(const uint8_t *data, size_t len, cast_msg *out)
{
    pb_reader r = { data, data + len };
    pb_field f;
    int rc;

    memset(out, 0, sizeof(*out));
    while ((rc = pb_next(&r, &f)) == 1) {
        char **dst = NULL;
        switch (f.num) {
        case 1:
            out->protocol_version = (int)f.varint;
            break;
        case 2: dst = &out->source_id; break;
        case 3: dst = &out->destination_id; break;
        case 4: dst = &out->ns; break;
        case 5:
            out->payload_type = (int)f.varint;
            break;
        case 6:
        case 7:
            if (f.wire_type != WT_LEN)
                goto fail;
            free(out->payload);
            out->payload = (uint8_t *)dup_bytes(f.data, f.len);
            if (!out->payload)
                goto fail;
            out->payload_len = f.len;
            break;
        default:
            break;
        }
        if (dst) {
            if (f.wire_type != WT_LEN)
                goto fail;
            free(*dst);
            *dst = dup_bytes(f.data, f.len);
            if (!*dst)
                goto fail;
        }
    }
    if (rc < 0 || !out->source_id || !out->destination_id || !out->ns)
        goto fail;
    if (!out->payload) {
        out->payload = (uint8_t *)dup_bytes((const uint8_t *)"", 0);
        if (!out->payload)
            goto fail;
    }
    return 0;
fail:
    cast_msg_free(out);
    return -1;
}

int cast_msg_encode_framed(sb_t *out, const char *source_id, const char *destination_id,
                           const char *ns, int payload_type, const void *payload,
                           size_t payload_len)
{
    sb_t body;
    sb_init(&body);
    pb_put_uint(&body, 1, 0); /* CASTV2_1_0 */
    pb_put_bytes(&body, 2, source_id, strlen(source_id));
    pb_put_bytes(&body, 3, destination_id, strlen(destination_id));
    pb_put_bytes(&body, 4, ns, strlen(ns));
    pb_put_uint(&body, 5, (uint64_t)payload_type);
    pb_put_bytes(&body, payload_type == CAST_PAYLOAD_BINARY ? 7 : 6, payload, payload_len);
    if (body.oom || body.len > CAST_MAX_MESSAGE) {
        sb_free(&body);
        return -1;
    }
    uint8_t hdr[4] = {
        (uint8_t)(body.len >> 24), (uint8_t)(body.len >> 16),
        (uint8_t)(body.len >> 8), (uint8_t)body.len
    };
    sb_putn(out, hdr, 4);
    sb_putn(out, body.buf, body.len);
    sb_free(&body);
    return out->oom ? -1 : 0;
}

int auth_parse_challenge(const uint8_t *data, size_t len, auth_challenge *out)
{
    pb_reader r = { data, data + len };
    pb_field f;
    int rc;

    memset(out, 0, sizeof(*out));
    out->signature_algorithm = AUTH_SIG_RSASSA_PKCS1V15;
    out->hash_algorithm = AUTH_HASH_SHA1;
    while ((rc = pb_next(&r, &f)) == 1) {
        if (f.num != 1 || f.wire_type != WT_LEN)
            continue;
        pb_reader cr = { f.data, f.data + f.len };
        pb_field cf;
        int crc;
        out->present = 1;
        while ((crc = pb_next(&cr, &cf)) == 1) {
            if (cf.num == 1 && cf.wire_type == WT_VARINT)
                out->signature_algorithm = (int)cf.varint;
            else if (cf.num == 2 && cf.wire_type == WT_LEN) {
                out->sender_nonce = cf.data;
                out->sender_nonce_len = cf.len;
            } else if (cf.num == 3 && cf.wire_type == WT_VARINT)
                out->hash_algorithm = (int)cf.varint;
        }
        if (crc < 0)
            return -1;
    }
    return rc < 0 ? -1 : 0;
}

void auth_build_response(sb_t *out, const uint8_t *sig, size_t sig_len,
                         const uint8_t *cert_der, size_t cert_len,
                         int signature_algorithm, int hash_algorithm,
                         const uint8_t *nonce, size_t nonce_len)
{
    sb_t resp;
    sb_init(&resp);
    pb_put_bytes(&resp, 1, sig, sig_len);
    pb_put_bytes(&resp, 2, cert_der, cert_len);
    pb_put_uint(&resp, 4, (uint64_t)signature_algorithm);
    if (nonce && nonce_len)
        pb_put_bytes(&resp, 5, nonce, nonce_len);
    pb_put_uint(&resp, 6, (uint64_t)hash_algorithm);
    pb_put_bytes(out, 2, resp.buf ? resp.buf : "", resp.len);
    sb_free(&resp);
}

void auth_build_error(sb_t *out, int error_type)
{
    sb_t err;
    sb_init(&err);
    pb_put_uint(&err, 1, (uint64_t)error_type);
    pb_put_bytes(out, 3, err.buf, err.len);
    sb_free(&err);
}
