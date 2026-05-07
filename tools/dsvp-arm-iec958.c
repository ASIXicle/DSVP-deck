/*
 * dsvp-arm-iec958.c — Set IEC 61937 non-audio bit on HDMI HDA codec
 *
 * Standalone helper invoked by udev when /dev/snd/hwC*D0 appears or
 * changes (HDMI hotplug, system resume). Writes SET_DIGI_CONVERT_1
 * verb to every digital-output converter widget on the codec, putting
 * the codec into non-audio mode for IEC 61937 passthrough.
 *
 * The codec retains the bit across snd_pcm_open and stream close, so
 * setting it once at boot is sufficient — DSVP itself runs unprivileged
 * and just opens hw:N,M normally.
 *
 * Invocation:
 *   dsvp-arm-iec958 <card_number>          # arms one card by number
 *   dsvp-arm-iec958                        # arms all hwC*D0 found
 *
 * udev rule (installed by scripts/install-udev-rule.sh):
 *   KERNEL=="hwC*D0", SUBSYSTEM=="sound", \
 *       ACTION=="add|change", \
 *       RUN+="/usr/local/sbin/dsvp-arm-iec958 %n"
 *
 * Why a separate binary instead of doing this inside DSVP?
 *   /dev/snd/hwC*D0 is owned by root:audio with mode 0660. DSVP runs
 *   as the deck user and shouldn't require audio-group membership or
 *   per-runtime ACL grants — that's friction users won't tolerate, and
 *   it precludes Flatpak distribution. A privileged udev-fired helper
 *   sets the codec state once at the right moment; everything else is
 *   unprivileged.
 *
 * Build: gcc -Wall -Wextra -O2 -o dsvp-arm-iec958 dsvp-arm-iec958.c
 * Install: install -m 0755 -o root -g root dsvp-arm-iec958 /usr/local/sbin/
 *
 * No external library deps. Only inlines the minimal subset of
 * <sound/hda_hwdep.h> we need (the structure layout is stable kernel
 * UAPI).
 *
 * License: same as DSVP project.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <dirent.h>

/* ── HDA UAPI (subset of <sound/hda_hwdep.h>) ─────────────────────── */
#ifndef HDA_HWDEP_VERSION
#define HDA_HWDEP_VERSION   ((1 << 16) | (0 << 8) | (0 << 0))  /* 1.0.0 */
struct hda_verb_ioctl {
    uint32_t verb;   /* nid << 24 | verb << 8 | param */
    uint32_t res;
};
#define HDA_IOCTL_PVERSION   _IOR('H', 0x10, int)
#define HDA_IOCTL_VERB_WRITE _IOWR('H', 0x11, struct hda_verb_ioctl)
#endif

#define HDA_VERB_PACK(nid, verb, param) \
    (((uint32_t)(nid) << 24) | ((uint32_t)(verb) << 8) | (uint32_t)(param))

/* HDA verbs we use */
#define HDA_VERB_GET_PARAMETERS     0x0f00
#define HDA_VERB_GET_AUDIO_WIDGET   0x0009
#define HDA_VERB_GET_DIGI_CONVERT   0x0f0d
#define HDA_VERB_SET_DIGI_CONVERT_1 0x70d
#define HDA_PAR_AUDIO_WIDGET_CAP    0x09
#define HDA_PAR_NODE_COUNT          0x04

/* DIGI_CONVERT_1 byte 1 bits */
#define HDA_DIG1_ENABLE   0x01  /* bit 0: digital output enable */
#define HDA_DIG1_V        0x02  /* bit 1: validity */
#define HDA_DIG1_NAUDIO   0x20  /* bit 5: non-audio (IEC 61937) */

/* AUDIO_WIDGET_CAP bits */
#define HDA_WCAPS_TYPE_MASK     0x00f00000
#define HDA_WCAPS_TYPE_SHIFT    20
#define HDA_WCAPS_TYPE_AUDIO_OUT 0x0
#define HDA_WCAPS_DIGITAL       0x00000200  /* bit 9: digital widget */

/* ── Logging ──────────────────────────────────────────────────────── */
static int verbose = 0;

/* If non-negative, only arm this NID; -1 = arm every digital out converter. */
static int target_nid = -1;

static void log_info(const char *fmt, ...) {
    if (!verbose) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "dsvp-arm-iec958: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static void log_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "dsvp-arm-iec958: ERROR: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* ── HDA verb wrappers ────────────────────────────────────────────── */

/* Issue a verb to the codec. Returns response on success, -1 on error. */
static int hda_verb(int fd, uint8_t nid, uint16_t verb, uint16_t param) {
    struct hda_verb_ioctl val;
    val.verb = HDA_VERB_PACK(nid, verb, param);
    val.res = 0;
    if (ioctl(fd, HDA_IOCTL_VERB_WRITE, &val) < 0) {
        log_err("ioctl(VERB_WRITE) nid=0x%02x verb=0x%03x param=0x%02x: %s",
                nid, verb, param, strerror(errno));
        return -1;
    }
    return (int)val.res;
}

/* Read AUDIO_WIDGET_CAP for a widget. Returns -1 on error. */
static int hda_get_wcaps(int fd, uint8_t nid) {
    return hda_verb(fd, nid, HDA_VERB_GET_PARAMETERS, HDA_PAR_AUDIO_WIDGET_CAP);
}

/* Read NODE_COUNT — returns (start_nid << 16) | count. */
static int hda_get_node_count(int fd, uint8_t fg_nid) {
    return hda_verb(fd, fg_nid, HDA_VERB_GET_PARAMETERS, HDA_PAR_NODE_COUNT);
}

/* ── Codec walking ────────────────────────────────────────────────── */

/* Set the non-audio bit on every digital audio-output converter widget
 * on the codec.  Returns the number of widgets armed (0 or more) on
 * success, -1 on hwdep ioctl failure. */
static int arm_codec(const char *hwdep_path) {
    int fd = open(hwdep_path, O_RDWR);
    if (fd < 0) {
        /* EACCES while running under udev means the rule fired before
         * the device permissions settled; this is non-fatal — udev will
         * fire us again on the matching change event. */
        log_err("open(%s): %s", hwdep_path, strerror(errno));
        return -1;
    }

    int version = 0;
    if (ioctl(fd, HDA_IOCTL_PVERSION, &version) < 0) {
        log_err("HDA_IOCTL_PVERSION on %s: %s", hwdep_path, strerror(errno));
        close(fd);
        return -1;
    }
    if (version < HDA_HWDEP_VERSION) {
        log_err("hwdep version 0x%x too old (need 0x%x)",
                version, HDA_HWDEP_VERSION);
        close(fd);
        return -1;
    }

    /* Walk the function-group children to find audio-out widgets.
     * The Audio Function Group is at NID 0x01 on every HDA codec.
     * NODE_COUNT on 0x01 returns (start_nid << 16) | count of children. */
    int nc = hda_get_node_count(fd, 0x01);
    if (nc < 0) {
        close(fd);
        return -1;
    }
    int start_nid = (nc >> 16) & 0xff;
    int count = nc & 0xff;
    log_info("%s: AFG NID 0x01 has %d children starting at 0x%02x",
             hwdep_path, count, start_nid);

    int armed = 0;
    for (int i = 0; i < count; i++) {
        uint8_t nid = (uint8_t)(start_nid + i);

        /* Filter to a specific NID if requested. Allows the install
         * script to constrain arming to the HBR converter (--nid 0x06
         * on Steam Deck) so other HDMI converters keep carrying
         * desktop audio in PCM mode. */
        if (target_nid >= 0 && nid != (uint8_t)target_nid) continue;

        int caps = hda_get_wcaps(fd, nid);
        if (caps < 0) continue;

        int type = (caps & HDA_WCAPS_TYPE_MASK) >> HDA_WCAPS_TYPE_SHIFT;
        int is_digital = (caps & HDA_WCAPS_DIGITAL) != 0;

        /* Only arm digital audio-output converters */
        if (type != HDA_WCAPS_TYPE_AUDIO_OUT || !is_digital) continue;

        if (hda_verb(fd, nid, HDA_VERB_SET_DIGI_CONVERT_1,
                     HDA_DIG1_ENABLE | HDA_DIG1_V | HDA_DIG1_NAUDIO) < 0) {
            continue;
        }

        log_info("%s: armed digital converter NID 0x%02x", hwdep_path, nid);
        armed++;
    }

    close(fd);
    return armed;
}

/* ── Card discovery ──────────────────────────────────────────────── */

/* Arm a single card by number. Returns 0 on success, non-zero on error. */
static int arm_one_card(int card_num) {
    char path[64];
    snprintf(path, sizeof(path), "/dev/snd/hwC%dD0", card_num);

    struct stat st;
    if (stat(path, &st) < 0) {
        /* Card has no hwdep node — not necessarily an error if the
         * card is, e.g., the internal speaker codec without HDMI. */
        log_info("%s: not present, skipping", path);
        return 0;
    }

    int armed = arm_codec(path);
    if (armed < 0) return 1;

    log_info("%s: armed %d digital converter(s)", path, armed);
    return 0;
}

/* Arm every hwC*D0 device present. Returns 0 on success. */
static int arm_all_cards(void) {
    int errors = 0;
    int found = 0;

    DIR *d = opendir("/dev/snd");
    if (!d) {
        log_err("opendir(/dev/snd): %s", strerror(errno));
        return 1;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        int card_num;
        char d0_check;
        /* Match "hwC<N>D0" exactly (D0, not D1/D2/etc). */
        if (sscanf(ent->d_name, "hwC%dD%c", &card_num, &d0_check) != 2)
            continue;
        if (d0_check != '0') continue;

        found++;
        if (arm_one_card(card_num) != 0)
            errors++;
    }

    closedir(d);

    if (found == 0)
        log_info("no /dev/snd/hwC*D0 devices found");

    return errors ? 1 : 0;
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    /* Parse args */
    int card_num = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr,
                    "usage: dsvp-arm-iec958 [-v] [--nid 0xNN] [<card_number>]\n"
                    "  Sets IEC 61937 non-audio bit on digital-output converter\n"
                    "  widgets of HDA codecs.\n"
                    "  -v          verbose logging to stderr\n"
                    "  --nid 0xNN  arm only this NID (default: all digital outs)\n"
                    "  <card>      arm only this card (default: walk /dev/snd)\n");
            return 0;
        } else if (strcmp(argv[i], "--nid") == 0) {
            if (i + 1 >= argc) {
                log_err("--nid requires a value");
                return 2;
            }
            char *end = NULL;
            long n = strtol(argv[++i], &end, 0);  /* base 0: accepts 0x06, 06, 6 */
            if (*end != '\0' || n < 0 || n > 0xff) {
                log_err("invalid NID: %s", argv[i]);
                return 2;
            }
            target_nid = (int)n;
        } else {
            /* Treat as card number */
            char *end = NULL;
            long n = strtol(argv[i], &end, 10);
            if (*end != '\0' || n < 0 || n > 31) {
                log_err("invalid card number: %s", argv[i]);
                return 2;
            }
            card_num = (int)n;
        }
    }

    /* Enable verbose if invoked from a terminal — udev runs without one. */
    if (isatty(2)) verbose = 1;

    if (target_nid >= 0)
        log_info("filter: only NID 0x%02x", target_nid);

    if (card_num >= 0)
        return arm_one_card(card_num);
    else
        return arm_all_cards();
}
