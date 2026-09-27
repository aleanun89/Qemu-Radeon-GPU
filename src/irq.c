#include "radeon_legacy_int.h"

static bool is_avivo(const RLGDevice *d) { return rlg_family_is_avivo(d->chip->family); }

void rlg__irq_update(RLGDevice *d)
{
    bool level = (d->int_status & d->int_cntl) != 0;

    /* RS600/RS690 route display interrupts through DxMODE_INT_MASK, not GEN_INT_CNTL. */
    if (is_avivo(d) && (d->disp_int_status & RLG_LB_D1_VBLANK_INTERRUPT) &&
        (d->dxmode_int_mask & RLG_D1MODE_VBLANK_INT_MASK)) {
        level = true;
    }
    d->irq_level = level;
    if (d->cfg.irq) {
        d->cfg.irq(d->cfg.opaque, level);
    }
}

void rlg_vblank(RLGDevice *d)
{
    if (!d) {
        return;
    }
    if (is_avivo(d)) {
        d->d1_frame_count++;
        d->d1_vblank_status |= RLG_D1MODE_VBLANK_OCCURRED | RLG_D1MODE_VBLANK_STAT;
        if (d->dxmode_int_mask & RLG_D1MODE_VBLANK_INT_MASK) {
            d->d1_vblank_status |= RLG_D1MODE_VBLANK_INTERRUPT;
            d->disp_int_status |= RLG_LB_D1_VBLANK_INTERRUPT;
            d->int_status |= RLG_INT_CRTC_VBLANK;       /* DISPLAY_INT_STAT */
        }
    } else {
        d->int_status |= RLG_INT_CRTC_VBLANK;
    }
    rlg__irq_update(d);
}
