#include "radeon_r3d.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * R300/R400 fragment program (US) interpreter.
 * Encoding: Mesa compiler/r300_fragprog_emit.c, r300_fragprog_swizzle.c and the
 * R300_ALU_* / R300_TEX_* fields of r300_reg.h. Each ALU instruction is a
 * vec3 (RGB) + scalar (alpha) pair sharing up to three RGB and three alpha
 * sources; texture instructions run at the start of each indirection node.
 */


static uint8_t decode_src(uint32_t addr, unsigned j, bool ext_msb)
{
    uint32_t a = (addr >> (6 * j)) & 0x3fu;
    uint8_t idx = (uint8_t)((a & 0x1fu) | (ext_msb ? 0x20u : 0u));
    return (a & 0x20u) ? (uint8_t)(idx | R3D_SRC_CONST) : idx;
}

R3DFragProg *r3d_fs_build(RLGDevice *d)
{
    struct RLG3D *r = d->r3d;
    R3DFragProg *p = calloc(1, sizeof(*p));
    uint32_t config = rlg_reg_read32(d, RLG_US_CONFIG);
    uint32_t ext = rlg_reg_read32(d, RLG_R400_US_CODE_EXT);
    unsigned last = config & 3u;

    if (!p) {
        return NULL;
    }
    p->nnodes = last + 1;
    for (unsigned i = 0; i <= last; ++i) {
        /* Nodes are right-aligned in CODE_ADDR_0..3; R400 MSBs as Mesa writes them. */
        uint32_t ca = rlg_reg_read32(d, RLG_US_CODE_ADDR_0 + (3 - last + i) * 4u);
        unsigned msb_shift = 6 + 6 * (3 - i);
        unsigned alu_start = (ca & 63u) | (((ext >> msb_shift) & 7u) << 6);
        unsigned alu_size = ((ca >> 6) & 63u) | (((ext >> (msb_shift + 3)) & 7u) << 6);
        unsigned tex_start = ((ca >> 12) & 31u) | (((ca >> 24) & 0xfu) << 5);
        unsigned tex_size = ((ca >> 17) & 31u) | (((ca >> 28) & 0xfu) << 5);
        bool has_tex = i > 0 || (config & (1u << 3));

        p->node[i].alu_start = alu_start;
        p->node[i].alu_count = alu_size + 1;
        p->node[i].tex_start = tex_start;
        p->node[i].tex_count = has_tex ? tex_size + 1 : 0;
    }

    for (unsigned k = 0; k < R3D_US_ALU_MAX; ++k) {
        ALUInst *a = &p->alu[k];
        uint32_t ra = r->alu_rgb_addr[k], aa = r->alu_alpha_addr[k];
        uint32_t ri = r->alu_rgb_inst[k], ai = r->alu_alpha_inst[k], x = r->alu_ext[k];
        for (unsigned j = 0; j < 3; ++j) {
            a->rgb_src[j] = decode_src(ra, j, x & (1u << j));
            a->a_src[j] = decode_src(aa, j, x & (1u << (j + 4)));
            a->rgb_arg[j] = (uint8_t)((ri >> (7 * j)) & 0x7fu);
            a->a_arg[j] = (uint8_t)((ai >> (7 * j)) & 0x7fu);
        }
        a->rgb_presub = (ri >> 21) & 3u;
        a->a_presub = (ai >> 21) & 3u;
        a->rgb_op = (ri >> 23) & 15u;
        a->a_op = (ai >> 23) & 15u;
        a->rgb_omod = (ri >> 27) & 7u;
        a->a_omod = (ai >> 27) & 7u;
        a->rgb_clamp = (ri >> 30) & 1u;
        a->a_clamp = (ai >> 30) & 1u;
        a->rgb_dst = (uint8_t)(((ra >> 18) & 31u) | ((x & 0x08u) ? 32u : 0u));
        a->rgb_wmask = (ra >> 23) & 7u;
        a->rgb_omask = (ra >> 26) & 7u;
        a->rgb_target = (ra >> 29) & 3u;
        a->a_dst = (uint8_t)(((aa >> 18) & 31u) | ((x & 0x80u) ? 32u : 0u));
        a->a_wreg = (aa >> 23) & 1u;
        a->a_out = (aa >> 24) & 1u;
        a->a_target = (aa >> 25) & 3u;
        a->a_depth = (aa >> 27) & 1u;
    }
    for (unsigned k = 0; k < R3D_US_TEX_MAX; ++k) {
        uint32_t t = r->tex_inst[k];
        p->tex[k].src = (uint8_t)((t & 31u) | ((t & (1u << 19)) ? 32u : 0u));
        p->tex[k].dst = (uint8_t)(((t >> 6) & 31u) | ((t & (1u << 20)) ? 32u : 0u));
        p->tex[k].unit = (t >> 11) & 15u;
        p->tex[k].op = (t >> 15) & 7u;
    }
    for (unsigned c = 0; c < 32; ++c) {
        for (unsigned j = 0; j < 4; ++j) {
            p->consts[c][j] = r3d_float24(rlg_reg_read32(d, RLG_PFS_PARAM_0 + c * 16u + j * 4u));
        }
    }
    return p;
}

void r3d_fs_free(R3DFragProg *p) { free(p); }

static void presub(unsigned mode, const float s0[4], const float s1[4], float o[4])
{
    for (int c = 0; c < 4; ++c) {
        switch (mode) {
        case 0: o[c] = 1.0f - 2.0f * s0[c]; break;
        case 1: o[c] = s1[c] - s0[c]; break;
        case 2: o[c] = s1[c] + s0[c]; break;
        default: o[c] = 1.0f - s0[c]; break;
        }
    }
}

static void rgb_arg(uint8_t arg, float rs[3][4], float as[3][4], const float sp[4], float o[3])
{
    unsigned sel = arg & 31u, n;
    switch (sel) {
    case 0: case 4: case 8: n = sel / 4; o[0] = rs[n][0]; o[1] = rs[n][1]; o[2] = rs[n][2]; break;
    case 1: case 5: case 9: n = sel / 4; o[0] = o[1] = o[2] = rs[n][0]; break;
    case 2: case 6: case 10: n = sel / 4; o[0] = o[1] = o[2] = rs[n][1]; break;
    case 3: case 7: case 11: n = sel / 4; o[0] = o[1] = o[2] = rs[n][2]; break;
    case 12: case 13: case 14: o[0] = o[1] = o[2] = as[sel - 12][3]; break;
    case 15: o[0] = sp[0]; o[1] = sp[1]; o[2] = sp[2]; break;
    case 16: case 17: case 18: case 19: o[0] = o[1] = o[2] = sp[sel - 16]; break;
    case 20: o[0] = o[1] = o[2] = 0.0f; break;
    case 21: o[0] = o[1] = o[2] = 1.0f; break;
    case 22: o[0] = o[1] = o[2] = 0.5f; break;
    case 23: case 24: case 25: n = sel - 23; o[0] = rs[n][1]; o[1] = rs[n][2]; o[2] = rs[n][0]; break;
    case 26: case 27: case 28: n = sel - 26; o[0] = rs[n][2]; o[1] = rs[n][0]; o[2] = rs[n][1]; break;
    default: n = sel - 29; o[0] = as[n][3]; o[1] = rs[n][2]; o[2] = rs[n][1]; break;
    }
    for (int c = 0; c < 3; ++c) {
        if (arg & 0x40u) {
            o[c] = fabsf(o[c]);
        }
        if (arg & 0x20u) {
            o[c] = -o[c];
        }
    }
}

static float a_arg(uint8_t arg, float rs[3][4], float as[3][4], const float sp[4])
{
    unsigned sel = arg & 31u;
    float v;
    if (sel < 9) {
        v = rs[sel / 3][sel % 3];
    } else if (sel < 12) {
        v = as[sel - 9][3];
    } else if (sel < 16) {
        v = sp[sel - 12];
    } else if (sel == 16) {
        v = 0.0f;
    } else if (sel == 17) {
        v = 1.0f;
    } else {
        v = 0.5f;
    }
    if (arg & 0x40u) {
        v = fabsf(v);
    }
    return (arg & 0x20u) ? -v : v;
}

static float omod(float v, unsigned m, bool clamp)
{
    static const float k[8] = { 1.0f, 2.0f, 4.0f, 8.0f, 0.5f, 0.25f, 0.125f, 1.0f };
    v *= k[m & 7u];
    return clamp ? fminf(fmaxf(v, 0.0f), 1.0f) : v;
}

static const float *fetch(const R3DFragProg *p, float t[R3D_MAX_TEMPS][4], uint8_t s)
{
    return (s & R3D_SRC_CONST) ? p->consts[s & 31u] : t[s & 63u];
}

/* Texture instruction on a 2x2 quad: lanes 0 (x,y), 1 (x+1,y), 2 (x,y+1), 3 (x+1,y+1).
 * The implicit LOD comes from the lane differences, as on the hardware. */
static void run_tex_quad(RLGDevice *d, const TEXInst *ti, float t[4][R3D_MAX_TEMPS][4],
                         bool alive[4])
{
    float c[4][4], res[4];

    for (int l = 0; l < 4; ++l) {
        memcpy(c[l], t[l][ti->src & 63u], sizeof(c[l]));
    }
    if (ti->op == TEX_KIL) {
        for (int l = 0; l < 4; ++l) {
            if (c[l][0] < 0.0f || c[l][1] < 0.0f || c[l][2] < 0.0f || c[l][3] < 0.0f) {
                alive[l] = false;
            }
        }
        return;
    }
    if (ti->op != TEX_LD && ti->op != TEX_TXP && ti->op != TEX_TXB) {
        return;
    }
    if (ti->op == TEX_TXP) {
        for (int l = 0; l < 4; ++l) {
            if (c[l][3] != 0.0f) {
                c[l][0] /= c[l][3]; c[l][1] /= c[l][3]; c[l][2] /= c[l][3];
            }
        }
    }
    float dx[2] = { c[1][0] - c[0][0], c[1][1] - c[0][1] };
    float dy[2] = { c[2][0] - c[0][0], c[2][1] - c[0][1] };
    float lod = r3d_tex_lod(d, ti->unit, dx, dy);
    for (int l = 0; l < 4; ++l) {
        r3d_tex_sample(d, ti->unit, c[l], lod + (ti->op == TEX_TXB ? c[l][3] : 0.0f), res);
        memcpy(t[l][ti->dst & 63u], res, sizeof(res));
    }
}

static void run_alu(const R3DFragProg *p, const ALUInst *a, float t[R3D_MAX_TEMPS][4],
                    float out[4][4], float *depth, bool *depth_written)
{
    float rs[3][4], as[3][4], spr[4], spa[4], ra[3][3], aa[3], rr[3], ar;

    for (int j = 0; j < 3; ++j) {
        memcpy(rs[j], fetch(p, t, a->rgb_src[j]), 16);
        memcpy(as[j], fetch(p, t, a->a_src[j]), 16);
    }
    presub(a->rgb_presub, rs[0], rs[1], spr);
    presub(a->a_presub, as[0], as[1], spa);
    for (int j = 0; j < 3; ++j) {
        rgb_arg(a->rgb_arg[j], rs, as, spr, ra[j]);
        aa[j] = a_arg(a->a_arg[j], rs, as, spa);
    }

    float dot3 = ra[0][0] * ra[1][0] + ra[0][1] * ra[1][1] + ra[0][2] * ra[1][2];
    float dot4 = dot3 + aa[0] * aa[1];

    switch (a->a_op) {
    case A_DP4: ar = a->rgb_op == RGB_DP3 ? dot3 : dot4; break;
    case A_MIN: ar = fminf(aa[0], aa[1]); break;
    case A_MAX: ar = fmaxf(aa[0], aa[1]); break;
    case A_CND: ar = aa[2] > 0.5f ? aa[0] : aa[1]; break;
    case A_CMP: ar = aa[2] >= 0.0f ? aa[0] : aa[1]; break;
    case A_FRC: ar = aa[0] - floorf(aa[0]); break;
    case A_EX2: ar = exp2f(aa[0]); break;
    case A_LG2: ar = log2f(aa[0]); break;
    case A_RCP: ar = 1.0f / aa[0]; break;
    case A_RSQ: ar = 1.0f / sqrtf(fabsf(aa[0])); break;
    default: ar = aa[0] * aa[1] + aa[2]; break;
    }
    ar = omod(ar, a->a_omod, a->a_clamp);

    for (int c = 0; c < 3; ++c) {
        switch (a->rgb_op) {
        case RGB_DP3: rr[c] = dot3; break;
        case RGB_DP4: rr[c] = dot4; break;
        case RGB_D2A: rr[c] = ra[0][0] * ra[1][0] + ra[0][1] * ra[1][1] + ra[2][0]; break;
        case RGB_MIN: rr[c] = fminf(ra[0][c], ra[1][c]); break;
        case RGB_MAX: rr[c] = fmaxf(ra[0][c], ra[1][c]); break;
        case RGB_CND: rr[c] = ra[2][c] > 0.5f ? ra[0][c] : ra[1][c]; break;
        case RGB_CMP: rr[c] = ra[2][c] >= 0.0f ? ra[0][c] : ra[1][c]; break;
        case RGB_FRC: rr[c] = ra[0][c] - floorf(ra[0][c]); break;
        case RGB_REPL_ALPHA: rr[c] = ar; break;
        default: rr[c] = ra[0][c] * ra[1][c] + ra[2][c]; break;
        }
        if (a->rgb_op != RGB_REPL_ALPHA) {
            rr[c] = omod(rr[c], a->rgb_omod, a->rgb_clamp);
        }
    }

    for (int c = 0; c < 3; ++c) {
        if (a->rgb_wmask & (1u << c)) {
            t[a->rgb_dst & 63u][c] = rr[c];
        }
        if (a->rgb_omask & (1u << c)) {
            out[a->rgb_target][c] = rr[c];
        }
    }
    if (a->a_wreg) {
        t[a->a_dst & 63u][3] = ar;
    }
    if (a->a_out) {
        out[a->a_target][3] = ar;
    }
    if (a->a_depth) {
        *depth = ar;
        *depth_written = true;
    }
}

void r3d_fs_run_quad(RLGDevice *d, const R3DFragProg *p, float temps[4][R3D_MAX_TEMPS][4],
                     float out[4][4][4], float depth[4], bool depth_written[4], bool alive[4])
{
    for (unsigned n = 0; n < p->nnodes; ++n) {
        for (unsigned k = 0; k < p->node[n].tex_count; ++k) {
            unsigned i = p->node[n].tex_start + k;
            if (i < R3D_US_TEX_MAX) {
                run_tex_quad(d, &p->tex[i], temps, alive);
            }
        }
        for (unsigned k = 0; k < p->node[n].alu_count; ++k) {
            unsigned i = p->node[n].alu_start + k;
            if (i >= R3D_US_ALU_MAX) {
                continue;
            }
            for (int l = 0; l < 4; ++l) {
                run_alu(p, &p->alu[i], temps[l], out[l], &depth[l], &depth_written[l]);
            }
        }
    }
}
