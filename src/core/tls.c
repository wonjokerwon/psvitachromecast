#include "tls.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/md.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/version.h>
#include <mbedtls/x509_crt.h>
#if MBEDTLS_VERSION_MAJOR >= 3 && defined(MBEDTLS_PSA_CRYPTO_C)
#include <psa/crypto.h>
#endif

#include "../platform/platform.h"
#include "castmsg.h"
#include "log.h"
#include "net.h"

#define IO_TIMEOUT_MS 15000

static mbedtls_entropy_context g_entropy;
static mbedtls_ctr_drbg_context g_drbg;
static pthread_mutex_t g_rng_lock = PTHREAD_MUTEX_INITIALIZER;
static mbedtls_x509_crt g_cert;
static mbedtls_pk_context g_key;
static mbedtls_ssl_config g_srv_conf;
static mbedtls_ssl_config g_cli_conf;
static int g_ready;

static int entropy_source(void *data, unsigned char *out, size_t len, size_t *olen)
{
    (void)data;
    if (plat_random(out, len) != 0)
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    *olen = len;
    return 0;
}

static int rng(void *ctx, unsigned char *out, size_t len)
{
    (void)ctx;
    pthread_mutex_lock(&g_rng_lock);
    int rc = mbedtls_ctr_drbg_random(&g_drbg, out, len);
    pthread_mutex_unlock(&g_rng_lock);
    return rc;
}

int tls_random(void *buf, size_t len)
{
    return rng(NULL, buf, len) == 0 ? 0 : -1;
}

static int write_file(const char *path, const unsigned char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    size_t n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len ? 0 : -1;
}

static int parse_key_file(const char *path)
{
#if MBEDTLS_VERSION_MAJOR >= 3
    return mbedtls_pk_parse_keyfile(&g_key, path, NULL, rng, NULL);
#else
    return mbedtls_pk_parse_keyfile(&g_key, path, NULL);
#endif
}

static int generate_identity(const char *key_path, const char *crt_path, const char *cn)
{
    mbedtls_x509write_cert crt;
    unsigned char *buf = NULL;
    unsigned char serial[16];
    char subject[160];
    int rc;

    LOGI("Generating RSA-2048 device key (first run, please wait)...");
    mbedtls_pk_free(&g_key);
    mbedtls_pk_init(&g_key);
    rc = mbedtls_pk_setup(&g_key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    if (rc == 0)
        rc = mbedtls_rsa_gen_key(mbedtls_pk_rsa(g_key), rng, NULL, 2048, 65537);
    if (rc != 0) {
        LOGI("RSA key generation failed (-0x%04x)", (unsigned)-rc);
        return -1;
    }

    buf = malloc(8192);
    if (!buf)
        return -1;

    rng(NULL, serial, sizeof(serial));
    serial[0] &= 0x7F; /* positive */
    serial[0] |= 0x01;
    snprintf(subject, sizeof(subject), "CN=%s,O=VitaCast", cn);

    mbedtls_x509write_crt_init(&crt);
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &g_key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &g_key);
    rc = mbedtls_x509write_crt_set_subject_name(&crt, subject);
    if (rc == 0)
        rc = mbedtls_x509write_crt_set_issuer_name(&crt, subject);
#if MBEDTLS_VERSION_NUMBER >= 0x03040000
    if (rc == 0)
        rc = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial));
#else
    {
        mbedtls_mpi mpi;
        mbedtls_mpi_init(&mpi);
        if (rc == 0)
            rc = mbedtls_mpi_read_binary(&mpi, serial, sizeof(serial));
        if (rc == 0)
            rc = mbedtls_x509write_crt_set_serial(&crt, &mpi);
        mbedtls_mpi_free(&mpi);
    }
#endif
    if (rc == 0)
        rc = mbedtls_x509write_crt_set_validity(&crt, "20240101000000", "20491231235959");
    if (rc == 0)
        rc = mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);
    if (rc == 0)
        rc = mbedtls_x509write_crt_pem(&crt, buf, 8192, rng, NULL);
    mbedtls_x509write_crt_free(&crt);
    if (rc == 0)
        rc = write_file(crt_path, buf, strlen((char *)buf));
    if (rc == 0)
        rc = mbedtls_x509_crt_parse(&g_cert, buf, strlen((char *)buf) + 1);
    if (rc == 0) {
        memset(buf, 0, 8192);
        rc = mbedtls_pk_write_key_pem(&g_key, buf, 8192);
        if (rc == 0)
            rc = write_file(key_path, buf, strlen((char *)buf));
    }
    memset(buf, 0, 8192);
    free(buf);
    if (rc != 0) {
        LOGI("Certificate generation failed (-0x%04x)", (unsigned)-rc);
        return -1;
    }
    LOGI("Device key generated");
    return 0;
}

static void conf_common(mbedtls_ssl_config *conf)
{
    mbedtls_ssl_conf_rng(conf, rng, NULL);
#if MBEDTLS_VERSION_MAJOR >= 3
    /* TLS 1.2 is supported by every Cast sender and keeps one code path. */
    mbedtls_ssl_conf_max_tls_version(conf, MBEDTLS_SSL_VERSION_TLS1_2);
#endif
}

int tls_init(const char *data_dir, const char *common_name)
{
    char key_path[512], crt_path[512];
    static const char pers[] = "vitacast";
    int rc;

    if (g_ready)
        return 0;

#if MBEDTLS_VERSION_MAJOR >= 3 && defined(MBEDTLS_PSA_CRYPTO_C)
    psa_crypto_init();
#endif
    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_drbg);
    mbedtls_x509_crt_init(&g_cert);
    mbedtls_pk_init(&g_key);
    mbedtls_ssl_config_init(&g_srv_conf);
    mbedtls_ssl_config_init(&g_cli_conf);

    mbedtls_entropy_add_source(&g_entropy, entropy_source, NULL, 32, MBEDTLS_ENTROPY_SOURCE_STRONG);
    rc = mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy,
                               (const unsigned char *)pers, sizeof(pers) - 1);
    if (rc != 0) {
        LOGI("RNG seeding failed (-0x%04x)", (unsigned)-rc);
        return -1;
    }

    snprintf(key_path, sizeof(key_path), "%s/device_key.pem", data_dir);
    snprintf(crt_path, sizeof(crt_path), "%s/device_cert.pem", data_dir);
    if (parse_key_file(key_path) != 0 || mbedtls_x509_crt_parse_file(&g_cert, crt_path) != 0) {
        mbedtls_x509_crt_free(&g_cert);
        mbedtls_x509_crt_init(&g_cert);
        if (generate_identity(key_path, crt_path, common_name) != 0)
            return -1;
    }

    rc = mbedtls_ssl_config_defaults(&g_srv_conf, MBEDTLS_SSL_IS_SERVER,
                                     MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc == 0) {
        conf_common(&g_srv_conf);
        rc = mbedtls_ssl_conf_own_cert(&g_srv_conf, &g_cert, &g_key);
    }
    if (rc == 0)
        rc = mbedtls_ssl_config_defaults(&g_cli_conf, MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc == 0) {
        conf_common(&g_cli_conf);
        mbedtls_ssl_conf_authmode(&g_cli_conf, MBEDTLS_SSL_VERIFY_NONE);
    }
    if (rc != 0) {
        LOGI("TLS configuration failed (-0x%04x)", (unsigned)-rc);
        return -1;
    }
    g_ready = 1;
    return 0;
}

void tls_shutdown(void)
{
    if (!g_ready)
        return;
    mbedtls_ssl_config_free(&g_srv_conf);
    mbedtls_ssl_config_free(&g_cli_conf);
    mbedtls_x509_crt_free(&g_cert);
    mbedtls_pk_free(&g_key);
    mbedtls_ctr_drbg_free(&g_drbg);
    mbedtls_entropy_free(&g_entropy);
    g_ready = 0;
}

const uint8_t *tls_cert_der(size_t *len)
{
    *len = g_cert.raw.len;
    return g_cert.raw.p;
}

int tls_sign(int hash_alg, const uint8_t *data, size_t len, uint8_t *sig, size_t sig_cap,
             size_t *sig_len)
{
    unsigned char hash[32];
    mbedtls_md_type_t md = hash_alg == AUTH_HASH_SHA256 ? MBEDTLS_MD_SHA256 : MBEDTLS_MD_SHA1;
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(md);
    int rc;

    if (!info || mbedtls_md(info, data, len, hash) != 0)
        return -1;
    if (sig_cap < mbedtls_pk_get_len(&g_key))
        return -1;
#if MBEDTLS_VERSION_MAJOR >= 3
    rc = mbedtls_pk_sign(&g_key, md, hash, mbedtls_md_get_size(info), sig, sig_cap, sig_len,
                         rng, NULL);
#else
    rc = mbedtls_pk_sign(&g_key, md, hash, mbedtls_md_get_size(info), sig, sig_len, rng, NULL);
#endif
    return rc == 0 ? 0 : -1;
}

/* --- streams --- */

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    tstream *ts = ctx;
    ssize_t n = send(ts->fd, buf, len, 0
#ifdef MSG_NOSIGNAL
                                           | MSG_NOSIGNAL
#endif
    );
    if (n < 0)
        return MBEDTLS_ERR_NET_SEND_FAILED;
    return (int)n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    tstream *ts = ctx;
    int n = net_recv(ts->fd, buf, len, IO_TIMEOUT_MS);
    if (n == NET_TIMEOUT)
        return MBEDTLS_ERR_SSL_TIMEOUT;
    if (n < 0)
        return MBEDTLS_ERR_NET_RECV_FAILED;
    return n;
}

static int handshake(tstream *ts)
{
    int rc;
    do {
        rc = mbedtls_ssl_handshake(&ts->ssl);
    } while (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE);
    return rc;
}

int tstream_accept(tstream *ts, int fd, int use_tls)
{
    memset(ts, 0, sizeof(*ts));
    ts->fd = fd;
    ts->tls = use_tls;
    if (!use_tls)
        return 0;
    mbedtls_ssl_init(&ts->ssl);
    int rc = mbedtls_ssl_setup(&ts->ssl, &g_srv_conf);
    if (rc == 0) {
        mbedtls_ssl_set_bio(&ts->ssl, ts, bio_send, bio_recv, NULL);
        rc = handshake(ts);
    }
    if (rc != 0) {
        LOGI("TLS handshake failed (-0x%04x)", (unsigned)-rc);
        tstream_close(ts);
        return -1;
    }
    return 0;
}

int tstream_connect(tstream *ts, const char *host, uint16_t port, int use_tls, int timeout_ms)
{
    memset(ts, 0, sizeof(*ts));
    ts->fd = net_tcp_connect(host, port, timeout_ms);
    if (ts->fd < 0)
        return -1;
    ts->tls = use_tls;
    if (!use_tls)
        return 0;
    mbedtls_ssl_init(&ts->ssl);
    int rc = mbedtls_ssl_setup(&ts->ssl, &g_cli_conf);
    if (rc == 0)
        rc = mbedtls_ssl_set_hostname(&ts->ssl, host);
    if (rc == 0) {
        mbedtls_ssl_set_bio(&ts->ssl, ts, bio_send, bio_recv, NULL);
        rc = handshake(ts);
    }
    if (rc != 0) {
        LOGI("TLS connect to %s failed (-0x%04x)", host, (unsigned)-rc);
        tstream_close(ts);
        return -1;
    }
    return 0;
}

int tstream_read(tstream *ts, void *buf, size_t len, int timeout_ms)
{
    if (!ts->tls)
        return net_recv(ts->fd, buf, len, timeout_ms);
    if (mbedtls_ssl_get_bytes_avail(&ts->ssl) == 0) {
        int w = net_wait_readable(ts->fd, timeout_ms);
        if (w == 0)
            return NET_TIMEOUT;
        if (w < 0)
            return -1;
    }
    int rc = mbedtls_ssl_read(&ts->ssl, buf, len);
    if (rc > 0)
        return rc;
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
        return NET_TIMEOUT;
    if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == MBEDTLS_ERR_SSL_CONN_EOF)
        return 0;
    return -1;
}

int tstream_write(tstream *ts, const void *buf, size_t len)
{
    const unsigned char *p = buf;
    if (!ts->tls)
        return net_send_all(ts->fd, buf, len);
    while (len) {
        int rc = mbedtls_ssl_write(&ts->ssl, p, len);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (rc <= 0)
            return -1;
        p += rc;
        len -= (size_t)rc;
    }
    return 0;
}

void tstream_abort(tstream *ts)
{
    net_shutdown(ts->fd);
}

void tstream_close(tstream *ts)
{
    if (ts->tls) {
        mbedtls_ssl_free(&ts->ssl);
        ts->tls = 0;
    }
    if (ts->fd >= 0)
        net_close(ts->fd);
    ts->fd = -1;
}
