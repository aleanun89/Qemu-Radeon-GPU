#include "radeon_r3d.h"
#include <math.h>
#include <string.h>

/* Vertex fetch (VAP_PROG_STREAM_CNTL), the PVS vertex program interpreter and
 * the viewport transform. Encodings: Mesa r300_reg.h (PVS_*, VE_*, ME_*) and
 * compiler/r3xx_vertprog.c. */

enum {
    DT_FLOAT_1 = 0, DT_FLOAT_2, DT_FLOAT_3, DT_FLOAT_4, DT_BYTE, DT_D3DCOLOR,
    DT_SHORT_2, DT_SHORT_4, DT_VEC3_TTT, DT_VEC3_EET, DT_FLOAT_8, DT_FLT16_2, DT_FLT16_4,
};
#define PSC_DST_VEC_LOC(h)  (((h) >> 8) & 0x1fu)
#define PSC_LAST_VEC        (1u << 13)
#define PSC_SIGNED          (1u << 14)
#define PSC_NORMALIZE       (1u << 15)

void r3d_vertex_setup(RLGDevice *d, R3DVertexSetup *vs)
{
    memset(vs, 0, sizeof(*vs));
    vs->tcl = !(rlg_reg_read32(d, RLG_VAP_CNTL_STATUS) & RLG_VAP_TCL_BYPASS);
    for (unsigned i = 0; i < 16; ++i) {
        uint32_t c = rlg_reg_read32(d, RLG_VAP_PROG_STREAM_CNTL_0 + (i / 2) * 4u);
        uint32_t e = rlg_reg_read32(d, RLG_VAP_PROG_STREAM_CNTL_EXT_0 + (i / 2) * 4u);
        vs->stream[i] = (i & 1) ? c >> 16 : c & 0xffffu;
        vs->ext[i] = (i & 1) ? e >> 16 : e & 0xffffu;
        vs->nstreams = i + 1;
        if (vs->stream[i] & PSC_LAST_VEC) {
            break;
        }
    }
    uint32_t cc = rlg_reg_read32(d, RLG_VAP_PVS_CODE_CNTL_0);
    vs->pvs_first = cc & 0x3ffu;
    vs->pvs_last = (cc >> 20) & 0x3ffu;
    vs->const_base = rlg_reg_read32(d, RLG_VAP_PVS_CONST_CNTL) & 0xffffu;
    if (rlg_reg_read32(d, RLG_VAP_PVS_FLOW_CNTL_OPC)) {
        d->r3d->stats.unsupported++;    /* PVS flow control: executed linearly */
    }
}

static unsigned type_bytes(unsigned t)
{
    switch (t) {
    case DT_FLOAT_1: return 4;
    case DT_FLOAT_2: return 8;
    case DT_FLOAT_3: return 12;
    case DT_FLOAT_4: return 16;
    case DT_SHORT_4: case DT_FLT16_4: return 8;
    case DT_FLOAT_8: return 32;
    default: return 4;
    }
}

static float norm8(uint8_t b, bool sgn, bool norm)
{
    if (sgn) {
        float f = (float)(int8_t)b;
        return norm ? (f < -127.0f ? -1.0f : f / 127.0f) : f;
    }
    return norm ? (float)b / 255.0f : (float)b;
}

static float norm16(uint16_t s, bool sgn, bool norm)
{
    if (sgn) {
        float f = (float)(int16_t)s;
        return norm ? (f < -32767.0f ? -1.0f : f / 32767.0f) : f;
    }
    return norm ? (float)s / 65535.0f : (float)s;
}

/* Decode one attribute into (x, y, z, w); missing components default to 0,0,0,1. */
static void decode_attr(unsigned t, bool sgn, bool norm, const uint8_t *p, float v[4])
{
    v[0] = v[1] = v[2] = 0.0f;
    v[3] = 1.0f;
    switch (t) {
    case DT_FLOAT_1: case DT_FLOAT_2: case DT_FLOAT_3: case DT_FLOAT_4:
        memcpy(v, p, (t + 1u) * 4u);
        break;
    case DT_BYTE:
        for (int i = 0; i < 4; ++i) {
            v[i] = norm8(p[i], sgn, norm);
        }
        break;
    case DT_D3DCOLOR:   /* memory B, G, R, A -> x = R ... always normalized */
        v[0] = p[2] / 255.0f;
        v[1] = p[1] / 255.0f;
        v[2] = p[0] / 255.0f;
        v[3] = p[3] / 255.0f;
        break;
    case DT_SHORT_2: case DT_SHORT_4:
        for (unsigned i = 0; i < (t == DT_SHORT_2 ? 2u : 4u); ++i) {
            uint16_t s;
            memcpy(&s, p + 2 * i, 2);
            v[i] = norm16(s, sgn, norm);
        }
        break;
    case DT_FLT16_2: case DT_FLT16_4:
        for (unsigned i = 0; i < (t == DT_FLT16_2 ? 2u : 4u); ++i) {
            uint16_t s;
            memcpy(&s, p + 2 * i, 2);
            v[i] = r3d_half(s);
        }
        break;
    default:
        break;      /* packed 10/11-bit vectors, FLOAT_8: not supported */
    }
}

/* Apply PROG_STREAM_CNTL_EXT: per-component source select and write mask. */
static void write_swizzled(float dst[4], const float src[4], uint32_t ext)
{
    for (unsigned c = 0; c < 4; ++c) {
        unsigned sel = (ext >> (3 * c)) & 7u;
        float val = sel < 4 ? src[sel] : (sel == 5 ? 1.0f : 0.0f);
        if (ext & (1u << (12 + c))) {
            dst[c] = val;
        }
    }
}

/* ---- PVS interpreter ------------------------------------------------------ */

enum {
    VE_NOP = 0, VE_DOT_PRODUCT, VE_MULTIPLY, VE_ADD, VE_MULTIPLY_ADD, VE_DISTANCE_VECTOR,
    VE_FRACTION, VE_MAXIMUM, VE_MINIMUM, VE_SET_GTE, VE_SET_LT, VE_MULTIPLYX2_ADD,
    VE_MULTIPLY_CLAMP, VE_FLT2FIX_DX, VE_FLT2FIX_DX_RND,
    VE_COND_MUX_EQ = 23, VE_COND_MUX_GT, VE_COND_MUX_GTE, VE_SET_GT, VE_SET_EQ, VE_SET_NE,
};
enum {
    ME_NOP = 0, ME_EXP_BASE2_DX, ME_LOG_BASE2_DX, ME_EXP_BASEE_FF, ME_LIGHT_COEFF_DX,
    ME_POWER_FUNC_FF, ME_RECIP_DX, ME_RECIP_FF, ME_RECIP_SQRT_DX, ME_RECIP_SQRT_FF,
    ME_MULTIPLY, ME_EXP_BASE2_FULL_DX, ME_LOG_BASE2_FULL_DX, ME_POWER_FUNC_FF_CLAMP_B,
    ME_POWER_FUNC_FF_CLAMP_B1, ME_POWER_FUNC_FF_CLAMP_01, ME_SIN, ME_COS,
    ME_LOG_BASE2_IEEE, ME_RECIP_IEEE, ME_RECIP_SQRT_IEEE,
};

typedef struct {
    float temp[128][4];
    float alt[128][4];
    float in[32][4];
    float out[32][4];
    int a0[4];
} PVSRegs;

static void pvs_src(const RLGDevice *d, const R3DVertexSetup *vs, const PVSRegs *r,
                    uint32_t s, float v[4])
{
    unsigned type = s & 3u, idx = (s >> 5) & 0xffu;
    const float *reg;
    float cst[4];

    if (s & (1u << 4)) {                /* relative addressing through A0 */
        idx = (unsigned)((int)idx + r->a0[(s >> 29) & 3u]) & 0x3ffu;
    }
    switch (type) {
    case 1: reg = r->in[idx & 31u]; break;
    case 2: {
        uint32_t vec = R3D_PVS_CONST_START + vs->const_base + idx;
        memcpy(cst, vec < R3D_PVS_VECTORS ? d->r3d->pvs[vec] : (const uint32_t[4]){0}, 16);
        reg = cst;
        break;
    }
    case 3: reg = r->alt[idx & 127u]; break;
    default: reg = r->temp[idx & 127u]; break;
    }
    for (unsigned c = 0; c < 4; ++c) {
        unsigned sel = (s >> (13 + 3 * c)) & 7u;
        float x = sel < 4 ? reg[sel] : (sel == 5 ? 1.0f : 0.0f);
        if (s & (1u << 3)) {
            x = fabsf(x);
        }
        if (s & (1u << (25 + c))) {
            x = -x;
        }
        v[c] = x;
    }
}

static void splat(float r[4], float x) { r[0] = r[1] = r[2] = r[3] = x; }

static void pvs_run(RLGDevice *d, const R3DVertexSetup *vs, PVSRegs *r)
{
    for (uint32_t pc = vs->pvs_first; pc <= vs->pvs_last && pc < 512u; ++pc) {
        const uint32_t *in = d->r3d->pvs[pc];
        uint32_t d0 = in[0], op = d0 & 0x3fu, dtype = (d0 >> 8) & 0xfu;
        uint32_t doff = (d0 >> 13) & 0x7fu, we = (d0 >> 20) & 0xfu;
        bool math = (d0 >> 6) & 1u, macro = (d0 >> 7) & 1u;
        bool sat = math ? (d0 >> 25) & 1u : (d0 >> 24) & 1u;
        float A[4], B[4], C[4], R[4] = {0, 0, 0, 0};

        pvs_src(d, vs, r, in[1], A);
        pvs_src(d, vs, r, in[2], B);
        pvs_src(d, vs, r, in[3], C);

        if (macro) {                    /* PVS_MACRO_OP_2CLK_MADD / M2X_ADD */
            for (int c = 0; c < 4; ++c) {
                R[c] = (op == 1 ? 2.0f : 1.0f) * A[c] * B[c] + C[c];
            }
        } else if (math) {
            float x = A[0];
            switch (op) {
            case ME_EXP_BASE2_DX: {
                float fl = floorf(x);
                R[0] = exp2f(fl); R[1] = x - fl; R[2] = exp2f(x); R[3] = 1.0f;
                break;
            }
            case ME_LOG_BASE2_DX: {
                float ax = fabsf(x), l = log2f(ax), fl = floorf(l);
                R[0] = fl; R[1] = ax / exp2f(fl); R[2] = l; R[3] = 1.0f;
                break;
            }
            case ME_EXP_BASEE_FF: splat(R, expf(x)); break;
            case ME_POWER_FUNC_FF: case ME_POWER_FUNC_FF_CLAMP_B:
            case ME_POWER_FUNC_FF_CLAMP_B1: case ME_POWER_FUNC_FF_CLAMP_01:
                splat(R, powf(x, C[0]));
                break;
            case ME_RECIP_DX: case ME_RECIP_FF: case ME_RECIP_IEEE:
                splat(R, 1.0f / x);
                if (op == ME_RECIP_FF && isinf(R[0])) {
                    splat(R, copysignf(3.402823466e38f, R[0]));
                }
                break;
            case ME_RECIP_SQRT_DX: case ME_RECIP_SQRT_FF: case ME_RECIP_SQRT_IEEE:
                splat(R, 1.0f / sqrtf(fabsf(x)));
                break;
            case ME_MULTIPLY: splat(R, x * C[0]); break;
            case ME_EXP_BASE2_FULL_DX: splat(R, exp2f(x)); break;
            case ME_LOG_BASE2_FULL_DX: case ME_LOG_BASE2_IEEE: splat(R, log2f(fabsf(x))); break;
            case ME_SIN: splat(R, sinf(x)); break;
            case ME_COS: splat(R, cosf(x)); break;
            default: d->r3d->stats.unsupported++; break;
            }
        } else {
            for (int c = 0; c < 4; ++c) {
                switch (op) {
                case VE_MULTIPLY: R[c] = A[c] * B[c]; break;
                case VE_ADD: R[c] = A[c] + B[c]; break;
                case VE_MULTIPLY_ADD: R[c] = A[c] * B[c] + C[c]; break;
                case VE_MULTIPLYX2_ADD: R[c] = 2.0f * A[c] * B[c] + C[c]; break;
                case VE_MULTIPLY_CLAMP: R[c] = fminf(fmaxf(A[c] * B[c], -1.0f), 1.0f); break;
                case VE_FRACTION: R[c] = A[c] - floorf(A[c]); break;
                case VE_MAXIMUM: R[c] = fmaxf(A[c], B[c]); break;
                case VE_MINIMUM: R[c] = fminf(A[c], B[c]); break;
                case VE_SET_GTE: R[c] = A[c] >= B[c] ? 1.0f : 0.0f; break;
                case VE_SET_LT: R[c] = A[c] < B[c] ? 1.0f : 0.0f; break;
                case VE_SET_GT: R[c] = A[c] > B[c] ? 1.0f : 0.0f; break;
                case VE_SET_EQ: R[c] = A[c] == B[c] ? 1.0f : 0.0f; break;
                case VE_SET_NE: R[c] = A[c] != B[c] ? 1.0f : 0.0f; break;
                case VE_FLT2FIX_DX: R[c] = floorf(A[c]); break;
                case VE_FLT2FIX_DX_RND: R[c] = floorf(A[c] + 0.5f); break;
                case VE_COND_MUX_EQ: R[c] = A[c] == 0.0f ? B[c] : C[c]; break;
                case VE_COND_MUX_GT: R[c] = A[c] > 0.0f ? B[c] : C[c]; break;
                case VE_COND_MUX_GTE: R[c] = A[c] >= 0.0f ? B[c] : C[c]; break;
                default: break;
                }
            }
            if (op == VE_DOT_PRODUCT) {
                splat(R, A[0] * B[0] + A[1] * B[1] + A[2] * B[2] + A[3] * B[3]);
            } else if (op == VE_DISTANCE_VECTOR) {
                R[0] = 1.0f; R[1] = A[1] * B[1]; R[2] = A[2]; R[3] = B[3];
            } else if (op > VE_FLT2FIX_DX_RND && op < VE_COND_MUX_EQ) {
                d->r3d->stats.unsupported++;    /* predication ops */
            }
        }
        if (sat) {
            for (int c = 0; c < 4; ++c) {
                R[c] = fminf(fmaxf(R[c], 0.0f), 1.0f);
            }
        }

        float *dst;
        switch (dtype) {
        case 0: dst = r->temp[doff]; break;
        case 1:                         /* A0 */
            for (int c = 0; c < 4; ++c) {
                if (we & (1u << c)) {
                    r->a0[c] = (int)R[c];
                }
            }
            continue;
        case 2: dst = r->out[doff & 31u]; break;
        case 3: dst = r->out[doff & 31u]; splat(R, R[0]); break;
        case 4: dst = r->alt[doff]; break;
        case 5: dst = r->in[doff & 31u]; break;
        default: continue;
        }
        for (int c = 0; c < 4; ++c) {
            if (we & (1u << c)) {
                dst[c] = R[c];
            }
        }
    }
}

/* ---- vertex assembly ------------------------------------------------------- */

int r3d_vertex_process(RLGDevice *d, const R3DVertexSetup *vs, uint32_t index,
                       const uint32_t *imm, R3DVertex *out)
{
    static const float zero1[4] = { 0, 0, 0, 1 };
    PVSRegs regs;
    float (*slot)[4];
    uint32_t imm_off = 0;
    uint32_t fmt0 = rlg_reg_read32(d, RLG_VAP_OUTPUT_VTX_FMT_0);
    uint32_t fmt1 = rlg_reg_read32(d, RLG_VAP_OUTPUT_VTX_FMT_1);

    memset(&regs, 0, sizeof(regs));
    for (int i = 0; i < 32; ++i) {
        memcpy(regs.in[i], zero1, sizeof(zero1));
    }
    slot = regs.in;     /* TCL: PVS inputs; bypass: fixed semantic slots */

    for (unsigned s = 0; s < vs->nstreams; ++s) {
        uint32_t h = vs->stream[s], t = h & 0xfu, n = type_bytes(t);
        uint8_t raw[32] = {0};
        float v[4];

        if (imm) {
            memcpy(raw, (const uint8_t *)imm + imm_off, n);
            imm_off += n;
        } else {
            const R3DVertexArray *a = &d->r3d->varray[s < R3D_MAX_VARRAYS ? s : 0];
            uint64_t addr = (uint64_t)a->addr + (uint64_t)index * a->stride_dw * 4u;
            if (s >= d->r3d->varray_count || rlg_gpu_read(d, addr, raw, n) != 0) {
                return -1;
            }
        }
        decode_attr(t, h & PSC_SIGNED, h & PSC_NORMALIZE, raw, v);
        write_swizzled(slot[PSC_DST_VEC_LOC(h)], v, vs->ext[s]);
    }

    memset(out, 0, sizeof(*out));
    if (vs->tcl) {
        unsigned k = 0;
        pvs_run(d, vs, &regs);
        memcpy(out->pos, regs.out[k++], 16);
        if (fmt0 & (1u << 16)) {
            out->psize = regs.out[k++][0];
        }
        for (unsigned c = 0; c < R3D_MAX_COLORS; ++c) {
            if (fmt0 & (2u << c)) {
                memcpy(out->color[c], regs.out[k++ & 31u], 16);
            }
        }
        for (unsigned t = 0; t < R3D_MAX_TEXCOORD; ++t) {
            if ((fmt1 >> (3 * t)) & 7u) {
                memcpy(out->tex[t], regs.out[k++ & 31u], 16);
            }
        }
    } else {
        memcpy(out->pos, slot[0], 16);
        out->psize = slot[15][0];
        for (unsigned c = 0; c < R3D_MAX_COLORS; ++c) {
            memcpy(out->color[c], slot[2 + c], 16);
        }
        for (unsigned t = 0; t < R3D_MAX_TEXCOORD; ++t) {
            memcpy(out->tex[t], slot[6 + t], 16);
        }
    }
    return 0;
}

void r3d_vertex_vte(RLGDevice *d, R3DVertex *v)
{
    uint32_t vte = rlg_reg_read32(d, RLG_VAP_VTE_CNTL);
    float w = v->pos[3];

    if (!(vte & RLG_VTX_XY_FMT)) {
        float iw = w != 0.0f ? 1.0f / w : 0.0f;
        v->pos[0] *= iw;
        v->pos[1] *= iw;
        if (!(vte & RLG_VTX_Z_FMT)) {
            v->pos[2] *= iw;
        }
        v->rhw = iw;
    } else {
        /* Pre-transformed (SW TCL / blits): W already holds 1/w. */
        v->rhw = w;
    }
    if (vte & RLG_VTE_X_SCALE_ENA) v->pos[0] *= r3d_regf(d, RLG_SE_VPORT_XSCALE);
    if (vte & RLG_VTE_X_OFFSET_ENA) v->pos[0] += r3d_regf(d, RLG_SE_VPORT_XSCALE + 4);
    if (vte & RLG_VTE_Y_SCALE_ENA) v->pos[1] *= r3d_regf(d, RLG_SE_VPORT_XSCALE + 8);
    if (vte & RLG_VTE_Y_OFFSET_ENA) v->pos[1] += r3d_regf(d, RLG_SE_VPORT_XSCALE + 12);
    if (vte & RLG_VTE_Z_SCALE_ENA) v->pos[2] *= r3d_regf(d, RLG_SE_VPORT_XSCALE + 16);
    if (vte & RLG_VTE_Z_OFFSET_ENA) v->pos[2] += r3d_regf(d, RLG_SE_VPORT_XSCALE + 20);
}
