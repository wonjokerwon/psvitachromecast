/* Vita-only glue between main loop, player and UI. */
#ifndef VITACAST_VITA_H
#define VITACAST_VITA_H

#include <vita2d.h>

void vita_platform_init(void);
void vita_platform_shutdown(void);

/* Latest decoded video frame as a vita2d texture (NULL when there is none).
 * Call from the render thread between vita2d_start_drawing/end_drawing. */
const vita2d_texture *player_vita_video_frame(int *width, int *height);
/* True when the current media has produced video frames. */
int player_vita_has_video(void);
/* Call after vita2d_end_drawing(): lets the player free frame memory safely. */
void player_vita_frame_done(void);

#endif
