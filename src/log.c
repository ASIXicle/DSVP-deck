/*
 * DSVP — Dead Simple Video Player
 * log.c — Crash-safe file logger
 * Writes to dsvp.log in the same directory as the executable.
 * Every write is flushed immediately so the log survives hard crashes.
 * Also mirrors output to stderr (visible in the console window).
 */

#include "dsvp.h"

static FILE *g_logfile = NULL;

void log_init(void) {
    g_logfile = fopen("dsvp.log", "w");
    if (g_logfile) {
        /* Disable buffering — every fprintf goes to disk immediately */
        setvbuf(g_logfile, NULL, _IONBF, 0);
        log_msg("=== DSVP %s started ===", DSVP_VERSION);
    }
}

void log_close(void) {
    if (g_logfile) {
        log_msg("=== DSVP shutdown ===");
        fclose(g_logfile);
        g_logfile = NULL;
    }
}

void log_msg(const char *fmt, ...) {
    va_list args;

    /* Timestamp */
    double t = get_time_sec();

    /* Write to log file */
    if (g_logfile) {
        fprintf(g_logfile, "[%10.3f] ", t);
        va_start(args, fmt);
        vfprintf(g_logfile, fmt, args);
        va_end(args);
        fprintf(g_logfile, "\n");
        /* No need to fflush — unbuffered mode handles it */
    }

    /* Also write to stderr */
    fprintf(stderr, "[DSVP %10.3f] ", t);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}
