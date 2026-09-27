#include "radeon_legacy_int.h"
#include <string.h>

/* ---- MC indirect index/data pairs (one per family) ------------------------ */

static bool mc_pair(const RLGDevice *d, uint32_t a, bool *is_data)
{
    uint32_t idx;
    switch (d->chip->family) {
    case RLG_FAMILY_RS600: idx = RLG_RS600_MC_INDEX; break;
    case RLG_FAMILY_RS690: idx = RLG_RS690_MC_INDEX; break;
    case RLG_FAMILY_R300: idx = RLG_PCIE_INDEX; break;
    case RLG_FAMILY_RS400:
    default: idx = RLG_RS480_NB_MC_INDEX; break;
    }
    if (a != idx && a != idx + 4u) {
        return false;
    }
    *is_data = (a == idx + 4u);
    return true;
}

static uint32_t mc_index_reg(const RLGDevice *d)
{
    switch (d->chip->family) {
    case RLG_FAMILY_RS600: return d->mc_index & RLG_RS600_MC_ADDR_MASK;
    case RLG_FAMILY_RS690: return d->mc_index & RLG_RS690_MC_INDEX_MASK;
    case RLG_FAMILY_R300: return d->mc_index & RLG_PCIE_REG_MASK;
    case RLG_FAMILY_RS400:
    default: return d->mc_index & 0xffu;
    }
}

static bool mc_write_enabled(const RLGDevice *d)
{
    switch (d->chip->family) {
    case RLG_FAMILY_RS600: return (d->mc_index & RLG_RS600_MC_IND_WR_EN) != 0;
    case RLG_FAMILY_RS690: return (d->mc_index & RLG_RS690_MC_INDEX_WR_EN) != 0;
    case RLG_FAMILY_R300: return true;                 /* PCIE_DATA has no write enable */
    case RLG_FAMILY_RS400:
    default: return (d->mc_index & RLG_RS480_NB_MC_IND_WR_EN) != 0;
    }
}

/* ---- reads ------------------------------------------------------------------ */

static uint32_t r32(RLGDevice *d, uint32_t a)
{
    bool is_data;

    if (mc_pair(d, a, &is_data)) {
        return is_data ? rlg__mc_read(d, mc_index_reg(d)) : d->mc_index;
    }
    switch (a) {
    case RLG_MM_INDEX: return d->mm_index;
    case RLG_GEN_INT_CNTL: return d->int_cntl;
    case RLG_GEN_INT_STATUS: return d->int_status;
    case RLG_CNFG_MEMSIZE: return d->vram_size;
    case RLG_MC_STATUS: return 5u;
    case RLG_HOST_PATH_CNTL: return 1u << 23;
    case RLG_RBBM_STATUS:
    case RLG_GUI_STAT:
        return (d->cp.busy ? RLG_RBBM_ACTIVE : 0u) | 64u;   /* 64 free FIFO entries */
    case RLG_CP_RB_BASE: return d->cp.base;
    case RLG_CP_RB_CNTL: return d->cp.cntl;
    case RLG_CP_RB_RPTR_ADDR: return d->cp.rptr_addr;
    case RLG_CP_RB_RPTR: return d->cp.rptr;
    case RLG_CP_RB_WPTR: return d->cp.wptr;
    case RLG_CP_CSQ_CNTL: return d->cp.csq_cntl;
    case RLG_CP_CSQ_STAT: return 0;                          /* queues empty */
    case RLG_CP_IB_BASE: return d->cp.ib_base;
    case RLG_SCRATCH_UMSK: return d->cp.scratch_umsk;
    case RLG_SCRATCH_ADDR: return d->cp.scratch_addr;
    default: break;
    }
    if (rlg_family_is_avivo(d->chip->family)) {
        switch (a) {
        case RLG_D1MODE_VBLANK_STATUS: return d->d1_vblank_status;
        case RLG_DXMODE_INT_MASK: return d->dxmode_int_mask;
        case RLG_DISP_INTERRUPT_STATUS: return d->disp_int_status;
        case RLG_D1CRTC_STATUS_FRAME_COUNT: return d->d1_frame_count;
        case RLG_D1CRTC_STATUS: {
            /* Toggle V_BLANK so driver "wait for vblank" loops make progress. */
            uint32_t v = rlg_reg_read32(d, a) ^ 1u;
            rlg_reg_write32(d, a, v);
            return v;
        }
        default: break;
        }
    }
    return rlg_reg_read32(d, a);
}

/* ---- 2D register decode ----------------------------------------------------- */

static void decode_pitch_offset(uint32_t v, uint32_t *offset, uint32_t *pitch)
{
    *offset = (v & 0x003fffffu) << 10;
    *pitch = ((v >> 22) & 0xffu) * 64u;
}

static void update2d(RLGDevice *d, uint32_t a, uint32_t v)
{
    RLG2DState *s = &d->eng2d;

    switch (a) {
    case RLG_DST_OFFSET: s->dst_offset = v & 0xfffffff0u; break;
    case RLG_DST_PITCH: s->dst_pitch = v & 0x3fffu; break;
    case RLG_SRC_OFFSET: s->src_offset = v & 0xfffffff0u; break;
    case RLG_SRC_PITCH: s->src_pitch = v & 0x3fffu; break;
    case RLG_SRC_PITCH_OFFSET: decode_pitch_offset(v, &s->src_offset, &s->src_pitch); break;
    case RLG_DST_PITCH_OFFSET: decode_pitch_offset(v, &s->dst_offset, &s->dst_pitch); break;
    case RLG_DEFAULT_PITCH_OFFSET: decode_pitch_offset(v, &s->default_offset, &s->default_pitch); break;
    case RLG_SRC_X: s->src_x = v & 0x3fffu; break;
    case RLG_SRC_Y: s->src_y = v & 0x3fffu; break;
    case RLG_DST_X: s->dst_x = v & 0x3fffu; break;
    case RLG_DST_Y: s->dst_y = v & 0x3fffu; break;
    case RLG_SRC_Y_X: s->src_x = v & 0x3fffu; s->src_y = (v >> 16) & 0x3fffu; break;
    case RLG_DST_Y_X: s->dst_x = v & 0x3fffu; s->dst_y = (v >> 16) & 0x3fffu; break;
    case RLG_SRC_X_Y: s->src_y = v & 0x3fffu; s->src_x = (v >> 16) & 0x3fffu; break;
    case RLG_DST_X_Y: s->dst_y = v & 0x3fffu; s->dst_x = (v >> 16) & 0x3fffu; break;
    case RLG_DP_GUI_MASTER_CNTL:
        s->gui_master_cntl = v;
        s->dp_datatype = (s->dp_datatype & ~0xfu) |
                         ((v & RLG_GMC_DST_DATATYPE_MASK) >> RLG_GMC_DST_DATATYPE_SHIFT);
        s->dp_mix = (s->dp_mix & ~RLG_GMC_ROP3_MASK) | (v & RLG_GMC_ROP3_MASK);
        break;
    case RLG_DP_BRUSH_FRGD_CLR: s->brush_fg = v; break;
    case RLG_DP_BRUSH_BKGD_CLR: s->brush_bg = v; break;
    case RLG_DP_SRC_FRGD_CLR: s->src_fg = v; break;
    case RLG_DP_SRC_BKGD_CLR: s->src_bg = v; break;
    case RLG_DP_CNTL: s->dp_cntl = v; break;
    case RLG_DP_DATATYPE: s->dp_datatype = v; break;
    case RLG_DP_MIX: s->dp_mix = v; break;
    case RLG_DP_WRITE_MASK: s->write_mask = v; break;
    case RLG_SC_LEFT: s->sc_left = v & 0x3fffu; break;
    case RLG_SC_RIGHT: s->sc_right = v & 0x3fffu; break;
    case RLG_SC_TOP: s->sc_top = v & 0x3fffu; break;
    case RLG_SC_BOTTOM: s->sc_bottom = v & 0x3fffu; break;
    case RLG_SC_TOP_LEFT: s->sc_left = v & 0x3fffu; s->sc_top = (v >> 16) & 0x3fffu; break;
    case RLG_SC_BOTTOM_RIGHT:
    case RLG_DEFAULT_SC_BOTTOM_RIGHT:
        s->sc_right = v & 0x3fffu;
        s->sc_bottom = (v >> 16) & 0x3fffu;
        break;
    case RLG_DST_WIDTH: s->dst_width = v & 0x3fffu; break;
    case RLG_DST_HEIGHT: s->dst_height = v & 0x3fffu; (void)rlg_2d_execute(d); break;
    case RLG_DST_HEIGHT_WIDTH:
        s->dst_width = v & 0x3fffu;
        s->dst_height = (v >> 16) & 0x3fffu;
        (void)rlg_2d_execute(d);
        break;
    case RLG_DST_WIDTH_HEIGHT:
        s->dst_height = v & 0x3fffu;
        s->dst_width = (v >> 16) & 0x3fffu;
        (void)rlg_2d_execute(d);
        break;
    default: break;
    }
}

/* ---- writes ------------------------------------------------------------------ */

static bool avivo_write(RLGDevice *d, uint32_t a, uint32_t v)
{
    switch (a) {
    case RLG_D1MODE_VBLANK_STATUS:
        if (v & RLG_D1MODE_VBLANK_ACK) {
            d->d1_vblank_status &= ~(RLG_D1MODE_VBLANK_OCCURRED | RLG_D1MODE_VBLANK_INTERRUPT);
            d->disp_int_status &= ~RLG_LB_D1_VBLANK_INTERRUPT;
            d->int_status &= ~RLG_INT_CRTC_VBLANK;
            rlg__irq_update(d);
        }
        return true;
    case RLG_DXMODE_INT_MASK:
        d->dxmode_int_mask = v;
        rlg__irq_update(d);
        return true;
    case RLG_DISP_INTERRUPT_STATUS:
    case RLG_D1CRTC_STATUS_FRAME_COUNT:
        return true;                                        /* read-only */
    default:
        return false;
    }
}

static void w32(RLGDevice *d, uint32_t a, uint32_t v)
{
    bool is_data;

    /* Raw copy first: sub-dword writes merge against it. */
    rlg_reg_write32(d, a, v);

    if (mc_pair(d, a, &is_data)) {
        if (!is_data) {
            d->mc_index = v;
        } else if (mc_write_enabled(d)) {
            rlg__mc_write(d, mc_index_reg(d), v);
        }
        return;
    }
    if (rlg_family_is_avivo(d->chip->family) && avivo_write(d, a, v)) {
        return;
    }

    if (a >= RLG_SCRATCH_REG0 && a < RLG_SCRATCH_REG0 + RLG_NUM_SCRATCH * 4u) {
        rlg__scratch_writeback(d, (a - RLG_SCRATCH_REG0) / 4u, v);
        return;
    }
    switch (a) {
    case RLG_MM_INDEX: d->mm_index = v; return;
    case RLG_GEN_INT_CNTL: d->int_cntl = v; rlg__irq_update(d); return;
    case RLG_GEN_INT_STATUS:
        /* Write-1-to-clear; SW_INT_FIRE raises SW_INT (fence interrupt). */
        d->int_status &= ~(v & ~RLG_INT_SW_FIRE);
        if (v & RLG_INT_SW_FIRE) {
            d->int_status |= RLG_INT_SW;
        }
        rlg__irq_update(d);
        return;
    case RLG_GEN_RESET_CNTL:
        if (v & RLG_SOFT_RESET_CP) {
            d->cp.rptr = d->cp.wptr = 0;
            d->cp.busy = false;
        }
        return;
    case RLG_CRTC_GEN_CNTL: d->display.crtc_gen_cntl = v; return;
    case RLG_CRTC_EXT_CNTL: d->display.crtc_ext_cntl = v; return;
    case RLG_CRTC_H_TOTAL_DISP: d->display.h_total_disp = v; return;
    case RLG_CRTC_H_SYNC_STRT_WID: d->display.h_sync = v; return;
    case RLG_CRTC_V_TOTAL_DISP: d->display.v_total_disp = v; return;
    case RLG_CRTC_V_SYNC_STRT_WID: d->display.v_sync = v; return;
    case RLG_CRTC_OFFSET: d->display.offset = v; return;
    case RLG_CRTC_OFFSET_CNTL: d->display.offset_cntl = v; return;
    case RLG_CRTC_PITCH: d->display.pitch = v; return;
    case RLG_MC_AGP_LOCATION:
    case RLG_AIC_CNTL:
    case RLG_AIC_PT_BASE:
    case RLG_AIC_LO_ADDR:
    case RLG_AIC_HI_ADDR:
        rlg__gart_update(d);
        return;
    case RLG_CP_RB_BASE: d->cp.base = v; return;
    case RLG_CP_RB_CNTL: d->cp.cntl = v; return;
    case RLG_CP_RB_RPTR_ADDR: d->cp.rptr_addr = v; return;
    case RLG_CP_RB_RPTR_WR:
        if (d->cp.cntl & RLG_RB_RPTR_WR_ENA) {
            d->cp.rptr = v;
        }
        return;
    case RLG_CP_RB_WPTR:
        d->cp.wptr = v;
        (void)rlg_cp_process(d);
        return;
    case RLG_CP_CSQ_CNTL:
        d->cp.csq_cntl = v;
        (void)rlg_cp_process(d);
        return;
    case RLG_CP_IB_BASE: d->cp.ib_base = v; return;
    case RLG_CP_IB_BUFSZ: (void)rlg__cp_exec_ib(d, d->cp.ib_base, v & 0x7fffffu); return;
    case RLG_SCRATCH_UMSK: d->cp.scratch_umsk = v; return;
    case RLG_SCRATCH_ADDR: d->cp.scratch_addr = v & ~0x1fu; return;
    default: update2d(d, a, v); return;
    }
}

/* ---- sized access ------------------------------------------------------------ */

static uint64_t size_mask(unsigned s) { return s >= 4 ? 0xffffffffull : ((1ull << (s * 8u)) - 1ull); }

/* MM_INDEX/MM_DATA: bit31 selects the framebuffer, otherwise a register offset. */
static bool mm_data_target(RLGDevice *d, uint32_t addr, unsigned s, bool *vram, uint32_t *target)
{
    uint32_t off = (d->mm_index & 0x7fffffffu) + (addr - RLG_MM_DATA);

    *vram = (d->mm_index & 0x80000000u) != 0;
    *target = off;
    if (*vram) {
        return off <= d->vram_size && s <= d->vram_size - off;
    }
    /* Never allow MM_DATA to alias MM_INDEX/MM_DATA themselves (recursion). */
    return (d->mm_index & 0x7fffffffu) > RLG_MM_DATA + 3u && off + s <= RLG_MMIO_SIZE;
}

uint64_t rlg_mmio_read(RLGDevice *d, uint32_t a, unsigned s)
{
    uint32_t v;

    if (!d || !s || s > 4 || a >= RLG_MMIO_SIZE || s > RLG_MMIO_SIZE - a) {
        return ~0ull;
    }
    if (a >= RLG_MM_DATA && a < RLG_MM_DATA + 4u) {
        bool vram;
        uint32_t t;
        if (!mm_data_target(d, a, s, &vram, &t)) {
            rlg__log(d, "MM_DATA read with bad MM_INDEX 0x%08x", d->mm_index);
            return 0;
        }
        if (vram) {
            v = 0;
            memcpy(&v, d->vram + t, s);
            return v;
        }
        return rlg_mmio_read(d, t, s);
    }
    v = r32(d, a & ~3u);
    return ((uint64_t)v >> ((a & 3u) * 8u)) & size_mask(s);
}

static bool is_w1c(uint32_t a)
{
    return a == RLG_GEN_INT_STATUS || a == RLG_D1MODE_VBLANK_STATUS;
}

void rlg_mmio_write(RLGDevice *d, uint32_t a, uint64_t val, unsigned s)
{
    uint32_t b = a & ~3u, sh = (a & 3u) * 8u, mask, nv;

    if (!d || !s || s > 4 || a >= RLG_MMIO_SIZE || s > RLG_MMIO_SIZE - a) {
        return;
    }
    if (a >= RLG_MM_DATA && a < RLG_MM_DATA + 4u) {
        bool vram;
        uint32_t t;
        if (!mm_data_target(d, a, s, &vram, &t)) {
            rlg__log(d, "MM_DATA write with bad MM_INDEX 0x%08x", d->mm_index);
            return;
        }
        if (vram) {
            uint32_t v32 = (uint32_t)val;
            memcpy(d->vram + t, &v32, s);
            if (d->cfg.dirty) {
                d->cfg.dirty(d->cfg.opaque, t, s);
            }
        } else {
            rlg_mmio_write(d, t, val, s);
        }
        return;
    }
    if (s == 4 && !(a & 3u)) {
        w32(d, b, (uint32_t)val);
        return;
    }
    mask = (uint32_t)(size_mask(s) << sh);
    if (is_w1c(b)) {
        nv = ((uint32_t)val << sh) & mask;          /* don't re-ack untouched bytes */
    } else {
        nv = (rlg_reg_read32(d, b) & ~mask) | (((uint32_t)val << sh) & mask);
    }
    w32(d, b, nv);
}
