#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "../platform/platform.h"
#include "log.h"

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
    size_t i = 0;
    while (s[i] && isspace((unsigned char)s[i]))
        i++;
    if (i)
        memmove(s, s + i, n - i + 1);
}

static int valid_uuid(const char *u)
{
    if (strlen(u) != 36)
        return 0;
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (u[i] != '-')
                return 0;
        } else if (!isxdigit((unsigned char)u[i])) {
            return 0;
        }
    }
    return 1;
}

static void derive_hex(vc_config *cfg)
{
    int o = 0;
    for (const char *p = cfg->uuid; *p && o < 32; p++)
        if (*p != '-')
            cfg->uuid_hex[o++] = (char)tolower((unsigned char)*p);
    cfg->uuid_hex[o] = '\0';
}

static void new_uuid(vc_config *cfg)
{
    unsigned char b[16];
    if (plat_random(b, sizeof(b)) != 0) {
        uint64_t t = plat_ticks_ms();
        for (int i = 0; i < 16; i++)
            b[i] = (unsigned char)(t >> ((i % 8) * 8)) ^ (unsigned char)(i * 37);
    }
    b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);
    snprintf(cfg->uuid, sizeof(cfg->uuid),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12],
             b[13], b[14], b[15]);
}

int config_save(const char *data_dir, const vc_config *cfg)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/config.txt", data_dir);
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "# VitaCast settings. Restart VitaCast after editing.\n");
    fprintf(f, "name=%s\n", cfg->name);
    fprintf(f, "uuid=%s\n", cfg->uuid);
    fclose(f);
    return 0;
}

int config_load(const char *data_dir, const char *default_name, vc_config *cfg)
{
    char path[512], line[256];
    int dirty = 0;

    memset(cfg, 0, sizeof(*cfg));
    snprintf(path, sizeof(path), "%s/config.txt", data_dir);
    FILE *f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            trim(line);
            if (line[0] == '#' || !line[0])
                continue;
            char *eq = strchr(line, '=');
            if (!eq)
                continue;
            *eq = '\0';
            char *k = line, *v = eq + 1;
            trim(k);
            trim(v);
            if (strcmp(k, "name") == 0)
                snprintf(cfg->name, sizeof(cfg->name), "%s", v);
            else if (strcmp(k, "uuid") == 0 && valid_uuid(v))
                snprintf(cfg->uuid, sizeof(cfg->uuid), "%s", v);
        }
        fclose(f);
    }
    if (!cfg->name[0]) {
        snprintf(cfg->name, sizeof(cfg->name), "%s", default_name);
        dirty = 1;
    }
    if (!cfg->uuid[0]) {
        new_uuid(cfg);
        dirty = 1;
    }
    derive_hex(cfg);
    if (dirty && config_save(data_dir, cfg) != 0)
        LOGI("Warning: cannot write %s", path);
    return 0;
}
