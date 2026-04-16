/*
 * DSVP — Dead Simple Video Player
 * browser.c — Built-in file browser for Game Mode
 *
 * Replaces the external zenity/kdialog file dialog that cannot be
 * navigated with a gamepad in Steam Deck Game Mode. Provides a
 * d-pad/keyboard-navigable directory listing rendered through the
 * existing overlay system (bitmap font + fill_rect).
 *
 * The browser is the default screen when no file is loaded.
 * Playback ending (B/Q) returns here rather than the old idle screen.
 * The native file dialog (O key) remains available for Desktop Mode.
 */

#include "dsvp.h"

#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

/* ═══════════════════════════════════════════════════════════════════
 * Forward declarations for helpers shared with main.c (video_extensions)
 * ═══════════════════════════════════════════════════════════════════ */

/* Defined in main.c — reuse for media file filtering */
extern const char *video_extensions[];
extern int is_media_file(const char *name);

/* ═══════════════════════════════════════════════════════════════════
 * Non-blocking path accessibility check
 *
 * stat() on a stale NFS mount blocks for 30-60+ seconds in the kernel.
 * We can't cancel it, so we run it in a detached thread and wait on
 * a semaphore with a short timeout. If the thread completes in time,
 * we get the result. If not, we abandon the thread (small leak, once)
 * and fall back to a safe local path.
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    const char    *path;
    SDL_Semaphore *sem;
    int            result;  /* 1 = accessible directory, 0 = not */
} PathCheckArgs;

static int path_check_thread(void *arg) {
    PathCheckArgs *a = (PathCheckArgs *)arg;
    struct stat st;
    a->result = (stat(a->path, &st) == 0 && S_ISDIR(st.st_mode));
    SDL_SignalSemaphore(a->sem);
    return 0;
}

/* Returns 1 if path is an accessible directory, 0 if not or timeout.
 * timeout_ms: max time to wait (e.g. 2000 for 2 seconds). */
static int path_accessible(const char *path, int timeout_ms) {
    /* Fast path: local paths (starting with /home) rarely stall */
    SDL_Semaphore *sem = SDL_CreateSemaphore(0);
    if (!sem) {
        /* Fallback: blocking stat (old behavior) */
        struct stat st;
        return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
    }

    PathCheckArgs args = { .path = path, .sem = sem, .result = 0 };
    SDL_Thread *t = SDL_CreateThread(path_check_thread, "pathchk", &args);
    if (!t) {
        SDL_DestroySemaphore(sem);
        struct stat st;
        return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
    }

    int ok = SDL_WaitSemaphoreTimeout(sem, timeout_ms);
    SDL_DestroySemaphore(sem);

    if (ok) {
        /* Thread completed in time */
        SDL_WaitThread(t, NULL);
        return args.result;
    } else {
        /* Timeout — thread is stuck in kernel stat().
         * Detach it; small leak, but only happens with stale NFS. */
        SDL_DetachThread(t);
        log_msg("browser: path check timed out (%dms): %s", timeout_ms, path);
        return 0;
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * USB / SD Card Auto-Mount
 *
 * SteamOS Game Mode only automounts drives formatted as Steam
 * Library volumes (ext4). Regular USB drives with NTFS/exFAT/ext4
 * that users plug in for media files are not mounted automatically.
 *
 * udisksctl is on every SteamOS install and needs no root. It mounts
 * to /run/media/deck/<label>, which the mount injection code in
 * browser_scan() already picks up.
 * ═══════════════════════════════════════════════════════════════════ */

static void try_automount_removable(void) {
    static int attempted = 0;
    if (attempted) return;
    attempted = 1;

    DIR *d = opendir("/dev/disk/by-id/");
    if (!d) {
        log_msg("browser: automount: /dev/disk/by-id/ not available");
        return;
    }

    /* Read /proc/mounts once to build list of already-mounted devices */
    char mounted_devs[4096] = "";
    FILE *pm = fopen("/proc/mounts", "r");
    if (pm) {
        char line[512];
        while (fgets(line, sizeof(line), pm)) {
            if (strncmp(line, "/dev/", 5) == 0) {
                char *sp = strchr(line, ' ');
                if (sp) {
                    size_t cur = strlen(mounted_devs);
                    snprintf(mounted_devs + cur,
                             sizeof(mounted_devs) - cur,
                             "%.*s|", (int)(sp - line), line);
                }
            }
        }
        fclose(pm);
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        /* Only USB and MMC partition entries */
        if (!strstr(ent->d_name, "usb") && !strstr(ent->d_name, "mmc"))
            continue;
        if (!strstr(ent->d_name, "part"))
            continue;  /* skip whole-disk entries */

        /* Resolve symlink → actual device path */
        char link[512];
        snprintf(link, sizeof(link), "/dev/disk/by-id/%s", ent->d_name);
        char real[512];
        if (!realpath(link, real)) continue;

        /* Already mounted? */
        char needle[520];
        snprintf(needle, sizeof(needle), "%s|", real);
        if (strstr(mounted_devs, needle)) continue;

        /* Mount via udisksctl — no root, handles NTFS/exFAT/ext4 */
        log_msg("browser: automount: mounting %s", real);
        char cmd[600];
        snprintf(cmd, sizeof(cmd),
                 "udisksctl mount -b %s --no-user-interaction 2>&1", real);
        FILE *fp = popen(cmd, "r");
        if (fp) {
            char buf[256];
            while (fgets(buf, sizeof(buf), fp)) {
                size_t blen = strlen(buf);
                if (blen > 0 && buf[blen - 1] == '\n')
                    buf[blen - 1] = '\0';
                log_msg("browser:   %s", buf);
            }
            pclose(fp);
        }
    }
    closedir(d);
}

/* ═══════════════════════════════════════════════════════════════════
 * Path Persistence — ~/.config/dsvp/last_path
 * ═══════════════════════════════════════════════════════════════════ */

static void get_config_dir(char *out, int size) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0])
        snprintf(out, size, "%s/dsvp", xdg);
    else {
        const char *home = getenv("HOME");
        snprintf(out, size, "%s/.config/dsvp", home ? home : "/tmp");
    }
}

static void ensure_config_dir(void) {
    char dir[512];
    get_config_dir(dir, sizeof(dir));
    /* mkdir -p equivalent: create parent then child */
    char parent[512];
    snprintf(parent, sizeof(parent), "%s", dir);
    char *slash = strrchr(parent, '/');
    if (slash) {
        *slash = '\0';
        mkdir(parent, 0755);  /* ~/.config — may already exist */
    }
    mkdir(dir, 0755);  /* ~/.config/dsvp — may already exist */
}

void browser_save_path(PlayerState *ps) {
    if (!ps->browser_path[0]) return;
    ensure_config_dir();

    char filepath[600];
    char dir[512];
    get_config_dir(dir, sizeof(dir));
    snprintf(filepath, sizeof(filepath), "%s/last_path", dir);

    FILE *f = fopen(filepath, "w");
    if (f) {
        fputs(ps->browser_path, f);
        fclose(f);
    }
}

static void browser_load_path(PlayerState *ps) {
    char filepath[600];
    char dir[512];
    get_config_dir(dir, sizeof(dir));
    snprintf(filepath, sizeof(filepath), "%s/last_path", dir);

    FILE *f = fopen(filepath, "r");
    if (f) {
        if (fgets(ps->browser_path, sizeof(ps->browser_path), f)) {
            /* Strip trailing newline */
            size_t len = strlen(ps->browser_path);
            if (len > 0 && ps->browser_path[len - 1] == '\n')
                ps->browser_path[len - 1] = '\0';
        }
        fclose(f);
    }

    /* Validate the saved path still exists (2s timeout for stale NFS) */
    if (ps->browser_path[0]) {
        if (!path_accessible(ps->browser_path, 2000))
            ps->browser_path[0] = '\0';
    }

    /* Default to home directory */
    if (!ps->browser_path[0]) {
        const char *home = getenv("HOME");
        snprintf(ps->browser_path, sizeof(ps->browser_path), "%s/",
                 home ? home : "/");
    }
}


/* ═══════════════════════════════════════════════════════════════════
 * Directory Scanning
 * ═══════════════════════════════════════════════════════════════════ */

void browser_free_entries(PlayerState *ps) {
    if (ps->browser_entries) {
        for (int i = 0; i < ps->browser_count; i++) {
            free(ps->browser_entries[i]);
            free(ps->browser_names[i]);
        }
        free(ps->browser_entries);
        free(ps->browser_names);
        free(ps->browser_is_dir);
        ps->browser_entries = NULL;
        ps->browser_names = NULL;
        ps->browser_is_dir = NULL;
    }
    ps->browser_count = 0;
}

/* Compare for qsort: directories first, then alphabetical */
typedef struct {
    char *name;
    char *path;
    int   is_dir;
} BrowseEntry;

static int cmp_browse_entries(const void *a, const void *b) {
    const BrowseEntry *ea = (const BrowseEntry *)a;
    const BrowseEntry *eb = (const BrowseEntry *)b;
    /* Directories before files */
    if (ea->is_dir != eb->is_dir)
        return eb->is_dir - ea->is_dir;
    /* Alphabetical (case-insensitive) */
    return strcasecmp(ea->name, eb->name);
}

void browser_scan(PlayerState *ps) {
    browser_free_entries(ps);

    if (!ps->browser_path[0]) return;

    /* Check path is reachable (2s timeout for stale NFS).
     * Fall back to $HOME if the current path is stuck. */
    if (!path_accessible(ps->browser_path, 2000)) {
        log_msg("browser: path inaccessible, falling back to HOME: %s",
                ps->browser_path);
        const char *home = getenv("HOME");
        snprintf(ps->browser_path, sizeof(ps->browser_path), "%s/",
                 home ? home : "/");
        /* HOME itself should never stall, but don't recurse — just proceed */
    }

    int capacity = 128;
    BrowseEntry *entries = malloc(capacity * sizeof(BrowseEntry));
    if (!entries) return;
    int count = 0;

    /* POSIX directory scan */
    DIR *d = opendir(ps->browser_path);
    if (!d) {
        log_msg("browser: cannot open directory: %s", ps->browser_path);
        free(entries);
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;  /* skip hidden + . + .. */

        char fullpath[2048];
        snprintf(fullpath, sizeof(fullpath), "%s%s",
                 ps->browser_path, ent->d_name);

        struct stat st;
        if (stat(fullpath, &st) != 0) continue;

        int is_dir = S_ISDIR(st.st_mode);

        /* Show directories and media files only */
        if (!is_dir && !is_media_file(ent->d_name)) continue;

        if (count >= capacity) {
            capacity *= 2;
            BrowseEntry *tmp = realloc(entries, capacity * sizeof(BrowseEntry));
            if (!tmp) break;
            entries = tmp;
        }

        entries[count].name = strdup(ent->d_name);
        entries[count].path = strdup(fullpath);
        entries[count].is_dir = is_dir;
        count++;
    }
    closedir(d);

    /* ── Auto-mount removable drives, then inject mount points ──
     * First try to mount any unmounted USB/SD devices (SteamOS Game
     * Mode doesn't automount media drives). Then scan /run/media/
     * for all mounts so the user can navigate to external storage. */
    try_automount_removable();
    {
        int mounts_injected = 0;
        const char *mount_bases[] = { "/run/media/", NULL };
        for (int mi = 0; mount_bases[mi]; mi++) {
            log_msg("browser: scanning mount base: %s", mount_bases[mi]);

            /* Check if we're already inside this mount base */
            if (strncmp(ps->browser_path, mount_bases[mi],
                        strlen(mount_bases[mi])) == 0) {
                log_msg("browser:   skipped (already inside)");
                continue;
            }

            DIR *md = opendir(mount_bases[mi]);
            if (!md) {
                log_msg("browser:   opendir failed");
                continue;
            }
            struct dirent *me;
            while ((me = readdir(md)) != NULL) {
                if (me->d_name[0] == '.') continue;
                char mpath[2048];
                snprintf(mpath, sizeof(mpath), "%s%s",
                         mount_bases[mi], me->d_name);
                struct stat mst;
                if (stat(mpath, &mst) != 0 || !S_ISDIR(mst.st_mode))
                    continue;

                log_msg("browser:   user dir: %s", mpath);

                /* Recurse one level into user dirs under /run/media/user/ */
                DIR *ud = opendir(mpath);
                if (!ud) continue;
                struct dirent *ue;
                while ((ue = readdir(ud)) != NULL) {
                    if (ue->d_name[0] == '.') continue;
                    char upath[2048 + 256];
                    snprintf(upath, sizeof(upath), "%s/%s",
                             mpath, ue->d_name);
                    struct stat ust;
                    if (stat(upath, &ust) != 0 || !S_ISDIR(ust.st_mode))
                        continue;

                    log_msg("browser:     mount found: %s", upath);

                    /* Check not already listed */
                    int dup = 0;
                    for (int i = 0; i < count; i++)
                        if (strcmp(entries[i].path, upath) == 0) { dup = 1; break; }
                    if (dup) continue;

                    if (count >= capacity) {
                        capacity *= 2;
                        BrowseEntry *tmp = realloc(entries, capacity * sizeof(BrowseEntry));
                        if (!tmp) break;
                        entries = tmp;
                    }

                    char label[8 + 256];
                    snprintf(label, sizeof(label), "[USB] %s", ue->d_name);
                    entries[count].name = strdup(label);
                    entries[count].path = strdup(upath);
                    entries[count].is_dir = 1;
                    count++;
                    mounts_injected++;
                }
                closedir(ud);
            }
            closedir(md);
        }
        log_msg("browser: mount injection done — %d mount(s) added", mounts_injected);
    }

    /* Sort: directories first, then alphabetical */
    qsort(entries, count, sizeof(BrowseEntry), cmp_browse_entries);

    /* Flatten into parallel arrays for PlayerState */
    ps->browser_entries = malloc(count * sizeof(char *));
    ps->browser_names   = malloc(count * sizeof(char *));
    ps->browser_is_dir  = malloc(count * sizeof(int));

    if (!ps->browser_entries || !ps->browser_names || !ps->browser_is_dir) {
        for (int i = 0; i < count; i++) { free(entries[i].name); free(entries[i].path); }
        free(entries);
        free(ps->browser_entries); free(ps->browser_names); free(ps->browser_is_dir);
        ps->browser_entries = NULL; ps->browser_names = NULL; ps->browser_is_dir = NULL;
        return;
    }

    for (int i = 0; i < count; i++) {
        ps->browser_entries[i] = entries[i].path;
        ps->browser_is_dir[i]  = entries[i].is_dir;
        /* Display name: prefix dirs with [DIR] */
        if (entries[i].is_dir) {
            char display[300];
            snprintf(display, sizeof(display), "[DIR] %s", entries[i].name);
            ps->browser_names[i] = strdup(display);
        } else {
            ps->browser_names[i] = strdup(entries[i].name);
        }
        free(entries[i].name);  /* path ownership moved, name copied */
    }
    free(entries);

    ps->browser_count = count;
    ps->browser_sel = 0;
    ps->browser_scroll = 0;

    log_msg("browser: scanned %s — %d entries",
            log_anon_active() ? "[redacted]" : ps->browser_path, count);
}


/* ═══════════════════════════════════════════════════════════════════
 * Navigation
 * ═══════════════════════════════════════════════════════════════════ */

void browser_init(PlayerState *ps) {
    browser_load_path(ps);
    browser_scan(ps);
    ps->browser_active = 1;
    log_msg("browser: initialized at %s",
            log_anon_active() ? "[redacted]" : ps->browser_path);
}

void browser_navigate(PlayerState *ps, int delta) {
    if (ps->browser_count == 0) return;
    ps->browser_sel += delta;
    if (ps->browser_sel < 0) ps->browser_sel = 0;
    if (ps->browser_sel >= ps->browser_count)
        ps->browser_sel = ps->browser_count - 1;

    /* Adjust scroll to keep selection visible */
    if (ps->browser_sel < ps->browser_scroll)
        ps->browser_scroll = ps->browser_sel;
    if (ps->browser_sel >= ps->browser_scroll + BROWSER_MAX_VISIBLE)
        ps->browser_scroll = ps->browser_sel - BROWSER_MAX_VISIBLE + 1;
}

void browser_page(PlayerState *ps, int delta) {
    browser_navigate(ps, delta * BROWSER_MAX_VISIBLE);
}

/* Returns 1 if a file was selected (path in ps->browser_selected_file),
 * 0 if navigated to a directory. */
int browser_enter(PlayerState *ps) {
    if (ps->browser_count == 0) return 0;
    if (ps->browser_sel < 0 || ps->browser_sel >= ps->browser_count) return 0;

    if (ps->browser_is_dir[ps->browser_sel]) {
        /* Navigate into directory */
        char newpath[1024];
        snprintf(newpath, sizeof(newpath), "%s/",
                 ps->browser_entries[ps->browser_sel]);
        snprintf(ps->browser_path, sizeof(ps->browser_path), "%s", newpath);
        browser_scan(ps);
        browser_save_path(ps);
        return 0;
    } else {
        /* File selected */
        snprintf(ps->browser_selected_file,
                sizeof(ps->browser_selected_file), "%s",
                ps->browser_entries[ps->browser_sel]);
        return 1;
    }
}

void browser_back(PlayerState *ps) {
    /* Don't go above home directory — root is scary for users */
    const char *home = getenv("HOME");
    if (home) {
        char cur[1024], hbuf[1024];
        snprintf(cur, sizeof(cur), "%s", ps->browser_path);
        snprintf(hbuf, sizeof(hbuf), "%s", home);
        size_t clen = strlen(cur);
        size_t hlen = strlen(hbuf);
        if (clen > 1 && cur[clen - 1] == '/') cur[clen - 1] = '\0';
        if (hlen > 1 && hbuf[hlen - 1] == '/') hbuf[hlen - 1] = '\0';
        if (strcmp(cur, hbuf) == 0) return;  /* already at home, stop */

        /* Inside a mount path — if going up would leave the mount,
         * snap back to $HOME instead of exposing system directories */
        const char *mount_base = "/run/media/";
        if (strncmp(cur, mount_base, strlen(mount_base)) == 0) {
            char tmp[1024];
            snprintf(tmp, sizeof(tmp), "%s", cur);
            char *parent = strrchr(tmp, '/');
            if (parent) *parent = '\0';
            char *pp = strrchr(tmp, '/');
            if (pp) *pp = '\0';
            if (strlen(tmp) <= strlen("/run/media")) {
                snprintf(ps->browser_path, sizeof(ps->browser_path),
                         "%s/", home);
                browser_scan(ps);
                browser_save_path(ps);
                return;
            }
        }
    }

    /* Go up one directory */
    char *path = ps->browser_path;
    size_t len = strlen(path);

    /* Remove trailing separator */
    if (len > 1 && path[len - 1] == '/')
        path[--len] = '\0';

    /* Find previous separator */
    char *sep = strrchr(path, '/');

    if (sep && sep != path) {
        *(sep + 1) = '\0';  /* keep trailing slash */
    } else if (sep == path) {
        /* At root "/" */
        path[1] = '\0';
    }

    browser_scan(ps);
    browser_save_path(ps);
}

int browser_at_root(PlayerState *ps) {
    const char *p = ps->browser_path;
    if (strcmp(p, "/") == 0) return 1;
    const char *home = getenv("HOME");
    if (home) {
        char cur[1024], hbuf[1024];
        snprintf(cur, sizeof(cur), "%s", p);
        snprintf(hbuf, sizeof(hbuf), "%s", home);
        size_t clen = strlen(cur);
        size_t hlen = strlen(hbuf);
        if (clen > 1 && cur[clen - 1] == '/') cur[clen - 1] = '\0';
        if (hlen > 1 && hbuf[hlen - 1] == '/') hbuf[hlen - 1] = '\0';
        if (strcmp(cur, hbuf) == 0) return 1;
    }
    return 0;
}
