/* Media player backend interface. The Vita port implements it with
 * sceAvPlayer; the host port with a simulated player used for testing. */
#ifndef VITACAST_PLAYER_H
#define VITACAST_PLAYER_H

typedef enum {
    PLAYER_IDLE = 0,
    PLAYER_LOADING,   /* opening the URL / probing the container */
    PLAYER_BUFFERING,
    PLAYER_PLAYING,
    PLAYER_PAUSED
} player_state;

typedef enum {
    PLAYER_IDLE_NONE = 0,
    PLAYER_IDLE_FINISHED,
    PLAYER_IDLE_CANCELLED,
    PLAYER_IDLE_INTERRUPTED,
    PLAYER_IDLE_ERROR
} player_idle_reason;

typedef struct player_status {
    unsigned generation;      /* id returned by player_load() */
    player_state state;
    player_idle_reason idle_reason;
    double current_time;      /* seconds */
    double duration;          /* seconds, 0 when unknown */
    char error[128];
} player_status;

typedef struct player_media {
    const char *url;
    const char *content_type;
    const char *title;
    double start_time;
    double duration_hint;
    int autoplay;
} player_media;

int player_init(void);
void player_shutdown(void);

/* Starts loading asynchronously. Returns a positive generation id, or 0 when
 * the media is rejected outright (err is filled in). */
unsigned player_load(const player_media *m, char *err, int errcap);
void player_play(void);
void player_pause(void);
void player_seek(double seconds);
void player_stop(void);
void player_set_volume(double level, int muted);
void player_get_status(player_status *out);

#endif
