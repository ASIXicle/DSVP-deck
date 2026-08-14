/*
 * DSVP — Dead Simple Video Player
 * log.c — Crash-safe file logger
 * Writes dsvp.log into the executable's own directory (resolved from
 * /proc/self/exe), so the log is always with the binary that produced it
 * regardless of where it was launched from.
 * Every write is flushed immediately so the log survives hard crashes.
 * Also mirrors output to stderr (visible in the console window).
 */

#include "dsvp.h"

static FILE *g_logfile = NULL;
static int g_log_anon = 0;  /* set by DSVP_LOG_ANON=1 to redact file paths */

void log_init(void) {
    g_log_anon = (getenv("DSVP_LOG_ANON") != NULL);

    /* Write the log NEXT TO THE EXECUTABLE, not into the current working
     * directory. The old fopen("dsvp.log") was CWD-relative, so launching
     * from different directories scattered logs around and left people
     * reading a stale one while debugging a fresh build — which happened,
     * and cost a round of confusion. Resolve the binary's own path instead.
     * Falls back to CWD if /proc is unavailable. */
    char path[4096];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n > 0) {
        path[n] = '\0';
        char *slash = strrchr(path, '/');
        if (slash && (size_t)(slash - path) < sizeof(path) - 10) {
            strcpy(slash + 1, "dsvp.log");
            g_logfile = fopen(path, "w");
        }
    }
    if (!g_logfile) {
        snprintf(path, sizeof(path), "dsvp.log");
        g_logfile = fopen(path, "w");
    }

    if (g_logfile) {
        /* Disable buffering — every fprintf goes to disk immediately */
        setvbuf(g_logfile, NULL, _IONBF, 0);
        log_msg("=== DSVP %s started ===", DSVP_VERSION);
        log_msg("Log file: %s", path);
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
    double t = get_time_sec();

    /* Format the whole line first, then emit it in ONE write per stream.
     * The old form used three stdio calls (prefix / body / newline). Each
     * call is individually locked, but the decode, demux, audio and
     * bitstream threads all log concurrently, so another thread's line
     * could land between our prefix and our body — producing spliced,
     * unreadable entries in exactly the seek-storm and passthrough logs
     * that get read for diagnosis. One write per line ends that. */
    char line[1024];
    int n = snprintf(line, sizeof(line), "[%10.3f] ", t);
    if (n < 0) return;
    if (n > (int)sizeof(line) - 2) n = (int)sizeof(line) - 2;

    va_start(args, fmt);
    int m = vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, args);
    va_end(args);
    if (m < 0) m = 0;
    size_t len = (size_t)n + (size_t)m;
    if (len > sizeof(line) - 2) len = sizeof(line) - 2;  /* truncated */
    line[len++] = '\n';

    if (g_logfile)
        fwrite(line, 1, len, g_logfile);   /* unbuffered: reaches disk now */

    /* stderr mirror keeps the "[DSVP " prefix it always had */
    fputs("[DSVP ", stderr);
    fwrite(line + 1, 1, len - 1, stderr);
}

int log_anon_active(void) { return g_log_anon; }
