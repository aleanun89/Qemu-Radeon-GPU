#include "radeon_legacy_int.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rlg__log(RLGDevice *d, const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    if (!d->cfg.log && !d->cfg.verbose) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (d->cfg.log) {
        d->cfg.log(d->cfg.opaque, buf);
    } else {
        fprintf(stderr, "radeon-legacy: %s\n", buf);
    }
}

static RLGConfig normalize(const RLGConfig *in)
{
    RLGConfig c = {0};
    if (in) {
        c = *in;
    }
    if ((unsigned)c.profile >= RLG_PROFILE_COUNT) {
        c.profile = RLG_PROFILE_X1250_AMD;
    }
    if (c.backend != RLG_BACKEND_VULKAN) {
        c.backend = RLG_BACKEND_SOFTWARE;
    }
    return c;
}

static RLGDevice *create_common(const RLGConfig *cfg, uint8_t *external, uint32_t external_size)
{
    RLGConfig c = normalize(cfg);
    const RLGChipInfo *chip = rlg_chip_info(c.profile);
    RLGDevice *d = calloc(1, sizeof(*d));
    if (!d) {
        return NULL;
    }
    d->cfg = c;
    d->chip = chip;
    d->regs = calloc(RLG_MMIO_SIZE / 4, sizeof(uint32_t));
    if (!d->regs) {
        free(d);
        return NULL;
    }
    if (external) {
        d->vram = external;
        d->vram_size = external_size;
        d->owns_vram = false;
    } else {
        uint32_t want = c.vram_size ? c.vram_size : chip->default_vram_mb * 1024u * 1024u;
        d->vram = calloc(1, want);
        if (!d->vram) {
            free(d->regs);
            free(d);
            return NULL;
        }
        d->vram_size = want;
        d->owns_vram = true;
    }
    rlg_reset(d);
    if (d->cfg.backend == RLG_BACKEND_VULKAN && rlg_host_init(d) != 0) {
        rlg__log(d, "Vulkan unavailable; falling back to software");
        d->cfg.backend = RLG_BACKEND_SOFTWARE;
    }
    return d;
}

RLGDevice *rlg_create(const RLGConfig *cfg) { return create_common(cfg, NULL, 0); }

RLGDevice *rlg_create_with_vram(const RLGConfig *cfg, uint8_t *vram, uint32_t size)
{
    if (!vram || size < (1u << 20)) {
        return NULL;
    }
    return create_common(cfg, vram, size);
}

void rlg_reset(RLGDevice *d)
{
    uint32_t fb_loc;

    if (!d) {
        return;
    }
    memset(d->regs, 0, RLG_MMIO_SIZE);
    memset(d->mc_regs, 0, sizeof(d->mc_regs));
    memset(&d->cp, 0, sizeof(d->cp));
    memset(&d->gart, 0, sizeof(d->gart));
    memset(&d->display, 0, sizeof(d->display));
    memset(&d->eng2d, 0, sizeof(d->eng2d));
    d->mc_index = d->mm_index = 0;
    d->int_cntl = d->int_status = 0;
    d->disp_int_status = d->d1_vblank_status = d->dxmode_int_mask = 0;
    d->d1_frame_count = 0;

    d->eng2d.write_mask = 0xffffffffu;
    d->eng2d.sc_right = d->eng2d.sc_bottom = 0x3fffu;
    d->eng2d.dp_cntl = RLG_DST_X_LEFT_TO_RIGHT | RLG_DST_Y_TOP_TO_BOTTOM;
    d->display.pitch = 640u / 8u;

    /* Firmware normally programs this; default to VRAM at MC address 0. */
    fb_loc = ((d->vram_size - 1u) >> 16) << 16;
    switch (d->chip->family) {
    case RLG_FAMILY_RS400:
    case RLG_FAMILY_R300:
        rlg_reg_write32(d, RLG_MC_FB_LOCATION, fb_loc);
        break;
    case RLG_FAMILY_RS600:
        d->mc_regs[RLG_RS600_MC_FB_LOCATION] = fb_loc;
        d->mc_regs[RLG_RS600_MC_STATUS] = 0x3u;         /* idle */
        break;
    case RLG_FAMILY_RS690:
        d->mc_regs[RLG_RS690_MCCFG_FB_LOCATION] = fb_loc;
        d->mc_regs[RLG_RS690_MC_SYSTEM_STATUS] = 0x1u;  /* idle */
        break;
    }
    if (d->chip->r4xx) {
        /* r420_pipes_init(): num_pipes = ((GB_PIPE_SELECT >> 12) & 3) + 1 quads. */
        unsigned quads = d->chip->pixel_pipes >= 4 ? d->chip->pixel_pipes / 4u : 1u;
        rlg_reg_write32(d, RLG_GB_PIPE_SELECT, ((quads - 1u) & 3u) << 12);
    }
    rlg_reg_write32(d, RLG_CNFG_MEMSIZE, d->vram_size);
    rlg_reg_write32(d, RLG_MC_STATUS, 5);
    rlg_reg_write32(d, RLG_DP_WRITE_MASK, 0xffffffffu);
    rlg__irq_update(d);
}

void rlg_destroy(RLGDevice *d)
{
    if (!d) {
        return;
    }
    rlg_host_destroy(d);
    if (d->owns_vram) {
        free(d->vram);
    }
    free(d->regs);
    free(d);
}

uint32_t rlg_reg_read32(const RLGDevice *d, uint32_t a)
{
    if (!d || a >= RLG_MMIO_SIZE || (a & 3)) {
        return 0xffffffffu;
    }
    return d->regs[a >> 2];
}

void rlg_reg_write32(RLGDevice *d, uint32_t a, uint32_t v)
{
    if (!d || a >= RLG_MMIO_SIZE || (a & 3)) {
        return;
    }
    d->regs[a >> 2] = v;
}
