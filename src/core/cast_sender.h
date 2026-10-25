/* Minimal Cast *sender*: controls a real Chromecast / Google TV over the
 * CASTV2 TLS channel. Used by VitaCast Mirror to show the Vita's screen. */
#ifndef VITACAST_CAST_SENDER_H
#define VITACAST_CAST_SENDER_H

#include <stddef.h>
#include <stdint.h>

/* DashCast: a public Cast app that loads any web page on the TV. */
#define DASHCAST_APP_ID "84912283"
#define DASHCAST_NAMESPACE "urn:x-cast:com.madmod.dashcast"

/* Stops whatever runs on the device, launches DashCast and makes it
 * navigate to url. Returns 0 on success; err gets a reason otherwise. */
int cast_open_url(const char *ip, uint16_t port, const char *url, char *err, size_t errcap);

/* Stops the running app (back to the TV's idle screen). */
int cast_stop_app(const char *ip, uint16_t port, char *err, size_t errcap);

#endif
