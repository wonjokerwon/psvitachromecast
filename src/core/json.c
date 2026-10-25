#include "json.h"

#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 64

typedef struct {
    const char *p;
    const char *end;
    int depth;
} jparser;

static json *jnew(json_type t)
{
    json *j = calloc(1, sizeof(*j));
    if (j)
        j->type = t;
    return j;
}

void json_free(json *j)
{
    while (j) {
        json *next = j->next;
        json_free(j->child);
        free(j->key);
        free(j->str);
        free(j);
        j = next;
    }
}

static void skip_ws(jparser *ps)
{
    while (ps->p < ps->end &&
           (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int read_hex4(jparser *ps, unsigned *out)
{
    if (ps->end - ps->p < 4)
        return -1;
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexval(ps->p[i]);
        if (h < 0)
            return -1;
        v = (v << 4) | (unsigned)h;
    }
    ps->p += 4;
    *out = v;
    return 0;
}

static void put_utf8(sb_t *sb, unsigned cp)
{
    if (cp < 0x80) {
        sb_putc(sb, (char)cp);
    } else if (cp < 0x800) {
        sb_putc(sb, (char)(0xC0 | (cp >> 6)));
        sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        sb_putc(sb, (char)(0xE0 | (cp >> 12)));
        sb_putc(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
    } else {
        sb_putc(sb, (char)(0xF0 | (cp >> 18)));
        sb_putc(sb, (char)(0x80 | ((cp >> 12) & 0x3F)));
        sb_putc(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
    }
}

/* Parses a string literal at ps->p (which points at the opening quote). */
static char *parse_string(jparser *ps)
{
    sb_t sb;
    sb_init(&sb);
    ps->p++;
    while (ps->p < ps->end) {
        char c = *ps->p++;
        if (c == '"') {
            if (!sb.buf)
                sb_puts(&sb, "");
            if (sb.oom) {
                sb_free(&sb);
                return NULL;
            }
            return sb.buf;
        }
        if ((unsigned char)c < 0x20)
            break;
        if (c != '\\') {
            sb_putc(&sb, c);
            continue;
        }
        if (ps->p >= ps->end)
            break;
        c = *ps->p++;
        switch (c) {
        case '"': sb_putc(&sb, '"'); break;
        case '\\': sb_putc(&sb, '\\'); break;
        case '/': sb_putc(&sb, '/'); break;
        case 'b': sb_putc(&sb, '\b'); break;
        case 'f': sb_putc(&sb, '\f'); break;
        case 'n': sb_putc(&sb, '\n'); break;
        case 'r': sb_putc(&sb, '\r'); break;
        case 't': sb_putc(&sb, '\t'); break;
        case 'u': {
            unsigned cp, lo;
            if (read_hex4(ps, &cp) < 0)
                goto fail;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if (ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                    ps->p += 2;
                    if (read_hex4(ps, &lo) < 0)
                        goto fail;
                    if (lo >= 0xDC00 && lo <= 0xDFFF)
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else
                        cp = 0xFFFD;
                } else {
                    cp = 0xFFFD;
                }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                cp = 0xFFFD;
            }
            if (cp == 0)
                cp = 0xFFFD; /* keep strings NUL-free */
            put_utf8(&sb, cp);
            break;
        }
        default:
            goto fail;
        }
    }
fail:
    sb_free(&sb);
    return NULL;
}

static json *parse_value(jparser *ps);

static json *parse_container(jparser *ps, int is_obj)
{
    json *node = jnew(is_obj ? JSON_OBJECT : JSON_ARRAY);
    json **tail;
    char close = is_obj ? '}' : ']';

    if (!node)
        return NULL;
    tail = &node->child;
    if (++ps->depth > JSON_MAX_DEPTH)
        goto fail;
    ps->p++;
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == close) {
        ps->p++;
        ps->depth--;
        return node;
    }
    for (;;) {
        char *key = NULL;
        skip_ws(ps);
        if (is_obj) {
            if (ps->p >= ps->end || *ps->p != '"')
                goto fail;
            key = parse_string(ps);
            if (!key)
                goto fail;
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') {
                free(key);
                goto fail;
            }
            ps->p++;
        }
        json *v = parse_value(ps);
        if (!v) {
            free(key);
            goto fail;
        }
        v->key = key;
        *tail = v;
        tail = &v->next;
        skip_ws(ps);
        if (ps->p >= ps->end)
            goto fail;
        if (*ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == close) {
            ps->p++;
            ps->depth--;
            return node;
        }
        goto fail;
    }
fail:
    json_free(node);
    return NULL;
}

static int match_lit(jparser *ps, const char *lit)
{
    size_t n = strlen(lit);
    if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, lit, n) != 0)
        return 0;
    ps->p += n;
    return 1;
}

static json *parse_number(jparser *ps)
{
    char tmp[64];
    size_t n = 0;
    const char *s = ps->p;

    while (s < ps->end && n < sizeof(tmp) - 1 &&
           (strchr("+-0123456789.eE", *s) != NULL)) {
        tmp[n++] = *s++;
    }
    tmp[n] = '\0';
    if (n == 0)
        return NULL;
    char *endp;
    double v = strtod(tmp, &endp);
    if (endp == tmp)
        return NULL;
    ps->p += (endp - tmp);
    json *j = jnew(JSON_NUMBER);
    if (j)
        j->num = v;
    return j;
}

static json *parse_value(jparser *ps)
{
    skip_ws(ps);
    if (ps->p >= ps->end)
        return NULL;
    switch (*ps->p) {
    case '{':
        return parse_container(ps, 1);
    case '[':
        return parse_container(ps, 0);
    case '"': {
        char *s = parse_string(ps);
        if (!s)
            return NULL;
        json *j = jnew(JSON_STRING);
        if (!j) {
            free(s);
            return NULL;
        }
        j->str = s;
        return j;
    }
    case 't':
        return match_lit(ps, "true") ? jnew(JSON_TRUE) : NULL;
    case 'f':
        return match_lit(ps, "false") ? jnew(JSON_FALSE) : NULL;
    case 'n':
        return match_lit(ps, "null") ? jnew(JSON_NULL) : NULL;
    default:
        return parse_number(ps);
    }
}

json *json_parse(const char *text, size_t len)
{
    jparser ps = { text, text + len, 0 };
    json *j = parse_value(&ps);
    if (!j)
        return NULL;
    skip_ws(&ps);
    if (ps.p != ps.end) {
        json_free(j);
        return NULL;
    }
    return j;
}

json *json_get(const json *obj, const char *key)
{
    if (!obj || obj->type != JSON_OBJECT)
        return NULL;
    for (json *c = obj->child; c; c = c->next)
        if (c->key && strcmp(c->key, key) == 0)
            return c;
    return NULL;
}

json *json_detach(json *obj, const char *key)
{
    if (!obj || obj->type != JSON_OBJECT)
        return NULL;
    for (json **pp = &obj->child; *pp; pp = &(*pp)->next) {
        json *c = *pp;
        if (c->key && strcmp(c->key, key) == 0) {
            *pp = c->next;
            c->next = NULL;
            return c;
        }
    }
    return NULL;
}

const char *json_str(const json *j, const char *def)
{
    return (j && j->type == JSON_STRING) ? j->str : def;
}

double json_num(const json *j, double def)
{
    return (j && j->type == JSON_NUMBER) ? j->num : def;
}

int json_bool(const json *j, int def)
{
    if (!j)
        return def;
    if (j->type == JSON_TRUE)
        return 1;
    if (j->type == JSON_FALSE)
        return 0;
    if (j->type == JSON_NUMBER)
        return j->num != 0;
    return def;
}

int json_is_number(const json *j)
{
    return j && j->type == JSON_NUMBER;
}

int json_write_obj_open(sb_t *sb, const json *obj, const char *skip_key)
{
    int n = 0;
    sb_putc(sb, '{');
    if (!obj || obj->type != JSON_OBJECT)
        return 0;
    for (const json *c = obj->child; c; c = c->next) {
        if (skip_key && c->key && strcmp(c->key, skip_key) == 0)
            continue;
        if (n++)
            sb_putc(sb, ',');
        sb_json_str(sb, c->key ? c->key : "");
        sb_putc(sb, ':');
        json_write(sb, c);
    }
    return n;
}

void json_write(sb_t *sb, const json *j)
{
    if (!j) {
        sb_puts(sb, "null");
        return;
    }
    switch (j->type) {
    case JSON_NULL: sb_puts(sb, "null"); break;
    case JSON_FALSE: sb_puts(sb, "false"); break;
    case JSON_TRUE: sb_puts(sb, "true"); break;
    case JSON_NUMBER: sb_json_num(sb, j->num); break;
    case JSON_STRING: sb_json_str(sb, j->str); break;
    case JSON_ARRAY: {
        int n = 0;
        sb_putc(sb, '[');
        for (const json *c = j->child; c; c = c->next) {
            if (n++)
                sb_putc(sb, ',');
            json_write(sb, c);
        }
        sb_putc(sb, ']');
        break;
    }
    case JSON_OBJECT:
        json_write_obj_open(sb, j, NULL);
        sb_putc(sb, '}');
        break;
    }
}
