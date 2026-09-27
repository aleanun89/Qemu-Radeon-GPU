#include "radeon_r3d.h"
#include <math.h>
#include <string.h>

/*
 * Texture units (TX). Register layout: Mesa r300_reg.h (TX_FILTER0/1,
 * TX_FORMAT0/1/2, TX_OFFSET). The mip-level layout reproduces Mesa
 * r300_texture_desc.c so that level N is read where the driver placed it.
 */

enum {
    F_X8 = 0x0, F_X16, F_Y4X4, F_Y8X8, F_Y16X16, F_Z3Y3X2, F_Z5Y6X5, F_Z6Y5X5,
    F_Z11Y11X10, F_Z10Y11X11, F_W4Z4Y4X4, F_W1Z5Y5X5, F_W8Z8Y8X8, F_W2Z10Y10X10,
    F_W16Z16Y16X16, F_DXT1, F_DXT3, F_DXT5, F_CXV8U8, F_AVYU444, F_VYUY422, F_YVYU422,
    F_16_MPEG, F_16_16_MPEG, F_16F, F_16F_16F, F_16F_16F_16F_16F, F_32F, F_32F_32F,
    F_32F_32F_32F_32F, F_W24_FP, F_ATI2N,
};

typedef struct {
    unsigned fmt, bpp;          /* bytes per texel, or per 4x4 block when compressed */
    bool compressed;
    unsigned w0, h0, d0, levels, min_level, target;  /* target: 0 2D, 1 3D, 2 cube */
    bool pitch_en, macro, micro;
    unsigned pitch_texels;
    uint32_t base;
} TexDesc;

static unsigned fmt_bpp(unsigned f, bool *compressed)
{
    *compressed = false;
    switch (f) {
    case F_X8: case F_Y4X4: case F_Z3Y3X2: return 1;
    case F_X16: case F_Y8X8: case F_Z5Y6X5: case F_Z6Y5X5: case F_W4Z4Y4X4: case F_W1Z5Y5X5:
    case F_16F: case F_16_MPEG: return 2;
    case F_W16Z16Y16X16: case F_16F_16F_16F_16F: case F_32F_32F: return 8;
    case F_32F_32F_32F_32F: return 16;
    case F_DXT1: *compressed = true; return 8;
    case F_DXT3: case F_DXT5: case F_ATI2N: *compressed = true; return 16;
    default: return 4;
    }
}

static unsigned ilog2(unsigned v) { unsigned r = 0; while (v >>= 1) r++; return r; }
static unsigned pot(unsigned v) { unsigned p = 1; while (p < v) p <<= 1; return p; }
static unsigned align_up(unsigned v, unsigned a) { return a ? (v + a - 1) / a * a : v; }
static unsigned minify(unsigned v, unsigned l) { v >>= l; return v ? v : 1; }

/* r300_get_pixel_alignment(): [macro][log2 bpp][micro][dim] */
static unsigned pixel_alignment(unsigned bpp, bool macro, unsigned micro, bool height)
{
    static const unsigned table[2][5][3][2] = {
        { {{32, 1}, {8, 4}, {0, 0}}, {{16, 1}, {8, 2}, {4, 4}}, {{8, 1}, {4, 2}, {0, 0}},
          {{4, 1}, {2, 2}, {0, 0}}, {{2, 1}, {0, 0}, {0, 0}} },
        { {{256, 8}, {64, 32}, {0, 0}}, {{128, 8}, {64, 16}, {32, 32}}, {{64, 8}, {32, 16}, {0, 0}},
          {{32, 8}, {16, 16}, {0, 0}}, {{16, 8}, {0, 0}, {0, 0}} },
    };
    unsigned t = table[macro][ilog2(bpp) > 4 ? 4 : ilog2(bpp)][micro > 2 ? 0 : micro][height];
    return t ? t : 1;
}

static void tex_desc(const RLGDevice *d, unsigned u, TexDesc *t)
{
    uint32_t f0 = rlg_reg_read32(d, RLG_TX_FORMAT0_0 + u * 4u);
    uint32_t f1 = rlg_reg_read32(d, RLG_TX_FORMAT1_0 + u * 4u);
    uint32_t f2 = rlg_reg_read32(d, RLG_TX_FORMAT2_0 + u * 4u);
    uint32_t fl = rlg_reg_read32(d, RLG_TX_FILTER0_0 + u * 4u);
    uint32_t off = rlg_reg_read32(d, RLG_TX_OFFSET_0 + u * 4u);

    t->fmt = f1 & 0x1fu;
    t->bpp = fmt_bpp(t->fmt, &t->compressed);
    t->w0 = (f0 & 0x7ffu) + 1;
    t->h0 = ((f0 >> 11) & 0x7ffu) + 1;
    t->d0 = 1u << ((f0 >> 22) & 0xfu);
    t->levels = (f0 >> 26) & 0xfu;
    t->min_level = (fl >> 17) & 0xfu;
    t->pitch_en = (f0 >> 31) & 1u;
    t->pitch_texels = (f2 & 0x1fffu) + 1;
    t->target = (f1 >> 25) & 3u;
    t->macro = (off >> 2) & 1u;
    t->micro = (off >> 3) & 3u;
    t->base = off & ~0x1fu;
}

/* Byte offset, stride and size of one level (Mesa r300_setup_miptree). */
static uint32_t level_layout(const RLGDevice *d, const TexDesc *t, unsigned level,
                             uint32_t *stride_out, uint32_t *layer_out)
{
    bool rs690 = d->chip->family == RLG_FAMILY_RS600 || d->chip->family == RLG_FAMILY_RS690;
    unsigned mw = pixel_alignment(t->bpp, true, t->micro, false);
    unsigned mh = pixel_alignment(t->bpp, true, t->micro, true);
    uint32_t offset = 0;

    for (unsigned i = 0; i <= level; ++i) {
        unsigned w = minify(t->w0, i), h = minify(t->h0, i), depth = minify(t->d0, i);
        bool macro = t->macro && w >= mw && h >= mh;
        uint32_t stride, rows, layer;

        if (t->target != 0 || t->levels != 0) {
            h = pot(h);
        }
        if (t->compressed) {
            stride = align_up(((w + 3) / 4) * t->bpp, rs690 ? 64 : 32);
            rows = (h + 3) / 4;
        } else {
            unsigned tw = pixel_alignment(t->bpp, macro, t->micro, false);
            if (!macro && rs690) {
                unsigned align = 64 / (t->bpp * pixel_alignment(t->bpp, false, t->micro, true));
                tw = tw < align ? align : tw;
            }
            if (t->pitch_en && i == 0) {
                stride = t->pitch_texels * t->bpp;
            } else {
                stride = align_up(w, tw) * t->bpp;
            }
            rows = align_up(h, pixel_alignment(t->bpp, macro, t->micro, true));
        }
        layer = stride * rows;
        if (i == level) {
            *stride_out = stride;
            *layer_out = layer;
            return offset;
        }
        offset += layer * (t->target == 2 ? 6u : (t->target == 1 ? depth : 1u));
    }
    return offset;
}

/* ---- texel decode ----------------------------------------------------------- */

static float unorm(uint32_t v, unsigned bits) { return (float)v / (float)((1u << bits) - 1u); }

static float snorm(uint32_t v, unsigned bits)
{
    int32_t s = (int32_t)(v << (32 - bits)) >> (32 - bits);
    float f = (float)s / (float)((1u << (bits - 1)) - 1u);
    return f < -1.0f ? -1.0f : f;
}

static void rgb565(uint16_t c, float o[3])
{
    o[0] = unorm(c & 31u, 5); o[1] = unorm((c >> 5) & 63u, 6); o[2] = unorm(c >> 11, 5);
}

/* DXT: returns X/Y/Z/W in the same layout as W8Z8Y8X8 (X = blue). */
static void dxt_texel(unsigned fmt, const uint8_t *blk, unsigned px, unsigned py, float xyzw[4])
{
    const uint8_t *cb = fmt == F_DXT1 ? blk : blk + 8;
    uint16_t c0 = (uint16_t)(cb[0] | cb[1] << 8), c1 = (uint16_t)(cb[2] | cb[3] << 8);
    uint32_t bits = (uint32_t)cb[4] | cb[5] << 8 | cb[6] << 16 | (uint32_t)cb[7] << 24;
    unsigned code = (bits >> (2 * (py * 4 + px))) & 3u;
    float a[3], b[3], alpha = 1.0f;

    rgb565(c0, a);
    rgb565(c1, b);
    for (int i = 0; i < 3; ++i) {
        float v;
        if (code == 0) v = a[i];
        else if (code == 1) v = b[i];
        else if (fmt != F_DXT1 || c0 > c1) v = code == 2 ? (2 * a[i] + b[i]) / 3 : (a[i] + 2 * b[i]) / 3;
        else v = code == 2 ? (a[i] + b[i]) / 2 : 0.0f;
        xyzw[i] = v;
    }
    if (fmt == F_DXT1) {
        alpha = (c0 <= c1 && code == 3) ? 0.0f : 1.0f;
    } else if (fmt == F_DXT3) {
        unsigned n = py * 4 + px;
        alpha = unorm((blk[n / 2] >> (4 * (n & 1))) & 15u, 4);
    } else {
        unsigned a0 = blk[0], a1 = blk[1], n = py * 4 + px;
        uint64_t ab = 0;
        for (int i = 0; i < 6; ++i) {
            ab |= (uint64_t)blk[2 + i] << (8 * i);
        }
        unsigned ac = (ab >> (3 * n)) & 7u;
        float fa0 = a0 / 255.0f, fa1 = a1 / 255.0f;
        if (ac == 0) alpha = fa0;
        else if (ac == 1) alpha = fa1;
        else if (a0 > a1) alpha = ((8 - ac) * fa0 + (ac - 1) * fa1) / 7.0f;
        else if (ac < 6) alpha = ((6 - ac) * fa0 + (ac - 1) * fa1) / 5.0f;
        else alpha = ac == 6 ? 0.0f : 1.0f;
    }
    xyzw[3] = alpha;
}

static void decode(unsigned fmt, uint32_t sgn, const uint8_t *p, float v[4])
{
    uint32_t w32 = 0;
    uint64_t w64 = 0;
    memcpy(&w32, p, 4);
    memcpy(&w64, p, 8);
    v[0] = v[1] = v[2] = 0.0f;
    v[3] = 1.0f;
#define CH(i, val, bits) v[i] = (sgn & (8u >> (i))) ? snorm((val), (bits)) : unorm((val), (bits))
    switch (fmt) {
    case F_X8: CH(0, p[0], 8); break;
    case F_X16: CH(0, (uint32_t)(p[0] | p[1] << 8), 16); break;
    case F_Y4X4: CH(0, p[0] & 15u, 4); CH(1, p[0] >> 4, 4); break;
    case F_Y8X8: CH(0, p[0], 8); CH(1, p[1], 8); break;
    case F_Y16X16: CH(0, w32 & 0xffffu, 16); CH(1, w32 >> 16, 16); break;
    case F_Z3Y3X2: CH(0, p[0] & 3u, 2); CH(1, (p[0] >> 2) & 7u, 3); CH(2, p[0] >> 5, 3); break;
    case F_Z5Y6X5: CH(0, w32 & 31u, 5); CH(1, (w32 >> 5) & 63u, 6); CH(2, (w32 >> 11) & 31u, 5); break;
    case F_Z6Y5X5: CH(0, w32 & 31u, 5); CH(1, (w32 >> 5) & 31u, 5); CH(2, (w32 >> 10) & 63u, 6); break;
    case F_Z11Y11X10: CH(0, w32 & 1023u, 10); CH(1, (w32 >> 10) & 2047u, 11); CH(2, w32 >> 21, 11); break;
    case F_Z10Y11X11: CH(0, w32 & 2047u, 11); CH(1, (w32 >> 11) & 2047u, 11); CH(2, w32 >> 22, 10); break;
    case F_W4Z4Y4X4:
        CH(0, w32 & 15u, 4); CH(1, (w32 >> 4) & 15u, 4); CH(2, (w32 >> 8) & 15u, 4); CH(3, (w32 >> 12) & 15u, 4);
        break;
    case F_W1Z5Y5X5:
        CH(0, w32 & 31u, 5); CH(1, (w32 >> 5) & 31u, 5); CH(2, (w32 >> 10) & 31u, 5); CH(3, (w32 >> 15) & 1u, 1);
        break;
    case F_W8Z8Y8X8: CH(0, p[0], 8); CH(1, p[1], 8); CH(2, p[2], 8); CH(3, p[3], 8); break;
    case F_W2Z10Y10X10:
        CH(0, w32 & 1023u, 10); CH(1, (w32 >> 10) & 1023u, 10); CH(2, (w32 >> 20) & 1023u, 10);
        CH(3, w32 >> 30, 2);
        break;
    case F_W16Z16Y16X16:
        for (int i = 0; i < 4; ++i) {
            CH(i, (uint32_t)((w64 >> (16 * i)) & 0xffffu), 16);
        }
        break;
    case F_16F: case F_16_MPEG: v[0] = r3d_half((uint16_t)(p[0] | p[1] << 8)); break;
    case F_16F_16F: v[0] = r3d_half((uint16_t)w32); v[1] = r3d_half((uint16_t)(w32 >> 16)); break;
    case F_16F_16F_16F_16F:
        for (int i = 0; i < 4; ++i) {
            v[i] = r3d_half((uint16_t)(w64 >> (16 * i)));
        }
        break;
    case F_32F: memcpy(&v[0], p, 4); break;
    case F_32F_32F: memcpy(v, p, 8); break;
    case F_32F_32F_32F_32F: memcpy(v, p, 16); break;
    default: break;
    }
#undef CH
}

static float srgb_to_linear(float c)
{
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

/* Wrap an integer texel coordinate; returns -1 for "use border". */
static int wrap(int i, int size, unsigned mode)
{
    switch (mode) {
    case 0: {                               /* REPEAT */
        int m = i % size;
        return m < 0 ? m + size : m;
    }
    case 1: {                               /* MIRRORED */
        int p = size * 2, m = i % p;
        m = m < 0 ? m + p : m;
        return m < size ? m : p - 1 - m;
    }
    case 3: case 5:                         /* MIRROR_ONCE(_TO_EDGE) */
        i = i < 0 ? -1 - i : i;
        return i >= size ? size - 1 : i;
    case 6: case 7:                         /* CLAMP_TO_BORDER, MIRROR_ONCE_TO_BORDER */
        if (mode == 7) {
            i = i < 0 ? -1 - i : i;
        }
        return (i < 0 || i >= size) ? -1 : i;
    default:                                /* CLAMP_TO_EDGE, CLAMP */
        return i < 0 ? 0 : (i >= size ? size - 1 : i);
    }
}

typedef struct {
    const TexDesc *t;
    uint32_t level_base, stride, layer;
    unsigned w, h;
    uint32_t fmt1, border;
    unsigned wrap_s, wrap_t;
} LevelCtx;

static void fetch_texel(RLGDevice *d, const LevelCtx *L, int x, int y, unsigned slice, float o[4])
{
    const TexDesc *t = L->t;
    int xx = wrap(x, (int)L->w, L->wrap_s), yy = wrap(y, (int)L->h, L->wrap_t);
    uint8_t buf[16];
    float raw[4];

    if (xx < 0 || yy < 0) {
        uint32_t b = L->border;
        raw[0] = (b & 0xffu) / 255.0f; raw[1] = ((b >> 8) & 0xffu) / 255.0f;
        raw[2] = ((b >> 16) & 0xffu) / 255.0f; raw[3] = (b >> 24) / 255.0f;
    } else if (t->compressed) {
        uint64_t a = (uint64_t)t->base + L->level_base + (uint64_t)slice * L->layer +
                     (uint64_t)(yy / 4) * L->stride + (uint64_t)(xx / 4) * t->bpp;
        if (rlg_gpu_read(d, a, buf, t->bpp) != 0) {
            memset(buf, 0, sizeof(buf));
        }
        dxt_texel(t->fmt, buf, (unsigned)xx & 3u, (unsigned)yy & 3u, raw);
    } else {
        uint64_t a = (uint64_t)t->base + L->level_base + (uint64_t)slice * L->layer +
                     (uint64_t)yy * L->stride + (uint64_t)xx * t->bpp;
        memset(buf, 0, sizeof(buf));
        if (rlg_gpu_read(d, a, buf, t->bpp) != 0) {
            memset(buf, 0, sizeof(buf));
        }
        decode(t->fmt, (L->fmt1 >> 5) & 0xfu, buf, raw);
    }
    /* TX_FORMAT1 swizzle: R 14:12, G 17:15, B 20:18, A 11:9 select X/Y/Z/W/0/1. */
    static const unsigned shift[4] = { 12, 15, 18, 9 };
    for (int c = 0; c < 4; ++c) {
        unsigned sel = (L->fmt1 >> shift[c]) & 7u;
        float v = sel < 4 ? raw[sel] : (sel == 5 ? 1.0f : 0.0f);
        if (sel >= 6) {
            v = 2.0f * raw[sel == 6 ? 2 : 3];
            v = v > 1.0f ? 0.0f : v;
        }
        o[c] = v;
    }
    if (L->fmt1 & (1u << 21)) {                /* TX_FORMAT_GAMMA: sRGB */
        for (int c = 0; c < 3; ++c) {
            o[c] = srgb_to_linear(o[c]);
        }
    }
}

static void sample_level(RLGDevice *d, const TexDesc *t, unsigned level, unsigned slice,
                         float s, float tc, bool linear, uint32_t fmt1, uint32_t filter0,
                         uint32_t border, float o[4])
{
    LevelCtx L = { .t = t, .fmt1 = fmt1, .border = border,
                   .wrap_s = filter0 & 7u, .wrap_t = (filter0 >> 3) & 7u };
    L.level_base = level_layout(d, t, level, &L.stride, &L.layer);
    L.w = minify(t->w0, level);
    L.h = minify(t->h0, level);

    float u = s * (float)L.w, v = tc * (float)L.h;
    if (!linear) {
        fetch_texel(d, &L, (int)floorf(u), (int)floorf(v), slice, o);
        return;
    }
    float fu = u - 0.5f, fv = v - 0.5f;
    int x0 = (int)floorf(fu), y0 = (int)floorf(fv);
    float ax = fu - (float)x0, ay = fv - (float)y0, t00[4], t10[4], t01[4], t11[4];
    fetch_texel(d, &L, x0, y0, slice, t00);
    fetch_texel(d, &L, x0 + 1, y0, slice, t10);
    fetch_texel(d, &L, x0, y0 + 1, slice, t01);
    fetch_texel(d, &L, x0 + 1, y0 + 1, slice, t11);
    for (int c = 0; c < 4; ++c) {
        float top = t00[c] + (t10[c] - t00[c]) * ax, bot = t01[c] + (t11[c] - t01[c]) * ax;
        o[c] = top + (bot - top) * ay;
    }
}

/* Cube face selection (+X, -X, +Y, -Y, +Z, -Z) as in GL/D3D. */
static unsigned cube_face(const float c[3], float *s, float *t)
{
    float ax = fabsf(c[0]), ay = fabsf(c[1]), az = fabsf(c[2]), ma, sc, tc;
    unsigned face;
    if (ax >= ay && ax >= az) {
        face = c[0] >= 0 ? 0 : 1; ma = ax; sc = c[0] >= 0 ? -c[2] : c[2]; tc = -c[1];
    } else if (ay >= az) {
        face = c[1] >= 0 ? 2 : 3; ma = ay; sc = c[0]; tc = c[1] >= 0 ? c[2] : -c[2];
    } else {
        face = c[2] >= 0 ? 4 : 5; ma = az; sc = c[2] >= 0 ? c[0] : -c[0]; tc = -c[1];
    }
    ma = ma != 0.0f ? ma : 1.0f;
    *s = 0.5f * (sc / ma + 1.0f);
    *t = 0.5f * (tc / ma + 1.0f);
    return face;
}

void r3d_tex_sample(RLGDevice *d, unsigned unit, const float coord[4], float lod,
                    float out[4])
{
    uint32_t filter0, filter1, fmt1, border;
    TexDesc t;
    float s = coord[0], tc = coord[1];
    unsigned slice = 0;

    if (unit > 15 || !(rlg_reg_read32(d, RLG_TX_ENABLE) & (1u << unit))) {
        out[0] = out[1] = out[2] = 0.0f;
        out[3] = 1.0f;
        return;
    }
    filter0 = rlg_reg_read32(d, RLG_TX_FILTER0_0 + unit * 4u);
    filter1 = rlg_reg_read32(d, RLG_TX_FILTER1_0 + unit * 4u);
    fmt1 = rlg_reg_read32(d, RLG_TX_FORMAT1_0 + unit * 4u);
    border = rlg_reg_read32(d, RLG_TX_BORDER_COLOR_0 + unit * 4u);
    tex_desc(d, unit, &t);

    if (t.target == 2) {
        slice = cube_face(coord, &s, &tc);
    } else if (t.target == 1) {
        float r = coord[2] * (float)t.d0;
        int z = (int)floorf(r);
        slice = (unsigned)(z < 0 ? 0 : (z >= (int)t.d0 ? (int)t.d0 - 1 : z));
    }

    /* LOD: caller's implicit LOD (already includes TXB bias) + sampler bias (1/32 units). */
    int bias = (int)((filter1 >> 3) & 0x3ffu);
    bias = bias & 0x200 ? bias - 0x400 : bias;
    lod += (float)bias / 32.0f;

    bool mag = lod <= 0.0f;
    unsigned img_filter = mag ? (filter0 >> 9) & 3u : (filter0 >> 11) & 3u;
    unsigned mip = (filter0 >> 13) & 3u;
    bool linear = img_filter == 2 || img_filter == 3;
    float maxl = (float)t.levels, minl = (float)t.min_level;

    if (mip == 0 || mag || t.levels == 0) {
        sample_level(d, &t, t.min_level, slice, s, tc, linear, fmt1, filter0, border, out);
        return;
    }
    float l = fminf(fmaxf(lod, minl), maxl);
    if (mip == 1) {
        unsigned lv = (unsigned)floorf(l + 0.5f);
        sample_level(d, &t, lv, slice, s, tc, linear, fmt1, filter0, border, out);
        return;
    }
    unsigned l0 = (unsigned)floorf(l), l1 = l0 + 1 > t.levels ? t.levels : l0 + 1;
    float f = l - (float)l0, a[4], b[4];
    sample_level(d, &t, l0, slice, s, tc, linear, fmt1, filter0, border, a);
    sample_level(d, &t, l1, slice, s, tc, linear, fmt1, filter0, border, b);
    for (int c = 0; c < 4; ++c) {
        out[c] = a[c] + (b[c] - a[c]) * f;
    }
}

/* Implicit LOD from screen-space derivatives of the (projected) coordinates. */
float r3d_tex_lod(RLGDevice *d, unsigned unit, const float dx[2], const float dy[2])
{
    TexDesc t;
    if (unit > 15) {
        return 0.0f;
    }
    tex_desc(d, unit, &t);
    float ux = dx[0] * (float)t.w0, vx = dx[1] * (float)t.h0;
    float uy = dy[0] * (float)t.w0, vy = dy[1] * (float)t.h0;
    float rho = fmaxf(sqrtf(ux * ux + vx * vx), sqrtf(uy * uy + vy * vy));
    return rho > 0.0f ? log2f(rho) : -16.0f;
}
