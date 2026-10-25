/* Thin POSIX socket helpers. vitasdk's newlib provides the BSD socket API on
 * top of SceNet, so the same code runs on Linux, macOS and the PS Vita. */
#ifndef VITACAST_NET_H
#define VITACAST_NET_H

#include <stddef.h>
#include <stdint.h>

#define NET_TIMEOUT (-2)

int net_tcp_listen(uint16_t port);
/* Returns the accepted fd; fills peer (dotted quad) if not NULL. */
int net_accept(int listen_fd, char peer[16]);
/* Resolves host and connects with a timeout. Returns fd or -1. */
int net_tcp_connect(const char *host, uint16_t port, int timeout_ms);
/* Waits until fd is readable: 1 readable, 0 timeout, -1 error. */
int net_wait_readable(int fd, int timeout_ms);
/* recv() with a timeout: >0 bytes, 0 closed, NET_TIMEOUT, -1 error. */
int net_recv(int fd, void *buf, size_t len, int timeout_ms);
/* Sends everything; returns 0 or -1. */
int net_send_all(int fd, const void *buf, size_t len);
/* Unblocks any thread waiting on fd, then closes it. */
void net_close(int fd);
void net_shutdown(int fd);

/* UDP socket bound to port and joined to the IPv4 multicast group. */
int net_udp_multicast(uint16_t port, const char *group, const char *iface_ip);
int net_udp_sendto(int fd, const void *buf, size_t len, const char *ip, uint16_t port);
/* Returns bytes, NET_TIMEOUT or -1. from_ip/from_port may be NULL. */
int net_udp_recvfrom(int fd, void *buf, size_t len, int timeout_ms,
                     char from_ip[16], uint16_t *from_port);

#endif
