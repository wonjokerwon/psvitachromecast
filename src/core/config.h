#ifndef VITACAST_CONFIG_H
#define VITACAST_CONFIG_H

typedef struct vc_config {
    char name[64];     /* friendly name shown to senders */
    char uuid[37];     /* 8-4-4-4-12 */
    char uuid_hex[33]; /* same without dashes */
} vc_config;

/* Loads <data_dir>/config.txt, creating it with defaults (random UUID,
 * default_name) when missing. Returns 0 on success. */
int config_load(const char *data_dir, const char *default_name, vc_config *cfg);
int config_save(const char *data_dir, const vc_config *cfg);

#endif
