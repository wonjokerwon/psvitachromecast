/* Growable byte/string buffer. Binary safe; always NUL terminated. */
#ifndef VITACAST_SB_H
#define VITACAST_SB_H

#include <stddef.h>
#include <stdint.h>

typedef struct sb {
    char *buf;
    size_t len;
    size_t cap;
    int oom;
} sb_t;

void sb_init(sb_t *sb);
void sb_free(sb_t *sb);
void sb_reset(sb_t *sb);
int sb_reserve(sb_t *sb, size_t extra);
void sb_putn(sb_t *sb, const void *data, size_t n);
void sb_puts(sb_t *sb, const char *s);
void sb_putc(sb_t *sb, char c);
void sb_printf(sb_t *sb, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
/* Appends s as a quoted, escaped JSON string ("null" if s is NULL). */
void sb_json_str(sb_t *sb, const char *s);
/* Appends a JSON number (integers without a fraction, NaN/inf as 0). */
void sb_json_num(sb_t *sb, double v);
/* Removes the first n bytes. */
void sb_consume(sb_t *sb, size_t n);

#endif
