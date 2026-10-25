#ifndef VITACAST_LOG_H
#define VITACAST_LOG_H

#define LOG_RING_LINES 10
#define LOG_LINE_MAX 120

void log_printf(const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

/* Copies the most recent log lines (oldest first) for on-screen display.
 * Returns the number of lines copied. */
int log_recent(char lines[][LOG_LINE_MAX], int max);

#define LOGI(...) log_printf(__VA_ARGS__)

#endif
