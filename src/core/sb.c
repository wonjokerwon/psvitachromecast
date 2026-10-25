#include "sb.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sb_init(sb_t *sb)
{
    sb->buf = NULL;
    sb->len = 0;
    sb->cap = 0;
    sb->oom = 0;
}

void sb_free(sb_t *sb)
{
    free(sb->buf);
    sb_init(sb);
}

void sb_reset(sb_t *sb)
{
    sb->len = 0;
    if (sb->buf)
        sb->buf[0] = '\0';
}

int sb_reserve(sb_t *sb, size_t extra)
{
    size_t need = sb->len + extra + 1;
    if (sb->oom)
        return -1;
    if (need <= sb->cap)
        return 0;
    size_t cap = sb->cap ? sb->cap : 64;
    while (cap < need)
        cap *= 2;
    char *nb = realloc(sb->buf, cap);
    if (!nb) {
        sb->oom = 1;
        return -1;
    }
    sb->buf = nb;
    sb->cap = cap;
    return 0;
}

void sb_putn(sb_t *sb, const void *data, size_t n)
{
    if (sb_reserve(sb, n) < 0)
        return;
    if (n)
        memcpy(sb->buf + sb->len, data, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

void sb_puts(sb_t *sb, const char *s)
{
    sb_putn(sb, s, strlen(s));
}

void sb_putc(sb_t *sb, char c)
{
    sb_putn(sb, &c, 1);
}

void sb_printf(sb_t *sb, const char *fmt, ...)
{
    va_list ap;
    char tmp[256];

    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n < sizeof(tmp)) {
        sb_putn(sb, tmp, (size_t)n);
        return;
    }
    if (sb_reserve(sb, (size_t)n) < 0)
        return;
    va_start(ap, fmt);
    vsnprintf(sb->buf + sb->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sb->len += (size_t)n;
}

void sb_json_str(sb_t *sb, const char *s)
{
    if (!s) {
        sb_puts(sb, "null");
        return;
    }
    sb_putc(sb, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"': sb_puts(sb, "\\\""); break;
        case '\\': sb_puts(sb, "\\\\"); break;
        case '\n': sb_puts(sb, "\\n"); break;
        case '\r': sb_puts(sb, "\\r"); break;
        case '\t': sb_puts(sb, "\\t"); break;
        case '\b': sb_puts(sb, "\\b"); break;
        case '\f': sb_puts(sb, "\\f"); break;
        default:
            if (*p < 0x20)
                sb_printf(sb, "\\u%04x", *p);
            else
                sb_putc(sb, (char)*p);
        }
    }
    sb_putc(sb, '"');
}

void sb_json_num(sb_t *sb, double v)
{
    if (isnan(v) || isinf(v)) {
        sb_putc(sb, '0');
        return;
    }
    if (v == floor(v) && fabs(v) < 1e15)
        sb_printf(sb, "%.0f", v);
    else
        sb_printf(sb, "%.10g", v);
}

void sb_consume(sb_t *sb, size_t n)
{
    if (n >= sb->len) {
        sb_reset(sb);
        return;
    }
    memmove(sb->buf, sb->buf + n, sb->len - n);
    sb->len -= n;
    sb->buf[sb->len] = '\0';
}
