#include "radeon_legacy.h"
#include <stdio.h>
#include <string.h>

static void irq(void *o, bool l)
{
    (void)o;
    printf("IRQ=%d\n", l ? 1 : 0);
}

static void set_mode_legacy(RLGDevice *d)
{
    rlg_mmio_write(d, RLG_CRTC_H_TOTAL_DISP, ((640u / 8u) - 1u) << 16, 4);
    rlg_mmio_write(d, RLG_CRTC_V_TOTAL_DISP, (480u - 1u) << 16, 4);
    rlg_mmio_write(d, RLG_CRTC_PITCH, 640u / 8u, 4);              /* units of 8 pixels */
    rlg_mmio_write(d, RLG_CRTC_GEN_CNTL, RLG_CRTC_EXT_DISP_EN | RLG_CRTC_EN | RLG_CRTC_32BPP, 4);
}

static void set_mode_avivo(RLGDevice *d)
{
    rlg_mmio_write(d, RLG_D1GRPH_CONTROL, 2, 4);                  /* 32 bpp ARGB8888 */
    rlg_mmio_write(d, RLG_D1GRPH_PRIMARY_SURFACE_ADDR, (uint32_t)rlg_fb_start(d), 4);
    rlg_mmio_write(d, RLG_D1GRPH_PITCH, 640, 4);
    rlg_mmio_write(d, RLG_D1GRPH_X_END, 640, 4);
    rlg_mmio_write(d, RLG_D1GRPH_Y_END, 480, 4);
    rlg_mmio_write(d, RLG_D1GRPH_ENABLE, 1, 4);
    rlg_mmio_write(d, RLG_D1CRTC_CONTROL, 1, 4);
}

int main(int argc, char **argv)
{
    RLGProfile p = RLG_PROFILE_X1250_AMD;
    RLGConfig c;
    RLGDevice *d;
    RLGDisplayMode m;
    uint32_t px = 0;

    if (argc > 1 && !rlg_profile_parse(argv[1], &p)) {
        fprintf(stderr, "unknown profile '%s' (%s)\n", argv[1], rlg_profile_models());
        return 2;
    }
    c = (RLGConfig){ .profile = p, .backend = RLG_BACKEND_SOFTWARE, .irq = irq };
    d = rlg_create(&c);
    if (!d) {
        return 1;
    }
    printf("%s\nPCI %04x:%04x VRAM=%u MiB core=%u MHz %s hw_tcl=%s\n", d->chip->name,
           d->chip->vendor_id, d->chip->device_id, d->vram_size / (1024u * 1024u),
           d->chip->nominal_core_mhz, d->chip->ps_profile, d->chip->hw_tcl ? "yes" : "no");

    if (!rlg_family_is_avivo(d->chip->family)) {
        set_mode_legacy(d);
    } else {
        set_mode_avivo(d);
    }
    m = rlg_get_display_mode(d);
    printf("scanout: %ux%u %ubpp pitch=%u %s enabled=%d\n", m.width, m.height, m.bpp,
           m.pitch_bytes, m.avivo ? "AVIVO" : "legacy CRTC", m.enabled);

    /* Solid fill through DST_PITCH_OFFSET, as the radeon driver programs it. */
    rlg_mmio_write(d, RLG_DST_PITCH_OFFSET, ((640u * 4u / 64u) << 22) | (uint32_t)(rlg_fb_start(d) >> 10), 4);
    rlg_mmio_write(d, RLG_DP_GUI_MASTER_CNTL,
                   RLG_GMC_DST_PITCH_OFFSET_CNTL | (RLG_DST_32BPP << RLG_GMC_DST_DATATYPE_SHIFT) |
                   RLG_ROP3_PATCOPY, 4);
    rlg_mmio_write(d, RLG_DP_BRUSH_FRGD_CLR, 0x00ff0000u, 4);
    rlg_mmio_write(d, RLG_DP_CNTL, RLG_DST_X_LEFT_TO_RIGHT | RLG_DST_Y_TOP_TO_BOTTOM, 4);
    rlg_mmio_write(d, RLG_DST_Y_X, (10u << 16) | 10u, 4);
    rlg_mmio_write(d, RLG_DST_WIDTH_HEIGHT, (64u << 16) | 64u, 4);
    memcpy(&px, d->vram + (10u * 640u + 10u) * 4u, 4);
    printf("2D sample=%08x fills=%llu\n", px, (unsigned long long)d->eng2d.fills);
    rlg_destroy(d);
    return 0;
}
