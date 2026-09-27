#include "radeon_r3d.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int r3d_init(RLGDevice *d)
{
    d->r3d = calloc(1, sizeof(*d->r3d));
    return d->r3d ? 0 : -1;
}

void r3d_fini(RLGDevice *d)
{
    if (d->r3d) {
        free(d->r3d->idx_buf);
        free(d->r3d);
        d->r3d = NULL;
    }
}

void r3d_reset(RLGDevice *d)
{
    if (!d->r3d) {
        return;
    }
    free(d->r3d->idx_buf);
    memset(d->r3d, 0, sizeof(*d->r3d));
}

/* Inverse of Mesa pack_float24(): sign bit 23, 7-bit exponent (bias 63), 16-bit mantissa. */
float r3d_float24(uint32_t v)
{
    uint32_t e = (v >> 16) & 0x7fu, m = v & 0xffffu;
    float f;

    if (!(v & 0x7fffffu)) {
        return 0.0f;
    }
    f = ldexpf(1.0f + (float)m / 65536.0f, (int)e - 63);
    return (v & (1u << 23)) ? -f : f;
}

float r3d_half(uint16_t h)
{
    uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 0x1fu, m = h & 0x3ffu;
    float f;

    if (e == 0) {
        f = ldexpf((float)m, -24);
    } else if (e == 31) {
        f = m ? NAN : INFINITY;
    } else {
        f = ldexpf(1.0f + (float)m / 1024.0f, (int)e - 15);
    }
    return s ? -f : f;
}

static unsigned us_bank(const RLGDevice *d)
{
    return rlg_reg_read32(d, RLG_R400_US_CODE_BANK) & 0xfu;
}

bool r3d_reg_write(RLGDevice *d, uint32_t a, uint32_t v)
{
    struct RLG3D *r = d->r3d;

    if (!r || a < RLG_R3D_BASE || a >= RLG_R3D_END) {
        return false;
    }
    switch (a) {
    case RLG_VAP_PVS_VECTOR_INDX_REG:
        r->pvs_index = v & 0x7ffu;
        r->pvs_sub = 0;
        return true;
    case RLG_VAP_PVS_UPLOAD_DATA:
        if (r->pvs_index < R3D_PVS_VECTORS) {
            r->pvs[r->pvs_index][r->pvs_sub] = v;
        }
        if (++r->pvs_sub == 4) {
            r->pvs_sub = 0;
            r->pvs_index++;
        }
        return true;
    case RLG_VAP_PORT_IDX0:
        r3d_port_idx(d, v);
        return true;
    default:
        break;
    }
    /* US program memory: the 64-entry register windows are banked on R400. */
    if (a >= RLG_US_ALU_RGB_ADDR_0 && a < RLG_R400_US_ALU_EXT_ADDR_0 + 64u * 4u) {
        uint32_t win = (a - RLG_US_ALU_RGB_ADDR_0) / 0x100u, i = ((a - RLG_US_ALU_RGB_ADDR_0) & 0xffu) / 4u;
        uint32_t slot = us_bank(d) * 64u + i;
        uint32_t *dst[5] = { r->alu_rgb_addr, r->alu_alpha_addr, r->alu_rgb_inst,
                             r->alu_alpha_inst, r->alu_ext };
        if (win < 5 && slot < R3D_US_ALU_MAX) {
            dst[win][slot] = v;
        }
        return true;
    }
    if (a >= RLG_US_TEX_INST_0 && a < RLG_US_TEX_INST_0 + 32u * 4u) {
        uint32_t slot = us_bank(d) * 32u + (a - RLG_US_TEX_INST_0) / 4u;
        if (slot < R3D_US_TEX_MAX) {
            r->tex_inst[slot] = v;
        }
        return true;
    }
    return false;
}
