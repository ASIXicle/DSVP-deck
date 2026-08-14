/*
 * dsvp-hdr-probe — dump the HDR_OUTPUT_METADATA and Colorspace DRM
 * connector properties: ground truth on what HDR infoframe the kernel
 * is currently sending each display.
 *
 * Zero dependencies beyond kernel UAPI + glibc (same philosophy as
 * dsvp-arm-iec958). Needs read access to /dev/dri/cardN — run as root
 * or a user in the `video` group.
 *
 * Usage: ./dsvp-hdr-probe [/dev/dri/card0]
 *        (no argument: probes card0..card2)
 *
 * This is the verification instrument for the HDR metadata forwarding
 * work (docs/TODO-HDR.md): run during HDR playback before and after
 * the SDL patch to see whether content metadata reaches the wire.
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

/* ── Vendored DRM UAPI ──
 * SteamOS ships no drm headers (they belong to libdrm-dev, not
 * linux-api-headers). These definitions are stable kernel ABI —
 * vendored verbatim from the kernel's include/uapi/drm/, exactly the
 * subset this tool touches. */
#define DRM_IOCTL_BASE 'd'
#define DRM_IOWR(nr, type) _IOWR(DRM_IOCTL_BASE, nr, type)
#define DRM_MODE_OBJECT_CONNECTOR 0xc0c0c0c0

struct drm_mode_card_res {
    __u64 fb_id_ptr;
    __u64 crtc_id_ptr;
    __u64 connector_id_ptr;
    __u64 encoder_id_ptr;
    __u32 count_fbs;
    __u32 count_crtcs;
    __u32 count_connectors;
    __u32 count_encoders;
    __u32 min_width;
    __u32 max_width;
    __u32 min_height;
    __u32 max_height;
};

struct drm_mode_get_connector {
    __u64 encoders_ptr;
    __u64 modes_ptr;
    __u64 props_ptr;
    __u64 prop_values_ptr;
    __u32 count_modes;
    __u32 count_props;
    __u32 count_encoders;
    __u32 encoder_id;
    __u32 connector_id;
    __u32 connector_type;
    __u32 connector_type_id;
    __u32 connection;
    __u32 mm_width;
    __u32 mm_height;
    __u32 subpixel;
    __u32 pad;
};

struct drm_mode_get_property {
    __u64 values_ptr;
    __u64 enum_blob_ptr;
    __u32 prop_id;
    __u32 flags;
    char  name[32];
    __u32 count_values;
    __u32 count_enum_blobs;
};

struct drm_mode_property_enum {
    __u64 value;
    char  name[32];
};

struct drm_mode_get_blob {
    __u32 blob_id;
    __u32 length;
    __u64 data;
};

struct drm_mode_obj_get_properties {
    __u64 props_ptr;
    __u64 prop_values_ptr;
    __u32 count_props;
    __u32 obj_id;
    __u32 obj_type;
};

#define DRM_IOCTL_MODE_GETRESOURCES      DRM_IOWR(0xA0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCONNECTOR      DRM_IOWR(0xA7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_GETPROPERTY       DRM_IOWR(0xAA, struct drm_mode_get_property)
#define DRM_IOCTL_MODE_GETPROPBLOB       DRM_IOWR(0xAC, struct drm_mode_get_blob)
#define DRM_IOCTL_MODE_OBJ_GETPROPERTIES DRM_IOWR(0xB9, struct drm_mode_obj_get_properties)

/* CTA-861-G static metadata blob layout (kernel struct hdr_output_metadata).
 * Defined locally: the kernel exports it via linux/hdmi.h on some
 * distros only, and the wire layout is stable ABI. */
struct hdr_infoframe {
    uint8_t  eotf;           /* 0=SDR gamma, 1=HDR gamma, 2=ST2084, 3=HLG */
    uint8_t  metadata_type;
    struct { uint16_t x, y; } primaries[3];   /* 0.00002 units */
    struct { uint16_t x, y; } white_point;    /* 0.00002 units */
    uint16_t max_mastering_lum;               /* 1 cd/m2 units  */
    uint16_t min_mastering_lum;               /* 0.0001 cd/m2   */
    uint16_t max_cll;                         /* cd/m2          */
    uint16_t max_fall;                        /* cd/m2          */
} __attribute__((packed));

struct hdr_blob {
    uint32_t metadata_type;
    struct hdr_infoframe inf;
};

static const char *eotf_name(uint8_t e)
{
    switch (e) {
    case 0: return "SDR (traditional gamma)";
    case 1: return "HDR (traditional gamma)";
    case 2: return "ST 2084 (PQ)";
    case 3: return "HLG";
    default: return "unknown";
    }
}

static void decode_hdr_blob(const uint8_t *data, uint32_t len)
{
    if (len < sizeof(struct hdr_blob)) {
        printf("      blob too small (%u bytes, want %zu)\n",
               len, sizeof(struct hdr_blob));
        return;
    }
    const struct hdr_blob *b = (const struct hdr_blob *)data;
    const struct hdr_infoframe *f = &b->inf;
    printf("      eotf            : %u (%s)\n", f->eotf, eotf_name(f->eotf));
    printf("      metadata type   : %u\n", f->metadata_type);
    for (int i = 0; i < 3; i++)
        printf("      primary[%d]      : (%.4f, %.4f)\n", i,
               f->primaries[i].x * 0.00002, f->primaries[i].y * 0.00002);
    printf("      white point     : (%.4f, %.4f)\n",
           f->white_point.x * 0.00002, f->white_point.y * 0.00002);
    printf("      mastering lum   : max %u nits, min %.4f nits\n",
           f->max_mastering_lum, f->min_mastering_lum * 0.0001);
    printf("      MaxCLL / MaxFALL: %u / %u nits\n", f->max_cll, f->max_fall);
}

static int get_blob(int fd, uint32_t blob_id, uint8_t **out, uint32_t *out_len)
{
    struct drm_mode_get_blob blob;
    memset(&blob, 0, sizeof(blob));
    blob.blob_id = blob_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob) != 0)
        return -1;
    uint8_t *buf = calloc(1, blob.length ? blob.length : 1);
    if (!buf)
        return -1;
    blob.data = (uintptr_t)buf;
    if (ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob) != 0) {
        free(buf);
        return -1;
    }
    *out = buf;
    *out_len = blob.length;
    return 0;
}

static void probe_connector(int fd, uint32_t conn_id)
{
    /* Connection status (counts left zero: metadata-only query) */
    struct drm_mode_get_connector conn;
    memset(&conn, 0, sizeof(conn));
    conn.connector_id = conn_id;
    if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) != 0)
        return;
    printf("  connector %u: type %u, %s\n", conn_id, conn.connector_type,
           conn.connection == 1 ? "CONNECTED" :
           conn.connection == 2 ? "disconnected" : "unknown");
    if (conn.connection != 1)
        return;

    /* Properties on the connector object (two-call) */
    struct drm_mode_obj_get_properties op;
    memset(&op, 0, sizeof(op));
    op.obj_id = conn_id;
    op.obj_type = DRM_MODE_OBJECT_CONNECTOR;
    if (ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op) != 0)
        return;
    uint32_t n = op.count_props;
    if (n == 0)
        return;
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

    for (uint32_t i = 0; i < n; i++) {
        struct drm_mode_get_property gp;
        memset(&gp, 0, sizeof(gp));
        gp.prop_id = ids[i];
        if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &gp) != 0)
            continue;

        if (strcmp(gp.name, "HDR_OUTPUT_METADATA") == 0) {
            if (vals[i] == 0) {
                printf("    HDR_OUTPUT_METADATA: NOT SET "
                       "(display is not being driven in HDR)\n");
            } else {
                printf("    HDR_OUTPUT_METADATA: blob %llu\n",
                       (unsigned long long)vals[i]);
                uint8_t *data; uint32_t len;
                if (get_blob(fd, (uint32_t)vals[i], &data, &len) == 0) {
                    decode_hdr_blob(data, len);
                    free(data);
                } else {
                    printf("      (blob fetch failed: %s)\n",
                           strerror(errno));
                }
            }
        } else if (strcmp(gp.name, "Colorspace") == 0) {
            /* Fetch enum names to translate the value (two-call) */
            uint32_t ne = gp.count_enum_blobs;
            printf("    Colorspace: value %llu",
                   (unsigned long long)vals[i]);
            if (ne > 0 && ne < 64) {
                struct drm_mode_property_enum *en =
                    calloc(ne, sizeof(*en));
                if (en) {
                    gp.enum_blob_ptr = (uintptr_t)en;
                    gp.count_enum_blobs = ne;
                    /* values_ptr must also be sized if count_values set;
                     * zero it to skip */
                    gp.count_values = 0;
                    gp.values_ptr = 0;
                    if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &gp) == 0) {
                        for (uint32_t e = 0; e < ne; e++)
                            if ((uint64_t)en[e].value == vals[i])
                                printf(" (%s)", en[e].name);
                    }
                    free(en);
                }
            }
            printf("\n");
        } else if (strcmp(gp.name, "max bpc") == 0) {
            printf("    max bpc: %llu\n", (unsigned long long)vals[i]);
        }
    }
    free(ids);
    free(vals);
}

static int probe_card(const char *path)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;
    printf("%s:\n", path);

    struct drm_mode_card_res res;
    memset(&res, 0, sizeof(res));
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) {
        printf("  (no KMS resources: %s)\n", strerror(errno));
        close(fd);
        return 0;
    }
    uint32_t nconn = res.count_connectors;
    if (nconn == 0 || nconn > 32) {
        printf("  (no connectors)\n");
        close(fd);
        return 0;
    }
    uint32_t *conns = calloc(nconn, sizeof(uint32_t));
    if (!conns) {
        close(fd);
        return -1;
    }
    memset(&res, 0, sizeof(res));
    res.connector_id_ptr = (uintptr_t)conns;
    res.count_connectors = nconn;
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) {
        free(conns);
        close(fd);
        return 0;
    }
    if (res.count_connectors < nconn)
        nconn = res.count_connectors;

    for (uint32_t i = 0; i < nconn; i++)
        probe_connector(fd, conns[i]);

    free(conns);
    close(fd);
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc > 1)
        return probe_card(argv[1]) < 0 ? 1 : 0;

    int any = 0;
    char path[32];
    for (int c = 0; c < 3; c++) {
        snprintf(path, sizeof(path), "/dev/dri/card%d", c);
        if (probe_card(path) == 0)
            any = 1;
    }
    if (!any) {
        fprintf(stderr, "no DRM card readable — run as root or add "
                        "user to the video group\n");
        return 1;
    }
    return 0;
}
