/* Functions every port (host, vita) must provide to the portable core. */
#ifndef VITACAST_PLATFORM_H
#define VITACAST_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

/* Monotonic milliseconds. */
uint64_t plat_ticks_ms(void);
void plat_sleep_ms(int ms);

/* Directory that holds config, keys and logs (no trailing slash). */
const char *plat_data_dir(void);

/* Writes the primary IPv4 address as dotted quad. Returns 0 on success. */
int plat_local_ipv4(char out[16]);

/* Fills buf with cryptographically secure random bytes. Returns 0 on success. */
int plat_random(void *buf, size_t len);

/* Sink for log lines (already formatted, no trailing newline). */
void plat_log_write(const char *line);

#endif
