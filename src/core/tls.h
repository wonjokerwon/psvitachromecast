/* TLS (mbedTLS) helpers: the receiver's identity, server/client configs and a
 * small stream abstraction over plain or TLS sockets. */
#ifndef VITACAST_TLS_H
#define VITACAST_TLS_H

#include <stddef.h>
#include <stdint.h>

#include <mbedtls/ssl.h>

/* Loads the device key/certificate from data_dir, generating and saving a
 * self-signed RSA-2048 pair on first run (takes a while on the Vita).
 * common_name is used for a newly generated certificate. */
int tls_init(const char *data_dir, const char *common_name);
/* Client-only setup (no device identity, so no slow key generation). */
int tls_init_client(void);
void tls_shutdown(void);

/* DER encoding of the certificate presented on the Cast TLS channel. */
const uint8_t *tls_cert_der(size_t *len);

/* Signs data with the device key (RSASSA-PKCS1-v1_5).
 * hash_alg is AUTH_HASH_SHA1 or AUTH_HASH_SHA256. Returns 0 on success. */
int tls_sign(int hash_alg, const uint8_t *data, size_t len, uint8_t *sig, size_t sig_cap,
             size_t *sig_len);

int tls_random(void *buf, size_t len);

typedef struct tstream {
    int fd;
    int tls;
    mbedtls_ssl_context ssl;
} tstream;

/* Wraps an accepted socket and runs the server handshake. On failure the fd
 * is closed. Returns 0 on success. */
int tstream_accept(tstream *ts, int fd, int use_tls);
/* Connects to host:port, optionally with TLS (server certificate is not
 * verified: the Vita has no usable CA store). Returns 0 on success. */
int tstream_connect(tstream *ts, const char *host, uint16_t port, int use_tls, int timeout_ms);
/* >0 bytes, 0 closed, NET_TIMEOUT, -1 error. */
int tstream_read(tstream *ts, void *buf, size_t len, int timeout_ms);
int tstream_write(tstream *ts, const void *buf, size_t len);
/* Makes blocked reads on another thread return (does not free). */
void tstream_abort(tstream *ts);
void tstream_close(tstream *ts);

#endif
