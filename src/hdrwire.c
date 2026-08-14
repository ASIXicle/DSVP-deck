/*
 * hdrwire.c — in-process HDR wire-state probe.
 *
 * Reads the HDR_OUTPUT_METADATA and Colorspace DRM connector
 * properties and writes them into dsvp.log: ground truth on what HDR
 * infoframe the kernel is sending each display, captured in the same
 * log as the playback it belongs to. The standalone tools/dsvp-hdr-probe
 * remains for SSH/root use; this runs as the player, which inside the
 * seat session holds logind's device ACL and normally needs no
 * privileges.
 *
 * Deliberately self-contained (vendored DRM UAPI, no dsvp.h): the
 * definitions are stable kernel ABI, and keeping SDL/FFmpeg types out
 * lets the file compile anywhere for verification.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/types.h>

void log_msg(const char *fmt, ...);   /* log.c */

/* ── Vendored DRM UAPI (see tools/dsvp-hdr-probe.c for provenance) ── */
#define DRM_IOCTL_BASE 'd'
#define DRM_IOWR(nr, type) _IOWR(DRM_IOCTL_BASE, nr, type)
#define DRM_MODE_OBJECT_CONNECTOR 0xc0c0c0c0

struct drm_mode_card_res {
    __u64 fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
    __u32 count_fbs, count_crtcs, count_connectors, count_encoders;
    __u32 min_width, max_width, min_height, max_height;
};
struct drm_mode_get_connector {
    __u64 encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
    __u32 count_modes, count_props, count_encoders;
    __u32 encoder_id, connector_id, connector_type, connector_type_id;
    __u32 connection, mm_width, mm_height, subpixel, pad;
};
struct drm_mode_get_property {
    __u64 values_ptr, enum_blob_ptr;
    __u32 prop_id, flags;
    char  name[32];
    __u32 count_values, count_enum_blobs;
};
struct drm_mode_get_blob {
    __u32 blob_id, length;
    __u64 data;
};
struct drm_mode_obj_get_properties {
    __u64 props_ptr, prop_values_ptr;
    __u32 count_props, obj_id, obj_type;
};
#define DRM_IOCTL_MODE_GETRESOURCES      DRM_IOWR(0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCONNECTOR      DRM_IOWR(0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_GETPROPERTY       DRM_IOWR(0xAA, struct drm_mode_get_property)
#define DRM_IOCTL_MODE_GETPROPBLOB       DRM_IOWR(0xAC, struct drm_mode_get_blob)
#define DRM_IOCTL_MODE_OBJ_GETPROPERTIES DRM_IOWR(0xB9, struct drm_mode_obj_get_properties)

struct hdrwire_infoframe {
    uint8_t  eotf, metadata_type;
    struct { uint16_t x, y; } primaries[3];
    struct { uint16_t x, y; } white_point;
    uint16_t max_mastering_lum;   /* 1 cd/m2    */
    uint16_t min_mastering_lum;   /* 0.0001 cd/m2 */
    uint16_t max_cll, max_fall;
} __attribute__((packed));

struct hdrwire_blob {
    uint32_t metadata_type;
    struct hdrwire_infoframe inf;
};

static const char *hdrwire_conn_name(uint32_t t)
{
    switch (t) {
    case 10: return "DP";
    case 11: return "HDMI-A";
    case 14: return "eDP";
    default: return "conn";
    }
}

static void hdrwire_one_connector(int fd, uint32_t conn_id)
{
    struct drm_mode_get_connector conn;
    memset(&conn, 0, sizeof(conn));
    conn.connector_id = conn_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) != 0)
        return;
    if (conn.connection != 1)   /* connected only */
        return;

    struct drm_mode_obj_get_properties op;
    memset(&op, 0, sizeof(op));
    op.obj_id = conn_id;
    op.obj_type = DRM_MODE_OBJECT_CONNECTOR;
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op) != 0
            || op.count_props == 0 || op.count_props > 256)
        return;
    uint32_t n = op.count_props;
    uint32_t *ids = calloc(n, sizeof(uint32_t));
    uint64_t *vals = calloc(n, sizeof(uint64_t));
    if (!ids || !vals) {
        free(ids); free(vals);
        return;
    }
    op.props_ptr = (uintptr_t)ids;
    op.prop_values_ptr = (uintptr_t)vals;
    op.count_props = n;
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op) != 0) {
        free(ids); free(vals);
        return;
    }
    if (op.count_props < n)
        n = op.count_props;

    uint64_t colorspace = 0, hdr_blob_id = 0;
    int have_cs = 0, have_hdr = 0;
    for (uint32_t i = 0; i < n; i++) {
        struct drm_mode_get_property gp;
        memset(&gp, 0, sizeof(gp));
        gp.prop_id = ids[i];
        if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &gp) != 0)
            continue;
        if (strcmp(gp.name, "HDR_OUTPUT_METADATA") == 0) {
            hdr_blob_id = vals[i];
            have_hdr = 1;
        } else if (strcmp(gp.name, "Colorspace") == 0) {
            colorspace = vals[i];
            have_cs = 1;
        }
    }
    free(ids);
    free(vals);
    if (!have_hdr)
        return;   /* connector without HDR property (nothing to report) */

    const char *cname = hdrwire_conn_name(conn.connector_type);
    if (hdr_blob_id == 0) {
        log_msg("HDRWIRE: %s-%u colorspace=%llu — HDR metadata NOT SET "
                "(connector not in HDR)",
                cname, conn.connector_type_id,
                (unsigned long long)(have_cs ? colorspace : 0));
        return;
    }

    struct drm_mode_get_blob blob;
    memset(&blob, 0, sizeof(blob));
    blob.blob_id = (uint32_t)hdr_blob_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob) != 0)
        return;
    uint8_t *buf = calloc(1, blob.length ? blob.length : 1);
    if (!buf)
        return;
    blob.data = (uintptr_t)buf;
    if (ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob) != 0
            || blob.length < sizeof(struct hdrwire_blob)) {
        free(buf);
        return;
    }
    const struct hdrwire_infoframe *f =
        &((const struct hdrwire_blob *)buf)->inf;
    log_msg("HDRWIRE: %s-%u blob=%u colorspace=%llu eotf=%u%s",
            cname, conn.connector_type_id, blob.blob_id,
            (unsigned long long)(have_cs ? colorspace : 0),
            f->eotf, f->eotf == 2 ? " (PQ)" : f->eotf == 3 ? " (HLG)" : "");
    log_msg("HDRWIRE:   mastering R(%.4f,%.4f) G(%.4f,%.4f) B(%.4f,%.4f) "
            "WP(%.4f,%.4f)",
            f->primaries[0].x * 0.00002, f->primaries[0].y * 0.00002,
            f->primaries[1].x * 0.00002, f->primaries[1].y * 0.00002,
            f->primaries[2].x * 0.00002, f->primaries[2].y * 0.00002,
            f->white_point.x * 0.00002, f->white_point.y * 0.00002);
    log_msg("HDRWIRE:   maxLum=%u minLum=%.4f MaxCLL=%u MaxFALL=%u",
            f->max_mastering_lum, f->min_mastering_lum * 0.0001,
            f->max_cll, f->max_fall);
    free(buf);
}

/* Log the current HDR wire state of every connected connector that
 * carries an HDR_OUTPUT_METADATA property. Safe to call any time;
 * quietly logs one line if the DRM nodes aren't readable (outside a
 * seat session without the logind ACL). */
void hdrwire_log_state(void)
{
    int opened = 0;
    char path[32];
    for (int c = 0; c < 3; c++) {
        snprintf(path, sizeof(path), "/dev/dri/card%d", c);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        opened = 1;

        struct drm_mode_card_res res;
        memset(&res, 0, sizeof(res));
        if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0
                && res.count_connectors > 0 && res.count_connectors <= 32) {
            uint32_t nconn = res.count_connectors;
            uint32_t *conns = calloc(nconn, sizeof(uint32_t));
            if (conns) {
                memset(&res, 0, sizeof(res));
                res.connector_id_ptr = (uintptr_t)conns;
                res.count_connectors = nconn;
                if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0) {
                    if (res.count_connectors < nconn)
                        nconn = res.count_connectors;
                    for (uint32_t i = 0; i < nconn; i++)
                        hdrwire_one_connector(fd, conns[i]);
                }
                free(conns);
            }
        }
        close(fd);
    }
    if (!opened)
        log_msg("HDRWIRE: no DRM node readable (no seat ACL?) — use "
                "sudo ./build/dsvp-hdr-probe instead");
}
