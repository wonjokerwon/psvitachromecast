#ifndef VITACAST_UI_H
#define VITACAST_UI_H

#include "../core/receiver.h"

typedef enum {
    UI_PHASE_KEYS,   /* generating the device key on first run */
    UI_PHASE_WIFI,   /* waiting for a network connection */
    UI_PHASE_READY,  /* receiver running */
    UI_PHASE_ERROR
} ui_phase;

typedef struct ui_model {
    ui_phase phase;
    const char *name;
    const char *ip;
    const char *message;
    receiver_ui rx;
    int show_overlay;
    int show_help;
} ui_model;

int ui_init(void);
void ui_shutdown(void);
void ui_draw(const ui_model *m);

#endif
