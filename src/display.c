#include "radeon_legacy_int.h"

static bool avivo_mode(const RLGDevice *d, RLGDisplayMode *m)
{
    uint32_t ctl = rlg_reg_read32(d, RLG_D1GRPH_CONTROL);
    uint32_t xs = rlg_reg_read32(d, RLG_D1GRPH_X_START), xe = rlg_reg_read32(d, RLG_D1GRPH_X_END);
    uint32_t ys = rlg_reg_read32(d, RLG_D1GRPH_Y_START), ye = rlg_reg_read32(d, RLG_D1GRPH_Y_END);
    uint32_t surf = rlg_reg_read32(d, RLG_D1GRPH_PRIMARY_SURFACE_ADDR);
    uint32_t pitch_px = rlg_reg_read32(d, RLG_D1GRPH_PITCH) & 0x3fffu;
    uint32_t offx = rlg_reg_read32(d, RLG_D1GRPH_SURFACE_OFFSET_X) & 0x1fffu;
    uint32_t offy = rlg_reg_read32(d, RLG_D1GRPH_SURFACE_OFFSET_Y) & 0x1fffu;
    uint32_t vram_off, bypp;
    uint64_t start;

    if (!(rlg_reg_read32(d, RLG_D1CRTC_CONTROL) & 1u) ||
        !(rlg_reg_read32(d, RLG_D1GRPH_ENABLE) & 1u)) {
        return false;
    }
    switch (ctl & 3u) {
    case 0: m->bpp = 8; break;
    case 1: m->bpp = ((ctl >> 8) & 7u) == 0 ? 15 : 16; break;  /* ARGB1555 / RGB565 */
    case 2: m->bpp = 32; break;
    default: return false;                                     /* 64 bpp: not supported */
    }
    bypp = (m->bpp + 7u) / 8u;
    m->width = (xe > xs) ? xe - xs : 0;
    m->height = (ye > ys) ? ye - ys : 0;
    if (!m->width || !m->height || m->width > 4096 || m->height > 4096) {
        return false;
    }
    m->pitch_bytes = (pitch_px ? pitch_px : m->width) * bypp;
    start = (uint64_t)surf + (uint64_t)(offy + ys) * m->pitch_bytes + (uint64_t)(offx + xs) * bypp;
    if (!rlg_mc_to_vram(d, start, (uint64_t)m->pitch_bytes * (m->height - 1u) + (uint64_t)m->width * bypp,
                        &vram_off)) {
        return false;
    }
    m->offset_bytes = vram_off;
    m->avivo = true;
    m->enabled = true;
    return true;
}

static void legacy_mode(const RLGDevice *d, RLGDisplayMode *m)
{
    uint32_t c = d->display.crtc_gen_cntl, bypp, off;

    m->enabled = (c & RLG_CRTC_EXT_DISP_EN) && (c & RLG_CRTC_EN) &&
                 !(d->display.crtc_ext_cntl & RLG_CRTC_DISPLAY_DIS);
    m->width = (((d->display.h_total_disp >> 16) & 0x1ffu) + 1u) * 8u;
    m->height = ((d->display.v_total_disp >> 16) & 0x7ffu) + 1u;
    switch (c & RLG_CRTC_PIX_WIDTH_MASK) {
    case RLG_CRTC_8BPP: m->bpp = 8; break;
    case RLG_CRTC_15BPP: m->bpp = 15; break;
    case RLG_CRTC_16BPP: m->bpp = 16; break;
    case RLG_CRTC_24BPP: m->bpp = 24; break;
    case RLG_CRTC_32BPP: m->bpp = 32; break;
    default: m->bpp = 32; m->enabled = false; break;
    }
    bypp = (m->bpp + 7u) / 8u;
    /* CRTC_PITCH is in units of 8 pixels (see radeon_crtc_set_base). */
    m->pitch_bytes = (d->display.pitch & 0x7ffu) * 8u * bypp;
    if (!m->pitch_bytes) {
        m->pitch_bytes = m->width * bypp;
    }
    /* CRTC_OFFSET is relative to the start of the framebuffer aperture. */
    off = d->display.offset & 0x07ffffffu;
    m->offset_bytes = off < d->vram_size ? off : 0;
}

RLGDisplayMode rlg_get_display_mode(const RLGDevice *d)
{
    RLGDisplayMode m = {0};

    if (!d) {
        return m;
    }
    m.refresh_hz = 60;
    if (rlg_family_is_avivo(d->chip->family) && avivo_mode(d, &m)) {
        return m;
    }
    m = (RLGDisplayMode){0};
    m.refresh_hz = 60;
    legacy_mode(d, &m);
    return m;
}

bool rlg_is_display_reg(const RLGDevice *d, uint32_t a)
{
    if (a == RLG_CRTC_GEN_CNTL || a == RLG_CRTC_EXT_CNTL ||
        (a >= RLG_CRTC_H_TOTAL_DISP && a <= RLG_CRTC_PITCH)) {
        return true;
    }
    if (!rlg_family_is_avivo(d->chip->family)) {
        return a == RLG_MC_FB_LOCATION;
    }
    return (a >= RLG_D1CRTC_H_TOTAL && a <= RLG_D1GRPH_UPDATE) ||
           (a >= RLG_D1MODE_VIEWPORT_START && a <= RLG_D1MODE_VIEWPORT_SIZE) ||
           a == RLG_RS600_MC_DATA || a == RLG_RS690_MC_DATA;
}
