/* Small JSON DOM parser/serializer, enough for Cast control messages. */
#ifndef VITACAST_JSON_H
#define VITACAST_JSON_H

#include <stddef.h>

#include "sb.h"

typedef enum {
    JSON_NULL,
    JSON_FALSE,
    JSON_TRUE,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} json_type;

typedef struct json {
    json_type type;
    char *key;           /* member name when inside an object */
    char *str;           /* JSON_STRING value */
    double num;          /* JSON_NUMBER value */
    struct json *child;  /* first element/member of arrays and objects */
    struct json *next;   /* next sibling */
} json;

json *json_parse(const char *text, size_t len);
void json_free(json *j);

json *json_get(const json *obj, const char *key);
/* Unlinks member `key` from obj and returns it (caller frees). */
json *json_detach(json *obj, const char *key);

const char *json_str(const json *j, const char *def);
double json_num(const json *j, double def);
int json_bool(const json *j, int def);
int json_is_number(const json *j);

void json_write(sb_t *sb, const json *j);
/* Writes "{" plus the members of obj except `skip_key` (may be NULL), without
 * the closing brace. Returns the number of members written. */
int json_write_obj_open(sb_t *sb, const json *obj, const char *skip_key);

#endif
