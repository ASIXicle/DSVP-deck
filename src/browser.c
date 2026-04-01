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

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <dirent.h>
  #include <sys/stat.h>
  #include <errno.h>
#endif

/* ═══════════════════════════════════════════════════════════════════
 * Forward declarations for helpers shared with main.c (video_extensions)
 * ═══════════════════════════════════════════════════════════════════ */

/* Defined in main.c — reuse for media file filtering */
extern const char *video_extensions[];
extern int is_media_file(const char *name);

/* ═══════════════════════════════════════════════════════════════════
 * Path Persistence — ~/.config/dsvp/last_path
 * ═══════════════════════════════════════════════════════════════════ */

static void get_config_dir(char *out, int size) {
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (appdata)
        snprintf(out, size, "%s\\dsvp", appdata);
    else
        snprintf(out, size, ".");
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0])
        snprintf(out, size, "%s/dsvp", xdg);
    else {
        const char *home = getenv("HOME");
        snprintf(out, size, "%s/.config/dsvp", home ? home : "/tmp");
    }
#endif
}

static void ensure_config_dir(void) {
    char dir[512];
    get_config_dir(dir, sizeof(dir));
#ifdef _WIN32
    CreateDirectoryA(dir, NULL);
#else
    /* mkdir -p equivalent: create parent then child */
    char parent[512];
    snprintf(parent, sizeof(parent), "%s", dir);
    char *slash = strrchr(parent, '/');
    if (slash) {
        *slash = '\0';
        mkdir(parent, 0755);  /* ~/.config — may already exist */
    }
    mkdir(dir, 0755);  /* ~/.config/dsvp — may already exist */
#endif
}

void browser_save_path(PlayerState *ps) {
    if (!ps->browser_path[0]) return;
    ensure_config_dir();

    char filepath[600];
    char dir[512];
    get_config_dir(dir, sizeof(dir));
#ifdef _WIN32
    snprintf(filepath, sizeof(filepath), "%s\\last_path", dir);
#else
    snprintf(filepath, sizeof(filepath), "%s/last_path", dir);
#endif

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
#ifdef _WIN32
    snprintf(filepath, sizeof(filepath), "%s\\last_path", dir);
#else
    snprintf(filepath, sizeof(filepath), "%s/last_path", dir);
#endif

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

    /* Validate the saved path still exists */
    if (ps->browser_path[0]) {
#ifdef _WIN32
        DWORD attr = GetFileAttributesA(ps->browser_path);
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
            ps->browser_path[0] = '\0';
#else
        struct stat st;
        if (stat(ps->browser_path, &st) != 0 || !S_ISDIR(st.st_mode))
            ps->browser_path[0] = '\0';
#endif
    }

    /* Default to home directory */
    if (!ps->browser_path[0]) {
#ifdef _WIN32
        const char *home = getenv("USERPROFILE");
        if (home)
            snprintf(ps->browser_path, sizeof(ps->browser_path), "%s\\", home);
        else
            snprintf(ps->browser_path, sizeof(ps->browser_path), "C:\\");
#else
        const char *home = getenv("HOME");
        snprintf(ps->browser_path, sizeof(ps->browser_path), "%s/",
                 home ? home : "/");
#endif
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
#ifdef _WIN32
    return _stricmp(ea->name, eb->name);
#else
    return strcasecmp(ea->name, eb->name);
#endif
}

void browser_scan(PlayerState *ps) {
    browser_free_entries(ps);

    if (!ps->browser_path[0]) return;

    int capacity = 128;
    BrowseEntry *entries = malloc(capacity * sizeof(BrowseEntry));
    if (!entries) return;
    int count = 0;

#ifdef _WIN32
    /* Windows directory scan */
    char pattern[2048];
    snprintf(pattern, sizeof(pattern), "%s*", ps->browser_path);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        free(entries);
        return;
    }
    do {
        if (fd.cFileName[0] == '.') continue;  /* skip hidden + . + .. */

        int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

        /* Show directories and media files only */
        if (!is_dir && !is_media_file(fd.cFileName)) continue;

        if (count >= capacity) {
            capacity *= 2;
            BrowseEntry *tmp = realloc(entries, capacity * sizeof(BrowseEntry));
            if (!tmp) break;
            entries = tmp;
        }

        char fullpath[2048];
        snprintf(fullpath, sizeof(fullpath), "%s%s", ps->browser_path, fd.cFileName);

        entries[count].name = strdup(fd.cFileName);
        entries[count].path = strdup(fullpath);
        entries[count].is_dir = is_dir;
        count++;
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
#else
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

    /* ── Inject mount points when browsing near root ──
     * Scan /run/media/ for SD card and NFS mounts so the user
     * can navigate to external storage without typing paths. */
    {
        const char *mount_bases[] = { "/run/media/", NULL };
        for (int mi = 0; mount_bases[mi]; mi++) {
            /* Check if we're already inside this mount base */
            if (strncmp(ps->browser_path, mount_bases[mi],
                        strlen(mount_bases[mi])) == 0)
                continue;

            DIR *md = opendir(mount_bases[mi]);
            if (!md) continue;
            struct dirent *me;
            while ((me = readdir(md)) != NULL) {
                if (me->d_name[0] == '.') continue;
                char mpath[2048];
                snprintf(mpath, sizeof(mpath), "%s%s",
                         mount_bases[mi], me->d_name);
                struct stat mst;
                if (stat(mpath, &mst) != 0 || !S_ISDIR(mst.st_mode))
                    continue;

                /* Recurse one level into user dirs under /run/media/user/ */
                DIR *ud = opendir(mpath);
                if (!ud) continue;
                struct dirent *ue;
                while ((ue = readdir(ud)) != NULL) {
                    if (ue->d_name[0] == '.') continue;
                    char upath[2048];
                    snprintf(upath, sizeof(upath), "%s/%s",
                             mpath, ue->d_name);
                    struct stat ust;
                    if (stat(upath, &ust) != 0 || !S_ISDIR(ust.st_mode))
                        continue;

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

                    char label[256];
                    snprintf(label, sizeof(label), "[SD] %s", ue->d_name);
                    entries[count].name = strdup(label);
                    entries[count].path = strdup(upath);
                    entries[count].is_dir = 1;
                    count++;
                }
                closedir(ud);
            }
            closedir(md);
        }
    }
#endif

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

    log_msg("browser: scanned %s — %d entries", ps->browser_path, count);
}


/* ═══════════════════════════════════════════════════════════════════
 * Navigation
 * ═══════════════════════════════════════════════════════════════════ */

void browser_init(PlayerState *ps) {
    browser_load_path(ps);
    browser_scan(ps);
    ps->browser_active = 1;
    log_msg("browser: initialized at %s", ps->browser_path);
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
        strncpy(ps->browser_path, newpath, sizeof(ps->browser_path) - 1);
        ps->browser_path[sizeof(ps->browser_path) - 1] = '\0';
        browser_scan(ps);
        browser_save_path(ps);
        return 0;
    } else {
        /* File selected */
        strncpy(ps->browser_selected_file,
                ps->browser_entries[ps->browser_sel],
                sizeof(ps->browser_selected_file) - 1);
        ps->browser_selected_file[sizeof(ps->browser_selected_file) - 1] = '\0';
        return 1;
    }
}

void browser_back(PlayerState *ps) {
    /* Go up one directory */
    char *path = ps->browser_path;
    size_t len = strlen(path);

    /* Remove trailing separator */
    if (len > 1 && (path[len - 1] == '/' || path[len - 1] == '\\'))
        path[--len] = '\0';

    /* Find previous separator */
    char *sep = strrchr(path, '/');
#ifdef _WIN32
    char *sep2 = strrchr(path, '\\');
    if (sep2 && (!sep || sep2 > sep)) sep = sep2;
#endif

    if (sep && sep != path) {
        *(sep + 1) = '\0';  /* keep trailing slash */
    } else if (sep == path) {
        /* At root "/" */
        path[1] = '\0';
    }
#ifdef _WIN32
    else {
        /* At drive root like "C:" */
        if (len >= 2 && path[1] == ':') {
            path[2] = '\\';
            path[3] = '\0';
        }
    }
#endif

    browser_scan(ps);
    browser_save_path(ps);
}

int browser_at_root(PlayerState *ps) {
    const char *p = ps->browser_path;
#ifdef _WIN32
    /* "C:\" or similar */
    return (strlen(p) <= 3 && p[1] == ':');
#else
    return (strcmp(p, "/") == 0);
#endif
}
