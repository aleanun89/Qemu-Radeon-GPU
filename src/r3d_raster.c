#include "radeon_r3d.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * GA/SU/SC/RS + FG/ZB/RB3D: primitive assembly, clipping, culling, scan
 * conversion in 2x2 quads, rasterizer interpolators feeding the fragment
 * program, and the per-fragment operations. Register fields: Mesa r300_reg.h.
 */

enum {
    PRIM_POINTS = 1, PRIM_LINES, PRIM_LINE_STRIP, PRIM_TRIANGLES, PRIM_TRIANGLE_FAN,
    PRIM_TRIANGLE_STRIP, PRIM_LINE_LOOP = 12, PRIM_QUADS, PRIM_QUAD_STRIP, PRIM_POLYGON,
};

/* Per-draw state shared by every primitive. */
typedef struct {
    RLGDevice *d;
    R3DDrawInfo info;
    bool clip;                          /* clip-space input: clip against near/far/w */
    bool dx_clip;                       /* z in [0, w] instead of [-w, w] */
    uint32_t fmt1;
    uint32_t cull;
    bool is_point;
    R3DTri *tris;                       /* collect mode (host backend): triangles */
    unsigned ntris, cap;
    bool collect_failed;
} DrawCtx;

/* ---- helpers ------------------------------------------------------------------- */

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

static void lerp_vertex(const R3DVertex *a, const R3DVertex *b, float t, R3DVertex *o)
{
    const float *pa = (const float *)a, *pb = (const float *)b;
    float *po = (float *)o;
    for (size_t i = 0; i < sizeof(R3DVertex) / sizeof(float); ++i) {
        po[i] = pa[i] + (pb[i] - pa[i]) * t;
    }
}

/* ---- per-fragment operations ------------------------------------------------------ */

static bool compare(unsigned func, float a, float b)
{
    switch (func & 7u) {
    case 0: return false;
    case 1: return a < b;
    case 2: return a <= b;
    case 3: return a == b;
    case 4: return a >= b;
    case 5: return a > b;
    case 6: return a != b;
    default: return true;
    }
}

static bool compare_u(unsigned func, uint32_t a, uint32_t b)
{
    return compare(func, (float)a, (float)b);
}

static uint8_t stencil_op(unsigned op, uint8_t s, uint8_t ref)
{
    switch (op & 7u) {
    case 1: return 0;
    case 2: return ref;
    case 3: return s == 255 ? 255 : (uint8_t)(s + 1);
    case 4: return s == 0 ? 0 : (uint8_t)(s - 1);
    case 5: return (uint8_t)~s;
    case 6: return (uint8_t)(s + 1);
    case 7: return (uint8_t)(s - 1);
    default: return s;
    }
}

/* Depth/stencil test and update. Returns true when the fragment survives. */
static bool depth_stencil(RLGDevice *d, int x, int y, float z, bool front)
{
    uint32_t cntl = rlg_reg_read32(d, RLG_ZB_CNTL);
    bool zen = cntl & (1u << 1), zwrite = cntl & (1u << 2), sten = cntl & 1u;
    uint32_t fmt = rlg_reg_read32(d, RLG_ZB_FORMAT) & 0xfu;
    uint32_t zs = rlg_reg_read32(d, RLG_ZB_ZSTENCILCNTL);
    uint32_t refmask = rlg_reg_read32(d, RLG_ZB_STENCILREFMASK);
    uint32_t pitch = rlg_reg_read32(d, RLG_ZB_DEPTHPITCH) & 0x3ffcu;
    uint64_t base = rlg_reg_read32(d, RLG_ZB_DEPTHOFFSET) & ~31u;
    unsigned bpp = fmt == 2 ? 4 : 2;
    uint64_t addr = base + ((uint64_t)y * pitch + (uint64_t)x) * bpp;
    uint32_t stored = 0, zval, zmax = fmt == 2 ? 0xffffffu : 0xffffu, szval;
    uint8_t s = 0, ref = refmask & 0xffu, mask = (refmask >> 8) & 0xffu, wmask = (refmask >> 16) & 0xffu;
    bool pass_s = true, pass_z = true;
    unsigned sh = (front || !(cntl & (1u << 4))) ? 3 : 15;

    if (!zen && !sten) {
        return true;
    }
    if (!pitch || rlg_gpu_read(d, addr, &stored, bpp) != 0) {
        return true;                    /* no depth buffer bound */
    }
    zval = (uint32_t)(clamp01(z) * (float)zmax + 0.5f);
    if (fmt == 2) {
        szval = stored >> 8;
        s = (uint8_t)(stored & 0xffu);
    } else {
        szval = stored & 0xffffu;
    }
    if (sten && fmt == 2) {
        pass_s = compare_u((zs >> sh) & 7u, ref & mask, s & mask);
    }
    if (zen) {
        pass_z = compare_u(zs & 7u, zval, szval);
    }
    if (sten && fmt == 2) {
        unsigned op = !pass_s ? (zs >> (sh + 3)) : (pass_z ? (zs >> (sh + 6)) : (zs >> (sh + 9)));
        uint8_t ns = stencil_op(op, s, ref);
        s = (uint8_t)((s & ~wmask) | (ns & wmask));
    }
    if (pass_s && pass_z && zen && zwrite) {
        szval = zval;
    }
    uint32_t nv = fmt == 2 ? (szval << 8) | s : szval;
    if (nv != stored) {
        rlg_gpu_write(d, addr, &nv, bpp);
    }
    return pass_s && pass_z;
}

static float blend_factor(unsigned f, int c, const float src[4], const float dst[4],
                          const float cst[4])
{
    switch (f) {
    case 32: return 0.0f;
    case 33: return 1.0f;
    case 34: return src[c];
    case 35: return 1.0f - src[c];
    case 36: return dst[c];
    case 37: return 1.0f - dst[c];
    case 38: return src[3];
    case 39: return 1.0f - src[3];
    case 40: return dst[3];
    case 41: return 1.0f - dst[3];
    case 42: return c == 3 ? 1.0f : fminf(src[3], 1.0f - dst[3]);
    case 43: return cst[c];
    case 44: return 1.0f - cst[c];
    case 45: return cst[3];
    case 46: return 1.0f - cst[3];
    default: return 1.0f;
    }
}

static float combine(unsigned fcn, float s, float dv)
{
    float r;
    switch (fcn & 7u) {
    case 2: case 3: r = s - dv; break;
    case 4: r = fminf(s, dv); break;
    case 5: r = fmaxf(s, dv); break;
    case 6: case 7: r = dv - s; break;
    default: r = s + dv; break;
    }
    return (fcn & 1u) ? r : clamp01(r);  /* even FCN values clamp */
}

/* Colour buffer formats (RB3D_COLORPITCH bits 24:21). Channels C0..C3 in memory order. */
enum { CF_ARGB1555 = 3, CF_RGB565 = 4, CF_ARGB8888 = 6, CF_ARGB32323232 = 7, CF_I8 = 9,
       CF_ARGB16161616 = 10, CF_UV88 = 13, CF_ARGB4444 = 15 };

static unsigned cf_bpp(unsigned f)
{
    switch (f) {
    case CF_I8: return 1;
    case CF_ARGB1555: case CF_RGB565: case CF_UV88: case CF_ARGB4444: return 2;
    case CF_ARGB16161616: return 8;
    case CF_ARGB32323232: return 16;
    default: return 4;
    }
}

static uint32_t q(float v, unsigned bits) { return (uint32_t)(clamp01(v) * (float)((1u << bits) - 1u) + 0.5f); }
static float dq(uint32_t v, unsigned bits) { return (float)v / (float)((1u << bits) - 1u); }

static uint16_t to_half(float f)
{
    union { float f; uint32_t u; } c = { f };
    uint32_t s = (c.u >> 16) & 0x8000u, e = (c.u >> 23) & 0xffu, m = c.u & 0x7fffffu;
    int ne = (int)e - 127 + 15;
    if (e == 255) return (uint16_t)(s | 0x7c00u | (m ? 0x200u : 0u));
    if (ne >= 31) return (uint16_t)(s | 0x7c00u);
    if (ne <= 0) return (uint16_t)s;
    return (uint16_t)(s | ((uint32_t)ne << 10) | (m >> 13));
}

static void unpack_cf(unsigned f, bool fp16, const uint8_t *p, float ch[4])
{
    uint32_t v = 0;
    memcpy(&v, p, cf_bpp(f) > 4 ? 4 : cf_bpp(f));
    ch[0] = ch[1] = ch[2] = ch[3] = 0.0f;
    switch (f) {
    case CF_ARGB1555: ch[0] = dq(v & 31u, 5); ch[1] = dq((v >> 5) & 31u, 5); ch[2] = dq((v >> 10) & 31u, 5); ch[3] = (float)((v >> 15) & 1u); break;
    case CF_RGB565: ch[0] = dq(v & 31u, 5); ch[1] = dq((v >> 5) & 63u, 6); ch[2] = dq(v >> 11, 5); ch[3] = 1.0f; break;
    case CF_ARGB4444: for (int i = 0; i < 4; ++i) ch[i] = dq((v >> (4 * i)) & 15u, 4); break;
    case CF_I8: ch[2] = dq(v & 255u, 8); break;
    case CF_UV88: ch[2] = dq(v & 255u, 8); ch[0] = dq((v >> 8) & 255u, 8); break;
    case CF_ARGB16161616:
        for (int i = 0; i < 4; ++i) {
            uint16_t h;
            memcpy(&h, p + 2 * i, 2);
            ch[i] = fp16 ? r3d_half(h) : dq(h, 16);
        }
        break;
    case CF_ARGB32323232: memcpy(ch, p, 16); break;
    default: for (int i = 0; i < 4; ++i) ch[i] = dq((v >> (8 * i)) & 255u, 8); break;
    }
}

static void pack_cf(unsigned f, bool fp16, const float ch[4], unsigned wmask, uint8_t *p)
{
    uint8_t old[16];
    float merged[4], cur[4];
    uint32_t v = 0;
    unsigned n = cf_bpp(f);

    memcpy(old, p, n);
    unpack_cf(f, fp16, old, cur);
    for (int i = 0; i < 4; ++i) {
        merged[i] = (wmask & (1u << i)) ? ch[i] : cur[i];
    }
    switch (f) {
    case CF_ARGB1555: v = q(merged[0], 5) | q(merged[1], 5) << 5 | q(merged[2], 5) << 10 | q(merged[3], 1) << 15; break;
    case CF_RGB565: v = q(merged[0], 5) | q(merged[1], 6) << 5 | q(merged[2], 5) << 11; break;
    case CF_ARGB4444: for (int i = 0; i < 4; ++i) v |= q(merged[i], 4) << (4 * i); break;
    case CF_I8: v = q(merged[2], 8); break;
    case CF_UV88: v = q(merged[2], 8) | q(merged[0], 8) << 8; break;
    case CF_ARGB16161616:
        for (int i = 0; i < 4; ++i) {
            uint16_t h = fp16 ? to_half(merged[i]) : (uint16_t)q(merged[i], 16);
            memcpy(p + 2 * i, &h, 2);
        }
        return;
    case CF_ARGB32323232: memcpy(p, merged, 16); return;
    default: for (int i = 0; i < 4; ++i) v |= q(merged[i], 8) << (8 * i); break;
    }
    memcpy(p, &v, n);
}

/* Blend one shader output into colour buffer `cb` at (x, y). */
static void write_color(RLGDevice *d, unsigned cb, int x, int y, const float src_in[4])
{
    uint32_t pitchreg = rlg_reg_read32(d, RLG_RB3D_COLORPITCH0 + cb * 4u);
    uint32_t pitch = pitchreg & 0x3ffeu, f = (pitchreg >> 21) & 0xfu;
    uint64_t base = rlg_reg_read32(d, RLG_RB3D_COLOROFFSET0 + cb * 4u) & ~31u;
    uint32_t outfmt = rlg_reg_read32(d, RLG_US_OUT_FMT_0 + cb * 4u);
    uint32_t cblend = rlg_reg_read32(d, RLG_RB3D_CBLEND);
    uint32_t ablend = rlg_reg_read32(d, RLG_RB3D_ABLEND);
    uint32_t chmask = (rlg_reg_read32(d, RLG_RB3D_COLOR_CHANNEL_MASK) >> (4 * cb)) & 0xfu;
    unsigned n = cf_bpp(f), otype = outfmt & 0x1fu;
    bool fp = otype >= 16, fp16 = otype == 18;
    uint8_t px[16];
    float src[4], dst[4] = {0, 0, 0, 0}, res[4], ch[4], mem[4];
    unsigned sel[4];

    if (!pitch || chmask == 0) {
        return;
    }
    for (int c = 0; c < 4; ++c) {
        src[c] = fp ? src_in[c] : clamp01(src_in[c]);
        /* US_OUT_FMT: C<n>_SEL picks A(0)/R(1)/G(2)/B(3) for memory channel n. */
        unsigned s = (outfmt >> (8 + 2 * c)) & 3u;
        sel[c] = s == 0 ? 3u : s - 1u;
    }
    uint64_t addr = base + ((uint64_t)y * pitch + (uint64_t)x) * n;
    if (rlg_gpu_read(d, addr, px, n) != 0) {
        return;
    }
    unpack_cf(f, fp16, px, mem);
    for (int c = 0; c < 4; ++c) {
        dst[sel[c]] = mem[c];
    }

    if (cblend & 1u) {
        uint32_t c_src = (cblend >> 16) & 63u, c_dst = (cblend >> 24) & 63u, c_fcn = (cblend >> 12) & 7u;
        uint32_t a_src = c_src, a_dst = c_dst, a_fcn = c_fcn;
        uint32_t bc = rlg_reg_read32(d, RLG_RB3D_BLEND_COLOR);
        float cst[4] = { ((bc >> 16) & 255u) / 255.0f, ((bc >> 8) & 255u) / 255.0f,
                         (bc & 255u) / 255.0f, (bc >> 24) / 255.0f };
        if (cblend & 2u) {
            a_src = (ablend >> 16) & 63u; a_dst = (ablend >> 24) & 63u; a_fcn = (ablend >> 12) & 7u;
        }
        for (int c = 0; c < 4; ++c) {
            bool alpha = c == 3;
            float sf = blend_factor(alpha ? a_src : c_src, c, src, dst, cst);
            float df = blend_factor(alpha ? a_dst : c_dst, c, src, dst, cst);
            unsigned fcn = alpha ? a_fcn : c_fcn;
            if ((fcn & 7u) == 4 || (fcn & 7u) == 5) {
                res[c] = combine(fcn, src[c], dst[c]);
            } else {
                res[c] = combine(fcn, src[c] * sf, dst[c] * df);
            }
        }
    } else {
        memcpy(res, src, sizeof(res));
    }
    for (int c = 0; c < 4; ++c) {
        ch[c] = res[sel[c]];
    }
    pack_cf(f, fp16, ch, chmask, px);
    rlg_gpu_write(d, addr, px, n);
}

/* ---- rasterizer (RS) --------------------------------------------------------------- */

typedef struct {
    float tex[R3D_MAX_TEXCOORD][4];
    float color[R3D_MAX_COLORS][4];
} Interp;

void r3d_rs_eval(RLGDevice *d, const R3DDrawInfo *info, const float color[R3D_MAX_COLORS][4],
                 const float tex[R3D_MAX_TEXCOORD][4], float t[R3D_MAX_TEMPS][4], uint64_t *written)
{
    float comps[64];
    const float *cols[R3D_MAX_COLORS];
    unsigned nc = 0, ncol = 0;

    memset(t, 0, sizeof(float) * R3D_MAX_TEMPS * 4);
    if (written) {
        *written = 0;
    }
    for (unsigned i = 0; i < R3D_MAX_TEXCOORD; ++i) {
        for (unsigned k = 0; k < info->tex_count[i] && nc < 64; ++k) {
            comps[nc++] = tex[i][k];
        }
    }
    for (unsigned c = 0; c < R3D_MAX_COLORS; ++c) {
        if (info->fmt0 & (2u << c)) {
            cols[ncol++] = color[c];
        }
    }
    unsigned count = (rlg_reg_read32(d, RLG_RS_INST_COUNT) & 0xfu) + 1;
    for (unsigned i = 0; i < count && i < 8; ++i) {
        uint32_t inst = rlg_reg_read32(d, RLG_RS_INST_0 + i * 4u);
        if (inst & (1u << 3)) {                                 /* TEX_CN_WRITE */
            uint32_t ip = rlg_reg_read32(d, RLG_RS_IP_0 + (inst & 7u) * 4u);
            unsigned ptr = ip & 63u;
            float *dst = t[(inst >> 6) & 31u];
            if (written) {
                *written |= 1ull << ((inst >> 6) & 31u);
            }
            for (unsigned c = 0; c < 4; ++c) {
                unsigned sel = (ip >> (13 + 3 * c)) & 7u;
                if (sel < 4) {
                    dst[c] = ptr + sel < nc ? comps[ptr + sel] : 0.0f;
                } else {
                    dst[c] = sel == 5 ? 1.0f : 0.0f;
                }
            }
        }
        if (inst & (1u << 14)) {                                /* COL_CN_WRITE */
            uint32_t ip = rlg_reg_read32(d, RLG_RS_IP_0 + ((inst >> 11) & 7u) * 4u);
            unsigned ptr = (ip >> 6) & 7u, fmt = (ip >> 9) & 15u;
            static const float zero[4] = { 0, 0, 0, 0 };
            const float *c = ptr < ncol ? cols[ptr] : zero;
            float *dst = t[(inst >> 17) & 31u];
            if (written) {
                *written |= 1ull << ((inst >> 17) & 31u);
            }
            switch (fmt) {
            case 1: dst[0] = c[0]; dst[1] = c[1]; dst[2] = c[2]; dst[3] = 0.0f; break;
            case 2: dst[0] = c[0]; dst[1] = c[1]; dst[2] = c[2]; dst[3] = 1.0f; break;
            case 4: dst[0] = dst[1] = dst[2] = 0.0f; dst[3] = c[3]; break;
            case 5: dst[0] = dst[1] = dst[2] = dst[3] = 0.0f; break;
            case 6: dst[0] = dst[1] = dst[2] = 0.0f; dst[3] = 1.0f; break;
            case 8: dst[0] = dst[1] = dst[2] = 1.0f; dst[3] = c[3]; break;
            case 9: dst[0] = dst[1] = dst[2] = 1.0f; dst[3] = 0.0f; break;
            case 10: dst[0] = dst[1] = dst[2] = dst[3] = 1.0f; break;
            default: memcpy(dst, c, 16); break;
            }
        }
    }
}

/* CBZB clear (Mesa r300_blit.c): while ZB_BW_CNTL.CB_CLEAR is set, the ZB unit
 * writes the raw ZB_DEPTHCLEARVALUE (a packed colour) for every fragment, with
 * the depth buffer pointed at the lower half of the colour buffer. */
static void cbzb_write(RLGDevice *d, int x, int y)
{
    uint32_t pitch = rlg_reg_read32(d, RLG_ZB_DEPTHPITCH) & 0x3ffcu;
    uint32_t v = rlg_reg_read32(d, RLG_ZB_DEPTHCLEARVALUE);
    unsigned bpp = (rlg_reg_read32(d, RLG_ZB_FORMAT) & 0xfu) == 2 ? 4 : 2;
    uint64_t base = rlg_reg_read32(d, RLG_ZB_DEPTHOFFSET) & ~31u;
    if (pitch) {
        rlg_gpu_write(d, base + ((uint64_t)y * pitch + (uint64_t)x) * bpp, &v, bpp);
    }
}

static bool clip_rule_pass(RLGDevice *d, int x, int y)
{
    uint32_t rule = rlg_reg_read32(d, RLG_SC_CLIP_RULE) & 0xffffu;
    unsigned m = 0;
    if (rule == 0 || rule == 0xffffu) {
        return true;
    }
    for (unsigned k = 0; k < 4; ++k) {
        uint32_t tl = rlg_reg_read32(d, RLG_SC_CLIPRECT_TL_0 + k * 8u);
        uint32_t br = rlg_reg_read32(d, RLG_SC_CLIPRECT_BR_0 + k * 8u);
        int x0 = (int)(tl & 0x1fffu) - RLG_CLIPRECT_OFFSET, y0 = (int)((tl >> 13) & 0x1fffu) - RLG_CLIPRECT_OFFSET;
        int x1 = (int)(br & 0x1fffu) - RLG_CLIPRECT_OFFSET, y1 = (int)((br >> 13) & 0x1fffu) - RLG_CLIPRECT_OFFSET;
        if (x >= x0 && x <= x1 && y >= y0 && y <= y1) {
            m |= 1u << k;
        }
    }
    return (rule >> m) & 1u;
}

static void shade_quad(DrawCtx *dc, int qx, int qy, const bool cover[4], const float z[4],
                       const Interp in[4], bool front, unsigned nbuf)
{
    RLGDevice *d = dc->d;
    float temps[4][R3D_MAX_TEMPS][4], out[4][4][4], depth[4];
    bool dw[4] = { false, false, false, false }, alive[4];
    uint32_t afunc = rlg_reg_read32(d, RLG_FG_ALPHA_FUNC);

    memset(out, 0, sizeof(out));
    for (int l = 0; l < 4; ++l) {
        alive[l] = true;
        r3d_rs_eval(d, &dc->info, in[l].color, in[l].tex, temps[l], NULL);
        depth[l] = z[l];
    }
    r3d_fs_run_quad(d, dc->info.fp, temps, out, depth, dw, alive);
    for (int l = 0; l < 4; ++l) {
        int x = qx + (l & 1), y = qy + (l >> 1);
        if (!cover[l]) {
            continue;
        }
        if (!alive[l]) {
            d->r3d->stats.killed++;
            continue;
        }
        if ((afunc & (1u << 11)) &&
            !compare((afunc >> 8) & 7u, clamp01(out[l][0][3]), (float)(afunc & 0xffu) / 255.0f)) {
            continue;
        }
        if (!depth_stencil(d, x, y, depth[l], front)) {
            continue;
        }
        d->r3d->stats.fragments++;
        for (unsigned cb = 0; cb < nbuf; ++cb) {
            write_color(d, cb, x, y, out[l][nbuf > 1 && !(rlg_reg_read32(d, RLG_RB3D_CCTL) & (3u << 5)) ? cb : 0]);
        }
        if (rlg_reg_read32(d, RLG_ZB_BW_CNTL) & RLG_ZB_CB_CLEAR_WRITE_ONLY) {
            cbzb_write(d, x, y);
        }
    }
}

static unsigned color_buffers(RLGDevice *d)
{
    unsigned multi = ((rlg_reg_read32(d, RLG_RB3D_CCTL) >> 5) & 3u) + 1, n = 1;
    for (unsigned k = 1; k < 4; ++k) {
        if (rlg_reg_read32(d, RLG_RB3D_COLORPITCH0 + k * 4u) & 0x3ffeu) {
            n = k + 1;
        }
    }
    return multi > 1 ? multi : n;
}

static float tri_area(const R3DVertex *v0, const R3DVertex *v1, const R3DVertex *v2)
{
    return (v1->pos[0] - v0->pos[0]) * (v2->pos[1] - v0->pos[1]) -
           (v2->pos[0] - v0->pos[0]) * (v1->pos[1] - v0->pos[1]);
}

/* Scan-convert one screen-space triangle (after VTE and culling). */
static void scan_triangle(DrawCtx *dc, const R3DVertex *v0, const R3DVertex *v1,
                          const R3DVertex *v2, bool front)
{
    const R3DVertex *v[3] = { v0, v1, v2 };
    float area = tri_area(v0, v1, v2);
    unsigned nbuf = color_buffers(dc->d);

    if (area == 0.0f || !isfinite(area)) {
        return;
    }

    float minx = fminf(v0->pos[0], fminf(v1->pos[0], v2->pos[0]));
    float maxx = fmaxf(v0->pos[0], fmaxf(v1->pos[0], v2->pos[0]));
    float miny = fminf(v0->pos[1], fminf(v1->pos[1], v2->pos[1]));
    float maxy = fmaxf(v0->pos[1], fmaxf(v1->pos[1], v2->pos[1]));
    int x0 = (int)floorf(minx), x1 = (int)ceilf(maxx), y0 = (int)floorf(miny), y1 = (int)ceilf(maxy);
    x0 = x0 < dc->info.sc_x0 ? dc->info.sc_x0 : x0;
    y0 = y0 < dc->info.sc_y0 ? dc->info.sc_y0 : y0;
    x1 = x1 > dc->info.sc_x1 ? dc->info.sc_x1 : x1;
    y1 = y1 > dc->info.sc_y1 ? dc->info.sc_y1 : y1;
    if (x0 > x1 || y0 > y1) {
        return;
    }
    x0 &= ~1;
    y0 &= ~1;

    float inv_area = 1.0f / area;
    for (int qy = y0; qy <= y1; qy += 2) {
        for (int qx = x0; qx <= x1; qx += 2) {
            bool cover[4], any = false;
            float z[4];
            Interp in[4];
            for (int l = 0; l < 4; ++l) {
                int x = qx + (l & 1), y = qy + (l >> 1);
                float px = (float)x + 0.5f, py = (float)y + 0.5f, b[3];
                for (int e = 0; e < 3; ++e) {
                    const R3DVertex *a = v[(e + 1) % 3], *c = v[(e + 2) % 3];
                    float w = (c->pos[0] - a->pos[0]) * (py - a->pos[1]) -
                              (px - a->pos[0]) * (c->pos[1] - a->pos[1]);
                    b[e] = w * inv_area;
                }
                bool inside = true;
                for (int e = 0; e < 3; ++e) {
                    const R3DVertex *a = v[(e + 1) % 3], *c = v[(e + 2) % 3];
                    if (b[e] < 0.0f) {
                        inside = false;
                    } else if (b[e] == 0.0f) {
                        /* top-left rule on the shared edge */
                        float ex = (c->pos[0] - a->pos[0]) * (area > 0 ? 1.0f : -1.0f);
                        float ey = (c->pos[1] - a->pos[1]) * (area > 0 ? 1.0f : -1.0f);
                        if (!((ey == 0.0f && ex < 0.0f) || ey > 0.0f)) {
                            inside = false;
                        }
                    }
                }
                cover[l] = inside && x >= dc->info.sc_x0 && x <= dc->info.sc_x1 && y >= dc->info.sc_y0 &&
                           y <= dc->info.sc_y1 && clip_rule_pass(dc->d, x, y);
                any |= cover[l];

                /* Perspective-correct attributes; z is linear in screen space. */
                float wsum = b[0] * v0->rhw + b[1] * v1->rhw + b[2] * v2->rhw;
                float iw = wsum != 0.0f ? 1.0f / wsum : 0.0f;
                float pw[3] = { b[0] * v0->rhw * iw, b[1] * v1->rhw * iw, b[2] * v2->rhw * iw };
                z[l] = b[0] * v0->pos[2] + b[1] * v1->pos[2] + b[2] * v2->pos[2];
                for (int k = 0; k < 4; ++k) {
                    for (unsigned t = 0; t < R3D_MAX_TEXCOORD; ++t) {
                        in[l].tex[t][k] = pw[0] * v0->tex[t][k] + pw[1] * v1->tex[t][k] + pw[2] * v2->tex[t][k];
                    }
                    for (unsigned c = 0; c < R3D_MAX_COLORS; ++c) {
                        in[l].color[c][k] = pw[0] * v0->color[c][k] + pw[1] * v1->color[c][k] + pw[2] * v2->color[c][k];
                    }
                }
            }
            if (any) {
                shade_quad(dc, qx, qy, cover, z, in, front, nbuf);
            }
        }
    }
}

/* Culling, then either collect the triangle for the host backend or scan it. */
static void raster_triangle(DrawCtx *dc, const R3DVertex *v0, const R3DVertex *v1,
                            const R3DVertex *v2)
{
    float area = tri_area(v0, v1, v2);
    bool ccw, front;

    if (area == 0.0f || !isfinite(area)) {
        return;
    }
    /* In the y-down window, a positive area is clockwise on screen. */
    ccw = area < 0.0f;
    front = (dc->cull & RLG_FRONT_FACE_CW) ? !ccw : ccw;
    if (!dc->is_point && (((dc->cull & RLG_CULL_FRONT) && front) || ((dc->cull & RLG_CULL_BACK) && !front))) {
        return;
    }
    dc->d->r3d->stats.prims++;
    if (!dc->tris) {
        scan_triangle(dc, v0, v1, v2, front);
        return;
    }
    if (dc->ntris == dc->cap) {
        unsigned ncap = dc->cap * 2;
        R3DTri *nt = realloc(dc->tris, ncap * sizeof(*nt));
        if (!nt) {
            dc->collect_failed = true;
            return;
        }
        dc->tris = nt;
        dc->cap = ncap;
    }
    dc->tris[dc->ntris].v[0] = *v0;
    dc->tris[dc->ntris].v[1] = *v1;
    dc->tris[dc->ntris].v[2] = *v2;
    dc->tris[dc->ntris].front = front;
    dc->ntris++;
}

/* ---- clipping and assembly ----------------------------------------------------------- */

/* Signed distance to the clip planes used: w > eps, near, far. */
static float plane_dist(const DrawCtx *dc, const R3DVertex *v, int p)
{
    switch (p) {
    case 0: return v->pos[3] - 1e-6f;
    case 1: return dc->dx_clip ? v->pos[2] : v->pos[2] + v->pos[3];
    default: return v->pos[3] - v->pos[2];
    }
}

static void emit_triangle(DrawCtx *dc, const R3DVertex *a, const R3DVertex *b, const R3DVertex *c)
{
    R3DVertex poly[2][12];
    unsigned n = 3, cur = 0;

    poly[0][0] = *a;
    poly[0][1] = *b;
    poly[0][2] = *c;
    if (dc->clip) {
        for (int p = 0; p < 3 && n; ++p) {
            unsigned m = 0;
            for (unsigned i = 0; i < n; ++i) {
                const R3DVertex *s = &poly[cur][i], *e = &poly[cur][(i + 1) % n];
                float ds = plane_dist(dc, s, p), de = plane_dist(dc, e, p);
                if (ds >= 0.0f && m < 12) {
                    poly[cur ^ 1][m++] = *s;
                }
                if ((ds >= 0.0f) != (de >= 0.0f) && m < 12) {
                    lerp_vertex(s, e, ds / (ds - de), &poly[cur ^ 1][m++]);
                }
            }
            n = m;
            cur ^= 1;
        }
    }
    if (n < 3) {
        return;
    }
    for (unsigned i = 0; i < n; ++i) {
        r3d_vertex_vte(dc->d, &poly[cur][i]);
    }
    for (unsigned i = 1; i + 1 < n; ++i) {
        raster_triangle(dc, &poly[cur][0], &poly[cur][i], &poly[cur][i + 1]);
    }
}

/* Lines are drawn as one-pixel-wide quads. */
static void emit_line(DrawCtx *dc, const R3DVertex *a, const R3DVertex *b)
{
    R3DVertex p0 = *a, p1 = *b, q[4];
    if (dc->clip && (plane_dist(dc, &p0, 0) < 0 || plane_dist(dc, &p1, 0) < 0)) {
        return;
    }
    r3d_vertex_vte(dc->d, &p0);
    r3d_vertex_vte(dc->d, &p1);
    float dx = p1.pos[0] - p0.pos[0], dy = p1.pos[1] - p0.pos[1];
    float len = sqrtf(dx * dx + dy * dy);
    if (len == 0.0f) {
        return;
    }
    float nx = -dy / len * 0.5f, ny = dx / len * 0.5f;
    q[0] = p0; q[1] = p1; q[2] = p1; q[3] = p0;
    q[0].pos[0] += nx; q[0].pos[1] += ny;
    q[1].pos[0] += nx; q[1].pos[1] += ny;
    q[2].pos[0] -= nx; q[2].pos[1] -= ny;
    q[3].pos[0] -= nx; q[3].pos[1] -= ny;
    bool saved = dc->is_point;
    dc->is_point = true;                    /* never culled */
    raster_triangle(dc, &q[0], &q[1], &q[2]);
    raster_triangle(dc, &q[0], &q[2], &q[3]);
    dc->is_point = saved;
}

/* Points: GA_POINT_SIZE box, with optional texture-coordinate stuffing (GB_ENABLE). */
static void emit_point(DrawCtx *dc, const R3DVertex *a)
{
    RLGDevice *d = dc->d;
    R3DVertex c = *a, q[4];
    uint32_t ps = rlg_reg_read32(d, RLG_GA_POINT_SIZE), gb = rlg_reg_read32(d, RLG_GB_ENABLE);
    float w = (float)(ps >> 16) / 6.0f, h = (float)(ps & 0xffffu) / 6.0f;

    if (dc->clip && plane_dist(dc, &c, 0) < 0) {
        return;
    }
    r3d_vertex_vte(d, &c);
    if (w <= 0.0f || h <= 0.0f) {
        w = h = 1.0f;
    }
    /* corners: 0 bottom-left, 1 bottom-right, 2 top-right, 3 top-left (y down) */
    for (int i = 0; i < 4; ++i) {
        q[i] = c;
        q[i].pos[0] = c.pos[0] + ((i == 1 || i == 2) ? 0.5f : -0.5f) * w;
        q[i].pos[1] = c.pos[1] + ((i == 0 || i == 1) ? 0.5f : -0.5f) * h;
    }
    if (gb & RLG_GB_POINT_STUFF_ENABLE) {
        float s0 = r3d_regf(d, RLG_GA_POINT_S0), t0 = r3d_regf(d, RLG_GA_POINT_S0 + 4);
        float s1 = r3d_regf(d, RLG_GA_POINT_S0 + 8), t1 = r3d_regf(d, RLG_GA_POINT_S0 + 12);
        for (unsigned t = 0; t < R3D_MAX_TEXCOORD; ++t) {
            if (((gb >> (16 + 2 * t)) & 3u) == 0) {
                continue;
            }
            for (int i = 0; i < 4; ++i) {
                q[i].tex[t][0] = (i == 1 || i == 2) ? s1 : s0;
                q[i].tex[t][1] = (i == 0 || i == 1) ? t0 : t1;
                q[i].tex[t][2] = 0.0f;
                q[i].tex[t][3] = 1.0f;
            }
        }
    }
    bool saved = dc->is_point;
    dc->is_point = true;
    raster_triangle(dc, &q[0], &q[1], &q[2]);
    raster_triangle(dc, &q[0], &q[2], &q[3]);
    dc->is_point = saved;
}

static void apply_flat(DrawCtx *dc, R3DVertex *tri[3])
{
    uint32_t gcc = rlg_reg_read32(dc->d, RLG_GA_COLOR_CONTROL);
    unsigned prov = (gcc >> 16) & 3u;
    const R3DVertex *pv = tri[prov == 0 ? 0 : (prov == 1 ? 1 : 2)];
    for (unsigned c = 0; c < R3D_MAX_COLORS; ++c) {
        bool flat_rgb = ((gcc >> (4 * c)) & 3u) != 2, flat_a = ((gcc >> (4 * c + 2)) & 3u) != 2;
        for (int i = 0; i < 3; ++i) {
            if (flat_rgb) memcpy(tri[i]->color[c], pv->color[c], 12);
            if (flat_a) tri[i]->color[c][3] = pv->color[c][3];
        }
    }
}

static void triangle(DrawCtx *dc, const R3DVertex *a, const R3DVertex *b, const R3DVertex *c)
{
    R3DVertex t[3] = { *a, *b, *c };
    R3DVertex *tp[3] = { &t[0], &t[1], &t[2] };
    apply_flat(dc, tp);
    emit_triangle(dc, &t[0], &t[1], &t[2]);
}

void r3d_draw_prims(RLGDevice *d, uint32_t prim, R3DVertex *v, uint32_t n)
{
    DrawCtx dc = { .d = d };
    uint32_t vte = rlg_reg_read32(d, RLG_VAP_VTE_CNTL);
    uint32_t sc_tl = rlg_reg_read32(d, RLG_SC_SCISSORS_TL), sc_br = rlg_reg_read32(d, RLG_SC_SCISSORS_BR);

    dc.info.fp = r3d_fs_build(d);
    if (!dc.info.fp) {
        return;
    }
    if (rlg_host_is_active(d)) {
        dc.cap = 64;
        dc.tris = malloc(dc.cap * sizeof(*dc.tris));
    }
    dc.clip = !(rlg_reg_read32(d, RLG_VAP_CLIP_CNTL) & RLG_CLIP_DISABLE) && !(vte & RLG_VTX_XY_FMT);
    dc.dx_clip = (rlg_reg_read32(d, RLG_VAP_CNTL) & RLG_DX_CLIP_SPACE_DEF) != 0;
    dc.info.fmt0 = rlg_reg_read32(d, RLG_VAP_OUTPUT_VTX_FMT_0);
    dc.fmt1 = rlg_reg_read32(d, RLG_VAP_OUTPUT_VTX_FMT_1);
    dc.cull = rlg_reg_read32(d, RLG_SU_CULL_MODE);
    dc.is_point = false;
    uint32_t gb = rlg_reg_read32(d, RLG_GB_ENABLE);
    for (unsigned t = 0; t < R3D_MAX_TEXCOORD; ++t) {
        unsigned stuff = (gb >> (16 + 2 * t)) & 3u;
        dc.info.tex_count[t] = (dc.fmt1 >> (3 * t)) & 7u;
        if (prim == PRIM_POINTS && (gb & RLG_GB_POINT_STUFF_ENABLE) && stuff) {
            dc.info.tex_count[t] = stuff == 1 ? 2 : 3;
        }
    }
    dc.info.sc_x0 = 0; dc.info.sc_y0 = 0; dc.info.sc_x1 = 4095; dc.info.sc_y1 = 4095;
    if (sc_br) {
        int x0 = (int)(sc_tl & 0x1fffu) - RLG_CLIPRECT_OFFSET, y0 = (int)((sc_tl >> 13) & 0x1fffu) - RLG_CLIPRECT_OFFSET;
        int x1 = (int)(sc_br & 0x1fffu) - RLG_CLIPRECT_OFFSET, y1 = (int)((sc_br >> 13) & 0x1fffu) - RLG_CLIPRECT_OFFSET;
        dc.info.sc_x0 = x0 > 0 ? x0 : 0; dc.info.sc_y0 = y0 > 0 ? y0 : 0;
        dc.info.sc_x1 = x1 < 4095 ? x1 : 4095; dc.info.sc_y1 = y1 < 4095 ? y1 : 4095;
    }

    switch (prim) {
    case PRIM_POINTS:
        for (uint32_t i = 0; i < n; ++i) emit_point(&dc, &v[i]);
        break;
    case PRIM_LINES:
        for (uint32_t i = 0; i + 1 < n; i += 2) emit_line(&dc, &v[i], &v[i + 1]);
        break;
    case PRIM_LINE_STRIP: case PRIM_LINE_LOOP:
        for (uint32_t i = 0; i + 1 < n; ++i) emit_line(&dc, &v[i], &v[i + 1]);
        if (prim == PRIM_LINE_LOOP && n > 2) emit_line(&dc, &v[n - 1], &v[0]);
        break;
    case PRIM_TRIANGLES:
        for (uint32_t i = 0; i + 2 < n; i += 3) triangle(&dc, &v[i], &v[i + 1], &v[i + 2]);
        break;
    case PRIM_TRIANGLE_STRIP:
        for (uint32_t i = 0; i + 2 < n; ++i) {
            if (i & 1) triangle(&dc, &v[i + 1], &v[i], &v[i + 2]);
            else triangle(&dc, &v[i], &v[i + 1], &v[i + 2]);
        }
        break;
    case PRIM_TRIANGLE_FAN: case PRIM_POLYGON:
        for (uint32_t i = 1; i + 1 < n; ++i) triangle(&dc, &v[0], &v[i], &v[i + 1]);
        break;
    case PRIM_QUADS:
        for (uint32_t i = 0; i + 3 < n; i += 4) {
            triangle(&dc, &v[i], &v[i + 1], &v[i + 2]);
            triangle(&dc, &v[i], &v[i + 2], &v[i + 3]);
        }
        break;
    case PRIM_QUAD_STRIP:
        for (uint32_t i = 0; i + 3 < n; i += 2) {
            triangle(&dc, &v[i], &v[i + 1], &v[i + 3]);
            triangle(&dc, &v[i], &v[i + 3], &v[i + 2]);
        }
        break;
    default:
        d->r3d->stats.unsupported++;
        break;
    }
    if (dc.tris) {
        R3DTri *tris = dc.tris;
        dc.tris = NULL;                 /* scan_triangle below must not collect */
        if (dc.collect_failed || r3d_vk_draw(d, &dc.info, tris, dc.ntris) != 0) {
            d->r3d->stats.sw_fallbacks++;
            r3d_vk_flush(d);            /* the software path reads and writes VRAM */
            for (unsigned i = 0; i < dc.ntris; ++i) {
                scan_triangle(&dc, &tris[i].v[0], &tris[i].v[1], &tris[i].v[2], tris[i].front);
            }
        } else {
            d->r3d->stats.vk_draws++;
        }
        free(tris);
    }
    r3d_fs_free(dc.info.fp);
}
