#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

int net_tcp_listen(uint16_t port)
{
    struct sockaddr_in addr;
    int one = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int net_accept(int listen_fd, char peer[16])
{
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    int fd = accept(listen_fd, (struct sockaddr *)&addr, &alen);
    if (fd < 0)
        return -1;
    if (peer) {
        if (!inet_ntop(AF_INET, &addr.sin_addr, peer, 16))
            snprintf(peer, 16, "?");
    }
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    return fd;
}

int net_wait_readable(int fd, int timeout_ms)
{
    struct pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    for (;;) {
        int rc = poll(&p, 1, timeout_ms);
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc < 0)
            return -1;
        if (rc == 0)
            return 0;
        return 1; /* readable, hung up or errored: let recv() tell which */
    }
}

int net_recv(int fd, void *buf, size_t len, int timeout_ms)
{
    int w = net_wait_readable(fd, timeout_ms);
    if (w == 0)
        return NET_TIMEOUT;
    if (w < 0)
        return -1;
    for (;;) {
        ssize_t n = recv(fd, buf, len, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return NET_TIMEOUT;
        return n < 0 ? -1 : (int)n;
    }
}

int net_send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

void net_shutdown(int fd)
{
    if (fd >= 0)
        shutdown(fd, SHUT_RDWR);
}

void net_close(int fd)
{
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
}

static int connect_with_timeout(int fd, const struct sockaddr *sa, socklen_t salen, int timeout_ms)
{
#ifdef __vita__
    /* The Vita socket layer does not support non-blocking connect through
     * fcntl reliably; SceNet applies its own connect timeout. */
    (void)timeout_ms;
    return connect(fd, sa, salen);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return connect(fd, sa, salen);
    int rc = connect(fd, sa, salen);
    if (rc < 0 && errno == EINPROGRESS) {
        struct pollfd p = { fd, POLLOUT, 0 };
        rc = poll(&p, 1, timeout_ms);
        if (rc <= 0) {
            rc = -1;
        } else {
            int err = 0;
            socklen_t elen = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
            rc = err ? -1 : 0;
        }
    }
    fcntl(fd, F_SETFL, flags);
    return rc;
#endif
}

int net_tcp_connect(const char *host, uint16_t port, int timeout_ms)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[8];
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
        return -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect_with_timeout(fd, ai->ai_addr, ai->ai_addrlen, timeout_ms) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
#ifdef SO_NOSIGPIPE
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    }
#endif
    return fd;
}

int net_udp_multicast(uint16_t port, const char *group, const char *iface_ip)
{
    struct sockaddr_in addr;
    struct ip_mreq mreq;
    struct in_addr iface;
    int one = 1;
#ifdef __vita__
    int ttl = 255, loop = 1; /* passed straight to sceNetSetsockopt */
#else
    unsigned char ttl = 255, loop = 1;
#endif

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    memset(&mreq, 0, sizeof(mreq));
    inet_pton(AF_INET, group, &mreq.imr_multiaddr);
    iface.s_addr = htonl(INADDR_ANY);
    if (iface_ip)
        inet_pton(AF_INET, iface_ip, &iface);
    mreq.imr_interface = iface;
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        /* Retry on the default interface. */
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
            close(fd);
            return -1;
        }
    }
    if (iface_ip)
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface));
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    return fd;
}

int net_udp_sendto(int fd, const void *buf, size_t len, const char *ip, uint16_t port)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1)
        return -1;
    ssize_t n = sendto(fd, buf, len, 0, (struct sockaddr *)&addr, sizeof(addr));
    return n < 0 ? -1 : (int)n;
}

int net_udp_recvfrom(int fd, void *buf, size_t len, int timeout_ms,
                     char from_ip[16], uint16_t *from_port)
{
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    int w = net_wait_readable(fd, timeout_ms);
    if (w == 0)
        return NET_TIMEOUT;
    if (w < 0)
        return -1;
    ssize_t n = recvfrom(fd, buf, len, 0, (struct sockaddr *)&addr, &alen);
    if (n < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? NET_TIMEOUT : -1;
    if (from_ip && !inet_ntop(AF_INET, &addr.sin_addr, from_ip, 16))
        from_ip[0] = '\0';
    if (from_port)
        *from_port = ntohs(addr.sin_port);
    return (int)n;
}
