#include "radeon_legacy_int.h"
#include <stdlib.h>
#include <string.h>

static unsigned get_bpp(const RLG2DState *s)
{
    switch (s->dp_datatype & 0xfu) {
    case RLG_DST_8BPP: return 8;
    case RLG_DST_15BPP:
    case RLG_DST_16BPP: return 16;
    case RLG_DST_24BPP: return 24;
    case RLG_DST_32BPP: return 32;
    default: return 0;
    }
}

static uint32_t pixel_mask(unsigned b) { return b >= 4 ? 0xffffffffu : ((1u << (b * 8)) - 1u); }

static uint32_t loadp(const uint8_t *p, unsigned b)
{
    uint32_t v = 0;
    memcpy(&v, p, b);
    return v;
}

static void storep(uint8_t *p, unsigned b, uint32_t v, uint32_t wm)
{
    uint32_t old = loadp(p, b), m = wm & pixel_mask(b), out = (old & ~m) | (v & m);
    memcpy(p, &out, b);
}

/* Resolve a surface row span (MC address) to a VRAM offset. */
static bool span(const RLGDevice *d, uint32_t base, uint32_t pitch, int y, int x, unsigned by,
                 size_t len, uint32_t *off)
{
    uint64_t mc = (uint64_t)base + (uint64_t)y * pitch + (uint64_t)x * by;
    return rlg_mc_to_vram(d, mc, len, off);
}

int rlg_2d_execute(RLGDevice *d)
{
    RLG2DState *s;
    unsigned bpp, by;
    uint32_t dst_base, dst_pitch, src_base, src_pitch, rop, fill;
    bool ltr, ttb;
    int dx0, dy0, sx0, sy0, left, top, right, bottom;

    if (!d) {
        return -1;
    }
    s = &d->eng2d;
    bpp = get_bpp(s);
    if (!bpp) {
        s->rejected++;
        return -2;
    }
    by = (bpp + 7) / 8;

    /* GMC_*_PITCH_OFFSET_CNTL selects explicit vs. DEFAULT_PITCH_OFFSET surfaces. */
    if (s->gui_master_cntl & RLG_GMC_DST_PITCH_OFFSET_CNTL) {
        dst_base = s->dst_offset;
        dst_pitch = s->dst_pitch;
    } else {
        dst_base = s->default_offset;
        dst_pitch = s->default_pitch;
    }
    if (s->gui_master_cntl & RLG_GMC_SRC_PITCH_OFFSET_CNTL) {
        src_base = s->src_offset;
        src_pitch = s->src_pitch;
    } else {
        src_base = s->default_offset;
        src_pitch = s->default_pitch;
    }
    if (!dst_pitch || !s->dst_width || !s->dst_height) {
        s->rejected++;
        return -3;
    }

    ltr = (s->dp_cntl & RLG_DST_X_LEFT_TO_RIGHT) != 0;
    ttb = (s->dp_cntl & RLG_DST_Y_TOP_TO_BOTTOM) != 0;
    dx0 = ltr ? (int)s->dst_x : (int)s->dst_x + 1 - (int)s->dst_width;
    dy0 = ttb ? (int)s->dst_y : (int)s->dst_y + 1 - (int)s->dst_height;
    sx0 = ltr ? (int)s->src_x : (int)s->src_x + 1 - (int)s->dst_width;
    sy0 = ttb ? (int)s->src_y : (int)s->src_y + 1 - (int)s->dst_height;
    left = dx0 < (int)s->sc_left ? (int)s->sc_left : dx0;
    top = dy0 < (int)s->sc_top ? (int)s->sc_top : dy0;
    right = dx0 + (int)s->dst_width - 1;
    bottom = dy0 + (int)s->dst_height - 1;
    if (right > (int)s->sc_right) {
        right = (int)s->sc_right;
    }
    if (bottom > (int)s->sc_bottom) {
        bottom = (int)s->sc_bottom;
    }
    if (left < 0) {
        left = 0;
    }
    if (top < 0) {
        top = 0;
    }
    if (left > right || top > bottom) {
        return 0;
    }

    rop = s->dp_mix & RLG_GMC_ROP3_MASK;
    fill = s->brush_fg;
    if (rop == RLG_ROP3_BLACKNESS) {
        fill = 0;
    } else if (rop == RLG_ROP3_WHITENESS) {
        fill = 0xffffffffu;
    }

    if (rop == RLG_ROP3_PATCOPY || rop == RLG_ROP3_BLACKNESS || rop == RLG_ROP3_WHITENESS) {
        size_t len = (size_t)(right - left + 1) * by;
        for (int y = top; y <= bottom; ++y) {
            uint32_t off;
            if (!span(d, dst_base, dst_pitch, y, left, by, len, &off)) {
                s->rejected++;
                return -4;
            }
            for (size_t x = 0; x < len; x += by) {
                storep(d->vram + off + x, by, fill, s->write_mask);
            }
            if (d->cfg.dirty) {
                d->cfg.dirty(d->cfg.opaque, off, (uint32_t)len);
            }
        }
        s->fills++;
        return 0;
    }

    if (rop == RLG_ROP3_SRCCOPY) {
        size_t rb = (size_t)(right - left + 1) * by;
        int ys = ttb ? top : bottom, ye = ttb ? bottom + 1 : top - 1, step = ttb ? 1 : -1;
        uint8_t *tmp;

        if (!src_pitch) {
            s->rejected++;
            return -5;
        }
        tmp = malloc(rb);
        if (!tmp) {
            return -6;
        }
        for (int y = ys; y != ye; y += step) {
            int sy = sy0 + (y - dy0), sx = sx0 + (left - dx0);
            uint32_t so, doff;
            if (sy < 0 || sx < 0 ||
                !span(d, src_base, src_pitch, sy, sx, by, rb, &so) ||
                !span(d, dst_base, dst_pitch, y, left, by, rb, &doff)) {
                free(tmp);
                s->rejected++;
                return -7;
            }
            memcpy(tmp, d->vram + so, rb);
            if (s->write_mask == 0xffffffffu) {
                memcpy(d->vram + doff, tmp, rb);
            } else {
                for (size_t x = 0; x < rb; x += by) {
                    storep(d->vram + doff + x, by, loadp(tmp + x, by), s->write_mask);
                }
            }
            if (d->cfg.dirty) {
                d->cfg.dirty(d->cfg.opaque, doff, (uint32_t)rb);
            }
        }
        free(tmp);
        s->blits++;
        return 0;
    }

    rlg__log(d, "2D: unsupported ROP3 0x%02x", rop >> 16);
    s->rejected++;
    return -8;
}
