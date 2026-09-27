/*
 * 3D engine tests. Commands are built the way Mesa r300g emits them
 * (r300_emit.c, r300_render.c, r300_fs.c) and shaders are hand-encoded with
 * the same field layouts the r300 compiler produces.
 */
#include "radeon_r3d.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define FB      0x00000000u     /* MC address of VRAM (default FB location) */
#define RING    0x00100000u
#define CBUF    0x00200000u
#define ZBUF    0x00400000u
#define TEX     0x00600000u
#define VBO     0x00700000u
#define W       64
#define H       64

static RLGDevice *dev;
static uint32_t wptr;

static void w(uint32_t a, uint32_t v) { rlg_mmio_write(dev, a, v, 4); }
static void wf(uint32_t a, float f) { uint32_t v; memcpy(&v, &f, 4); w(a, v); }
static uint32_t fbits(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }
static uint32_t px(int x, int y) { uint32_t v; memcpy(&v, dev->vram + CBUF + (y * W + x) * 4, 4); return v; }

/* ---- ring ------------------------------------------------------------------ */
static void ring(uint32_t v) { memcpy(dev->vram + RING + (wptr++ & 1023) * 4, &v, 4); }
static void kick(void) { w(RLG_CP_RB_WPTR, wptr & 1023); }
static void pkt3(uint32_t op, const uint32_t *p, uint32_t n)
{
    ring(RLG_PM4_PACKET3(op, n));
    for (uint32_t i = 0; i < n; ++i) ring(p[i]);
    kick();
}

/* ---- US (fragment program) encoders: Mesa r300_fragprog_emit.c -------------- */
enum { ARGC_SRC0C_XYZ = 0, ARGC_SRC1C_XYZ = 4, ARGC_SRC2C_XYZ = 8, ARGC_SRC0A = 12, ARGC_ZERO = 20,
       ARGC_ONE = 21 };
enum { ARGA_SRC0A = 9, ARGA_SRC1A = 10, ARGA_ZERO = 16, ARGA_ONE = 17 };
#define OUTC_MAD (0u << 23)
#define OUTC_MAX (5u << 23)
#define OUTA_MAD (0u << 23)
#define OUTA_MAX (3u << 23)
#define SRC_CONST(i) ((i) | 0x20u)

typedef struct { uint32_t rgb_addr, alpha_addr, rgb_inst, alpha_inst; } Alu;

/* out[target].rgba = MAX(src, src): Mesa's MOV. */
static Alu alu_mov_out(uint32_t src)
{
    Alu a = {0};
    a.rgb_addr = src | (7u << 26);                   /* output XYZ, target 0 */
    a.alpha_addr = src | (1u << 24);                 /* output alpha */
    a.rgb_inst = ARGC_SRC0C_XYZ | (ARGC_SRC0C_XYZ << 7) | (ARGC_ZERO << 14) | OUTC_MAX;
    a.alpha_inst = ARGA_SRC0A | (ARGA_SRC0A << 7) | (ARGA_ZERO << 14) | OUTA_MAX;
    return a;
}

static void load_fs(const Alu *alu, unsigned nalu, const uint32_t *tex, unsigned ntex)
{
    w(RLG_US_CONFIG, ntex ? (1u << 3) : 0);          /* one node, FIRST_NODE_HAS_TEX */
    w(RLG_US_PIXSIZE, 4);
    w(RLG_US_CODE_OFFSET, ((nalu - 1) << 6) | ((ntex ? ntex - 1 : 0) << 18));
    w(RLG_R400_US_CODE_EXT, 0);
    for (int i = 0; i < 3; ++i) w(RLG_US_CODE_ADDR_0 + i * 4, 0);
    w(RLG_US_CODE_ADDR_0 + 12, ((nalu - 1) << 6) | ((ntex ? ntex - 1 : 0) << 17));
    for (unsigned i = 0; i < nalu; ++i) {
        w(RLG_US_ALU_RGB_INST_0 + i * 4, alu[i].rgb_inst);
        w(RLG_US_ALU_RGB_ADDR_0 + i * 4, alu[i].rgb_addr);
        w(RLG_US_ALU_ALPHA_INST_0 + i * 4, alu[i].alpha_inst);
        w(RLG_US_ALU_ALPHA_ADDR_0 + i * 4, alu[i].alpha_addr);
    }
    for (unsigned i = 0; i < ntex; ++i) w(RLG_US_TEX_INST_0 + i * 4, tex[i]);
}

/* Inverse check of Mesa pack_float24(). */
static uint32_t pack_float24(float f)
{
    union { float fl; uint32_t u; } u = { f };
    int e;
    float m;
    uint32_t r = 0;
    if (f == 0.0f) return 0;
    m = frexpf(f, &e);
    if (m < 0) { r |= 1u << 23; }
    r |= (uint32_t)(e + 62) << 16;
    r |= (u.u & 0x7fffffu) >> 7;
    return r;
}

/* ---- PVS encoders: Mesa r300_reg.h PVS_OP_DST_OPERAND / PVS_SRC_OPERAND ------ */
static uint32_t pvs_dst(unsigned op, unsigned idx, unsigned mask, unsigned cls)
{
    return (op & 0x3fu) | ((cls & 0xfu) << 8) | ((idx & 0x7fu) << 13) | ((mask & 0xfu) << 20);
}
static uint32_t pvs_src(unsigned idx, unsigned sx, unsigned sy, unsigned sz, unsigned sw, unsigned cls)
{
    return ((idx & 0xffu) << 5) | (sx << 13) | (sy << 16) | (sz << 19) | (sw << 22) | (cls & 3u);
}
#define PVS_TEMP 0
#define PVS_IN 1
#define PVS_CONST 2
#define PVS_OUT 2
#define XYZW 0, 1, 2, 3
#define ZERO4 4, 4, 4, 4

static void pvs_upload(uint32_t at, const uint32_t *d, unsigned n)
{
    w(RLG_VAP_PVS_VECTOR_INDX_REG, at);
    for (unsigned i = 0; i < n; ++i) w(RLG_VAP_PVS_UPLOAD_DATA, d[i]);
}

/* ---- common state -------------------------------------------------------------- */
static void setup(RLGProfile prof)
{
    RLGConfig c = { .profile = prof, .vram_size = 16u << 20 };
    if (dev) rlg_destroy(dev);
    dev = rlg_create(&c);
    wptr = 0;
    w(RLG_CP_RB_CNTL, 9 | RLG_RB_NO_UPDATE);        /* 1024-dword ring */
    w(RLG_CP_RB_BASE, FB + RING);
    w(RLG_CP_CSQ_CNTL, 4u << RLG_CSQ_MODE_SHIFT);
    w(RLG_RB3D_COLOROFFSET0, FB + CBUF);
    w(RLG_RB3D_COLORPITCH0, W | (6u << 21));        /* ARGB8888 */
    w(RLG_US_OUT_FMT_0, 0 | (3u << 8) | (2u << 10) | (1u << 12) | (0u << 14)); /* C4_8, BGRA */
    w(RLG_SC_CLIP_RULE, 0xffff);
    w(RLG_RB3D_COLOR_CHANNEL_MASK, 0xf);
    w(RLG_GA_COLOR_CONTROL, 0x3aaaa);               /* r300_emit_draw_init: smooth, provoking last */
}

/* SW TCL (TCL bypass): position (FLOAT_4 -> slot 0) + colour 0 (FLOAT_4 -> slot 2). */
static void setup_bypass_pos_color(void)
{
    w(RLG_VAP_CNTL_STATUS, RLG_VAP_TCL_BYPASS);
    w(RLG_VAP_VTE_CNTL, RLG_VTX_XY_FMT | RLG_VTX_Z_FMT);
    w(RLG_VAP_OUTPUT_VTX_FMT_0, 1u | 2u);           /* POS | COLOR_0 */
    w(RLG_VAP_OUTPUT_VTX_FMT_1, 0);
    w(RLG_VAP_PROG_STREAM_CNTL_0, (3u | (0u << 8)) | ((3u | (2u << 8) | (1u << 13)) << 16));
    w(RLG_VAP_PROG_STREAM_CNTL_EXT_0, (0xf688u << 0) | (0xf688u << 16) );
    w(RLG_VAP_VTX_SIZE, 8);
    w(RLG_RS_COUNT, 1u << 7);
    w(RLG_RS_INST_COUNT, 0);
    w(RLG_RS_IP_0, (0u << 6) | (0u << 9));          /* COL_PTR 0, RGBA */
    w(RLG_RS_INST_0, (0u << 11) | (1u << 14) | (0u << 17)); /* COL_ID 0 -> temp 0 */
    Alu mov = alu_mov_out(0);
    load_fs(&mov, 1, NULL, 0);
}

static void draw_immd(uint32_t prim, const float *verts, unsigned nverts, unsigned vsize)
{
    uint32_t buf[256];
    buf[0] = (3u << 4) | (nverts << 16) | prim;     /* WALK_VERTEX_EMBEDDED */
    for (unsigned i = 0; i < nverts * vsize; ++i) buf[1 + i] = fbits(verts[i]);
    pkt3(RLG_PM4_3D_DRAW_IMMD_2, buf, 1 + nverts * vsize);
}

/* ---- tests ------------------------------------------------------------------------ */

static void test_float24(void)
{
    const float vals[] = { 1.0f, 0.5f, -2.0f, 0.25f, 3.75f, 100.0f };
    for (unsigned i = 0; i < sizeof(vals) / sizeof(vals[0]); ++i) {
        CHECK(fabsf(r3d_float24(pack_float24(vals[i])) - vals[i]) < 1e-3f * fabsf(vals[i]));
    }
    CHECK(r3d_float24(0) == 0.0f);
    CHECK(r3d_half(0x3c00) == 1.0f && r3d_half(0xc000) == -2.0f);
}

/* Gouraud-free flat triangle through the TCL bypass path (IGP / SW TCL). */
static void test_bypass_triangle(void)
{
    setup(RLG_PROFILE_X1250_AMD);
    setup_bypass_pos_color();
    const float v[] = {
        4, 4, 0.5f, 1,   1, 0, 0, 1,
        60, 4, 0.5f, 1,  1, 0, 0, 1,
        4, 60, 0.5f, 1,  1, 0, 0, 1,
    };
    draw_immd(4, v, 3, 8);                          /* PRIM_TRIANGLES */
    CHECK(px(10, 10) == 0xffff0000u);               /* A=ff R=ff G=0 B=0 in BGRA memory */
    CHECK(px(50, 50) == 0);                         /* outside (below the diagonal) */
    CHECK(px(2, 2) == 0);
    CHECK(dev->r3d->stats.draws == 1 && dev->r3d->stats.prims == 1);
}

/* Colour interpolation and exact coverage of a full-screen quad. */
static void test_gouraud_quad(void)
{
    setup(RLG_PROFILE_X1250_AMD);
    setup_bypass_pos_color();
    const float v[] = {
        0, 0, 0, 1,    0, 0, 0, 1,
        64, 0, 0, 1,   1, 0, 0, 1,
        64, 64, 0, 1,  1, 1, 0, 1,
        0, 64, 0, 1,   0, 1, 0, 1,
    };
    draw_immd(13, v, 4, 8);                         /* PRIM_QUADS */
    unsigned covered = 0;
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) covered += (px(x, y) >> 24) == 0xff;
    CHECK(covered == W * H);                        /* no gaps, no overdraw holes */
    uint32_t c = px(63, 32);                        /* right edge: R ~ 1, G ~ 0.5 */
    CHECK(((c >> 16) & 0xff) > 0xf0 && ((c >> 8) & 0xff) > 0x70 && ((c >> 8) & 0xff) < 0x90);
    c = px(0, 0);
    CHECK(((c >> 16) & 0xff) < 0x08 && ((c >> 8) & 0xff) < 0x08);
}

/* HW TCL: vertex arrays + PVS program + viewport + clipping (discrete chips). */
static void test_tcl_vbo(void)
{
    setup(RLG_PROFILE_X700PRO);
    w(RLG_VAP_CNTL_STATUS, 0);                      /* TCL enabled */
    /* Viewport: NDC [-1,1] -> [0,64), y flipped like a window framebuffer. */
    wf(RLG_SE_VPORT_XSCALE, 32); wf(RLG_SE_VPORT_XSCALE + 4, 32);
    wf(RLG_SE_VPORT_XSCALE + 8, -32); wf(RLG_SE_VPORT_XSCALE + 12, 32);
    wf(RLG_SE_VPORT_XSCALE + 16, 0.5f); wf(RLG_SE_VPORT_XSCALE + 20, 0.5f);
    w(RLG_VAP_VTE_CNTL, 0x3f | (1u << 10));         /* all scale/offset + W0_FMT */
    w(RLG_VAP_OUTPUT_VTX_FMT_0, 1u | 2u);
    w(RLG_VAP_OUTPUT_VTX_FMT_1, 0);
    /* Streams: pos FLOAT_3 -> input 0, colour BYTE normalized -> input 1. */
    w(RLG_VAP_PROG_STREAM_CNTL_0, (2u | (0u << 8)) | ((4u | (1u << 8) | (1u << 13) | (1u << 15)) << 16));
    w(RLG_VAP_PROG_STREAM_CNTL_EXT_0, 0xf688u | (0xf688u << 16));
    /* VS: out0 = MUL(in0, c0) [scales x by 0.5]; out1 = in1. */
    const uint32_t vs[] = {
        pvs_dst(2 /*VE_MULTIPLY*/, 0, 0xf, PVS_OUT), pvs_src(0, XYZW, PVS_IN), pvs_src(0, XYZW, PVS_CONST), pvs_src(0, ZERO4, PVS_IN),
        pvs_dst(3 /*VE_ADD*/, 1, 0xf, PVS_OUT), pvs_src(1, XYZW, PVS_IN), pvs_src(1, ZERO4, PVS_IN), pvs_src(1, ZERO4, PVS_IN),
    };
    w(RLG_VAP_PVS_CODE_CNTL_0, (0u) | (0u << 10) | (1u << 20));
    pvs_upload(0, vs, 8);
    w(RLG_VAP_PVS_CONST_CNTL, 0);
    const uint32_t c0[] = { fbits(0.5f), fbits(1), fbits(1), fbits(1) };
    pvs_upload(R3D_PVS_CONST_START, c0, 4);
    w(RLG_RS_COUNT, 1u << 7);
    w(RLG_RS_INST_COUNT, 0);
    w(RLG_RS_IP_0, 0);
    w(RLG_RS_INST_0, (1u << 14));
    Alu mov = alu_mov_out(0);
    load_fs(&mov, 1, NULL, 0);

    /* Interleaved VBO: xyz float + RGBA8, stride 16 bytes. Triangle covering the left half
     * after the 0.5 x-scale, plus a vertex behind the eye (w clip handled by near plane). */
    struct { float x, y, z; uint8_t c[4]; } vb[3] = {
        { -1, 1, 0, {0, 255, 0, 255} }, { 1, -1, 0, {0, 255, 0, 255} }, { -1, -1, 0, {0, 255, 0, 255} },
    };
    memcpy(dev->vram + VBO, vb, sizeof(vb));
    const uint32_t vbp[] = { 2, (12u >> 2) | ((16u >> 2) << 8) | ((4u >> 2) << 16) | ((16u >> 2) << 24),
                             FB + VBO, FB + VBO + 12 };
    pkt3(RLG_PM4_3D_LOAD_VBPNTR, vbp, 4);
    const uint32_t draw = (2u << 4) | (3u << 16) | 4u;  /* VERTEX_LIST, 3 verts, TRIANGLES */
    pkt3(RLG_PM4_3D_DRAW_VBUF_2, &draw, 1);

    /* x scaled by 0.5: NDC x in [-0.5, 0.5] -> pixels [16, 48]; triangle is bottom-left half. */
    CHECK(px(20, 60) == 0xff00ff00u);
    CHECK(px(40, 4) == 0);                          /* upper-right, outside */
    CHECK(px(8, 60) == 0);                          /* left of x = 16, outside */
    CHECK(dev->r3d->stats.unsupported == 0);
}

/* Indexed draw with the indices DMA'd by INDX_BUFFER (Mesa r300_emit_draw_elements). */
static void test_indexed(void)
{
    setup(RLG_PROFILE_X700PRO);
    setup_bypass_pos_color();
    /* Vertex arrays in bypass mode: pos + colour interleaved, 32-byte stride. */
    const float verts[4][8] = {
        { 0, 0, 0, 1, 0, 0, 1, 1 }, { 64, 0, 0, 1, 0, 0, 1, 1 },
        { 64, 64, 0, 1, 0, 0, 1, 1 }, { 0, 64, 0, 1, 0, 0, 1, 1 },
    };
    memcpy(dev->vram + VBO, verts, sizeof(verts));
    const uint16_t idx[6] = { 0, 1, 2, 0, 2, 3 };
    memcpy(dev->vram + VBO + 0x1000, idx, sizeof(idx));
    const uint32_t vbp[] = { 2, 4u | (8u << 8) | (4u << 16) | (8u << 24), FB + VBO, FB + VBO + 16 };
    pkt3(RLG_PM4_3D_LOAD_VBPNTR, vbp, 4);
    w(RLG_VAP_VF_MAX_VTX_INDX, 3);
    const uint32_t draw = (1u << 4) | (6u << 16) | 4u;  /* WALK_INDICES, 6 indices, TRIANGLES */
    pkt3(RLG_PM4_3D_DRAW_INDX_2, &draw, 1);
    CHECK(px(32, 32) == 0);                         /* nothing drawn until the indices arrive */
    const uint32_t ib[] = { (1u << 31) | (RLG_VAP_PORT_IDX0 >> 2), FB + VBO + 0x1000, 3 };
    pkt3(RLG_PM4_INDX_BUFFER, ib, 3);
    CHECK(px(32, 32) == 0xff0000ffu && px(1, 62) == 0xff0000ffu && px(62, 1) == 0xff0000ffu);
}

/* Depth test (16-bit Z, LESS) and colour blending (SRC_ALPHA, ONE_MINUS_SRC_ALPHA). */
static void test_depth_blend(void)
{
    setup(RLG_PROFILE_X1250_AMD);
    setup_bypass_pos_color();
    w(RLG_ZB_DEPTHOFFSET, FB + ZBUF);
    w(RLG_ZB_DEPTHPITCH, W);
    w(RLG_ZB_FORMAT, 0);                            /* 16-bit int Z */
    for (int i = 0; i < W * H; ++i) { uint16_t z = 0xffff; memcpy(dev->vram + ZBUF + i * 2, &z, 2); }
    w(RLG_ZB_CNTL, (1u << 1) | (1u << 2));          /* Z enable + write */
    w(RLG_ZB_ZSTENCILCNTL, 1);                      /* LESS */
#define QUAD(z, r, g, b, a) { 0, 0, z, 1, r, g, b, a, 64, 0, z, 1, r, g, b, a, \
                              64, 64, z, 1, r, g, b, a, 0, 64, z, 1, r, g, b, a }
    const float red[] = QUAD(0.5f, 1, 0, 0, 1), green[] = QUAD(0.7f, 0, 1, 0, 1), blue[] = QUAD(0.3f, 0, 0, 1, 0.5f);
    draw_immd(13, red, 4, 8);
    draw_immd(13, green, 4, 8);                     /* behind: rejected */
    CHECK(px(32, 32) == 0xffff0000u);
    w(RLG_RB3D_CBLEND, 1u | (38u << 16) | (39u << 24));  /* enable, SRC_ALPHA, ONE_MINUS_SRC_ALPHA */
    draw_immd(13, blue, 4, 8);                      /* in front, 50% blue over red */
    uint32_t c = px(32, 32);
    CHECK(((c >> 16) & 0xff) >= 0x7e && ((c >> 16) & 0xff) <= 0x81);  /* red 0.5 */
    CHECK((c & 0xff) >= 0x7e && (c & 0xff) <= 0x81);                    /* blue 0.5 */
    uint16_t z;
    memcpy(&z, dev->vram + ZBUF + (32 * W + 32) * 2, 2);
    CHECK(abs((int)z - (int)(0.3f * 65535)) <= 1);
}

/* Textured quad: TEX in node 0, ARGB8888 nearest, texcoords through the RS. */
static void test_texture(void)
{
    setup(RLG_PROFILE_X1250_AMD);
    w(RLG_VAP_CNTL_STATUS, RLG_VAP_TCL_BYPASS);
    w(RLG_VAP_VTE_CNTL, RLG_VTX_XY_FMT | RLG_VTX_Z_FMT);
    w(RLG_VAP_OUTPUT_VTX_FMT_0, 1u);                /* POS */
    w(RLG_VAP_OUTPUT_VTX_FMT_1, 4u);                /* TEX0: 4 components */
    w(RLG_VAP_PROG_STREAM_CNTL_0, 3u | ((3u | (6u << 8) | (1u << 13)) << 16));  /* pos, tex0 -> slot 6 */
    w(RLG_VAP_PROG_STREAM_CNTL_EXT_0, 0xf688u | (0xf688u << 16));
    w(RLG_VAP_VTX_SIZE, 8);
    w(RLG_RS_COUNT, 4u);                            /* 4 texture components */
    w(RLG_RS_INST_COUNT, 0);
    w(RLG_RS_IP_0, 0u | (0u << 13) | (1u << 16) | (2u << 19) | (3u << 22)); /* TEX_PTR 0, STRQ = C0..C3 */
    w(RLG_RS_INST_0, 0u | (1u << 3) | (0u << 6));   /* TEX_ID 0 -> temp 0 */
    /* FS: temp1 = TEX(unit 0, temp0); out = temp1 */
    uint32_t tex = 0u | (1u << 6) | (0u << 11) | (1u << 15);   /* src t0, dst t1, unit 0, LD */
    Alu mov = alu_mov_out(1);
    load_fs(&mov, 1, &tex, 1);
    /* 4x4 ARGB8888 checkerboard of two colours, nearest filtering, clamp. */
    uint32_t texels[16];
    for (int i = 0; i < 16; ++i) texels[i] = (((i & 3) + (i >> 2)) & 1) ? 0xff00ff00u : 0xff0000ffu;
    /* Linear 4x4 32bpp: stride aligns to 8 texels on RS690 (64 bytes). */
    for (int y = 0; y < 4; ++y) memcpy(dev->vram + TEX + y * 64, texels + y * 4, 16);
    w(RLG_TX_ENABLE, 1);
    w(RLG_TX_FILTER0_0, 2u | (2u << 3) | (1u << 9) | (1u << 11));   /* clamp, nearest */
    w(RLG_TX_FILTER1_0, 0);
    w(RLG_TX_FORMAT0_0, 3u | (3u << 11));           /* 4x4, 1 level */
    w(RLG_TX_FORMAT1_0, 0xcu | (3u << 9) | (2u << 12) | (1u << 15) | (0u << 18));  /* W8Z8Y8X8, A=W R=Z G=Y B=X */
    w(RLG_TX_FORMAT2_0, 0);
    w(RLG_TX_OFFSET_0, FB + TEX);
    const float v[] = {
        0, 0, 0, 1,    0, 0, 0, 1,
        64, 0, 0, 1,   1, 0, 0, 1,
        64, 64, 0, 1,  1, 1, 0, 1,
        0, 64, 0, 1,   0, 1, 0, 1,
    };
    draw_immd(13, v, 4, 8);
    CHECK(px(8, 8) == 0xff0000ffu);                 /* texel (0,0) */
    CHECK(px(24, 8) == 0xff00ff00u);                /* texel (1,0) */
    CHECK(px(24, 24) == 0xff0000ffu);               /* texel (1,1) */
    CHECK(px(56, 40) == 0xff00ff00u);               /* texel (3,2) */
}

/* u_blitter clear as Mesa r300 emits it: one point sized to the rectangle,
 * constant colour from a float24 FS constant, with SC cliprect scissoring. */
static void test_blitter_clear(void)
{
    setup(RLG_PROFILE_X1250_AMD);
    setup_bypass_pos_color();
    w(RLG_VAP_OUTPUT_VTX_FMT_0, 1u);
    w(RLG_VAP_PROG_STREAM_CNTL_0, 3u | (1u << 13));
    w(RLG_VAP_VTX_SIZE, 4);
    w(RLG_RS_COUNT, 0);
    w(RLG_RS_INST_0, 0);
    Alu mov = alu_mov_out(SRC_CONST(0));
    load_fs(&mov, 1, NULL, 0);
    w(RLG_PFS_PARAM_0 + 0, pack_float24(0.0f));
    w(RLG_PFS_PARAM_0 + 4, pack_float24(0.5f));
    w(RLG_PFS_PARAM_0 + 8, pack_float24(1.0f));
    w(RLG_PFS_PARAM_0 + 12, pack_float24(1.0f));
    /* scissor to x in [8, 23], y in [8, 23] through cliprect 0 + rule 0xAAAA */
    w(RLG_SC_CLIPRECT_TL_0, (8u + 1440) | ((8u + 1440) << 13));
    w(RLG_SC_CLIPRECT_BR_0, (23u + 1440) | ((23u + 1440) << 13));
    w(RLG_SC_CLIP_RULE, 0xaaaa);
    w(RLG_GA_POINT_SIZE, (64u * 6) | ((64u * 6) << 16));
    w(RLG_VAP_CLIP_CNTL, RLG_CLIP_DISABLE);
    const float v[] = { 32, 32, 0, 1 };
    draw_immd(1, v, 1, 4);                          /* PRIM_POINTS */
    CHECK(px(8, 8) == 0xff0080ffu || px(8, 8) == 0xff007fffu);   /* B=1, G=0.5 */
    CHECK(px(23, 23) == px(8, 8));
    CHECK(px(7, 8) == 0 && px(24, 23) == 0 && px(8, 24) == 0);
}

/* Point sprite texcoord stuffing (GB_ENABLE + GA_POINT_S0..T1), used by blits. */
static void test_point_stuffing(void)
{
    setup(RLG_PROFILE_X1250_AMD);
    setup_bypass_pos_color();
    w(RLG_VAP_OUTPUT_VTX_FMT_0, 1u);
    w(RLG_VAP_PROG_STREAM_CNTL_0, 3u | (1u << 13));
    w(RLG_VAP_VTX_SIZE, 4);
    w(RLG_GB_ENABLE, RLG_GB_POINT_STUFF_ENABLE | (2u << 16));   /* TEX0 <- STR */
    wf(RLG_GA_POINT_S0, 0.0f); wf(RLG_GA_POINT_S0 + 4, 1.0f);   /* LLC (s0, t0) */
    wf(RLG_GA_POINT_S0 + 8, 1.0f); wf(RLG_GA_POINT_S0 + 12, 0.0f); /* URC (s1, t1) */
    w(RLG_GA_POINT_SIZE, (64u * 6) | ((64u * 6) << 16));
    w(RLG_RS_COUNT, 3u);
    w(RLG_RS_INST_COUNT, 0);
    w(RLG_RS_IP_0, (0u << 13) | (1u << 16) | (4u << 19) | (5u << 22));  /* XY01 */
    w(RLG_RS_INST_0, (1u << 3));
    Alu mov = alu_mov_out(0);                       /* colour = (s, t, 0, 1) */
    load_fs(&mov, 1, NULL, 0);
    const float v[] = { 32, 32, 0, 1 };
    draw_immd(1, v, 1, 4);
    uint32_t tl = px(0, 0), br = px(63, 63);
    CHECK(((tl >> 16) & 0xff) < 4 && ((tl >> 8) & 0xff) < 4);     /* s ~ 0, t ~ 0 at top-left */
    CHECK(((br >> 16) & 0xff) > 250 && ((br >> 8) & 0xff) > 250); /* s ~ 1, t ~ 1 at bottom-right */
}

/* Run a one-instruction FS over a full-screen quad and return the pixel. */
static const float fullscreen[] = {
    0, 0, 0, 1,    0, 0, 0, 1,   64, 0, 0, 1,   0, 0, 0, 1,
    64, 64, 0, 1,  0, 0, 0, 1,   0, 64, 0, 1,   0, 0, 0, 1,
};

static void set_const(unsigned i, float x, float y, float z, float a)
{
    w(RLG_PFS_PARAM_0 + i * 16 + 0, pack_float24(x));
    w(RLG_PFS_PARAM_0 + i * 16 + 4, pack_float24(y));
    w(RLG_PFS_PARAM_0 + i * 16 + 8, pack_float24(z));
    w(RLG_PFS_PARAM_0 + i * 16 + 12, pack_float24(a));
}

static uint32_t run_alu_const(Alu a)
{
    memset(dev->vram + CBUF, 0, W * H * 4);
    load_fs(&a, 1, NULL, 0);
    draw_immd(13, fullscreen, 4, 8);
    return px(10, 10);
}

#define B(v) ((uint32_t)((v) * 255.0f + 0.5f))
#define ARGB(r, g, b, a) ((B(a) << 24) | (B(r) << 16) | (B(g) << 8) | B(b))
#define SRCS(a, b, c) ((a) | ((b) << 6) | ((c) << 12))

/* ALU semantics, checked against hand-computed results. */
static void test_fs_alu(void)
{
    setup(RLG_PROFILE_X1250_AMD);
    setup_bypass_pos_color();
    set_const(0, 0.25f, 0.5f, 0.75f, 0.5f);
    set_const(1, 2.0f, 1.0f, 0.5f, 0.5f);
    set_const(2, 0.1f, 0.21f, 0.3f, 0.25f);    /* 0.2 would land on a rounding edge in FP24 */
    set_const(3, -1.0f, 1.0f, -1.0f, 1.0f);
    const uint32_t srcs = SRCS(SRC_CONST(0), SRC_CONST(1), SRC_CONST(2));
    const uint32_t outs_rgb = 7u << 26, outs_a = 1u << 24;
    const uint32_t clamp = 1u << 30;
    Alu a;

    /* MAD: c0 * c1 + c2 */
    a = (Alu){ srcs | outs_rgb, srcs | outs_a,
               ARGC_SRC0C_XYZ | (ARGC_SRC1C_XYZ << 7) | (ARGC_SRC2C_XYZ << 14) | OUTC_MAD | clamp,
               9u | (10u << 7) | (11u << 14) | OUTA_MAD | clamp };
    CHECK(run_alu_const(a) == ARGB(0.6f, 0.71f, 0.675f, 0.5f));

    /* DP3 (RGB) + DP4 (alpha) of c0 . c2: 0.025 + 0.105 + 0.225 = 0.355 everywhere */
    a = (Alu){ srcs | outs_rgb, srcs | outs_a,
               ARGC_SRC0C_XYZ | (ARGC_SRC2C_XYZ << 7) | (ARGC_ZERO << 14) | (1u << 23) | clamp,
               ARGA_ZERO | (ARGA_ZERO << 7) | (ARGA_ZERO << 14) | (1u << 23) | clamp };
    CHECK(run_alu_const(a) == ARGB(0.355f, 0.355f, 0.355f, 0.355f));

    /* CMP (hardware order): arg2 >= 0 ? arg0 : arg1 with arg2 = c3 = (-1, 1, -1) */
    const uint32_t srcs3 = SRCS(SRC_CONST(0), SRC_CONST(1), SRC_CONST(3));
    a = (Alu){ srcs3 | outs_rgb, srcs3 | outs_a,
               ARGC_SRC0C_XYZ | (ARGC_SRC1C_XYZ << 7) | (ARGC_SRC2C_XYZ << 14) | (8u << 23) | clamp,
               ARGA_ONE | (ARGA_ONE << 7) | (ARGA_ZERO << 14) | OUTA_MAD };
    CHECK(run_alu_const(a) == ARGB(1.0f, 0.5f, 0.5f, 1.0f));

    /* presub 1 - src0 through SRCP_XYZ: 1 - c0 */
    a = (Alu){ srcs | outs_rgb, srcs | outs_a,
               15u | (ARGC_ONE << 7) | (ARGC_ZERO << 14) | (3u << 21) | OUTC_MAD,
               ARGA_ONE | (ARGA_ONE << 7) | (ARGA_ZERO << 14) | OUTA_MAD };
    CHECK(run_alu_const(a) == ARGB(0.75f, 0.5f, 0.25f, 1.0f));

    /* native swizzle SRC0C_YZX */
    a = (Alu){ srcs | outs_rgb, srcs | outs_a,
               23u | (ARGC_ONE << 7) | (ARGC_ZERO << 14) | OUTC_MAD,
               ARGA_ONE | (ARGA_ONE << 7) | (ARGA_ZERO << 14) | OUTA_MAD };
    CHECK(run_alu_const(a) == ARGB(0.5f, 0.75f, 0.25f, 1.0f));

    /* alpha RCP(c0.a = 0.5) = 2, output modifier DIV8 -> 0.25, REPL_ALPHA into RGB */
    a = (Alu){ srcs | outs_rgb, srcs | outs_a,
               ARGC_ZERO | (ARGC_ZERO << 7) | (ARGC_ZERO << 14) | (10u << 23),
               ARGA_SRC0A | (ARGA_ZERO << 7) | (ARGA_ZERO << 14) | (10u << 23) | (6u << 27) };
    CHECK(run_alu_const(a) == ARGB(0.25f, 0.25f, 0.25f, 0.25f));
}

/* Mip selection + level layout (RS690 linear rules of r300_texture_desc.c). */
static void test_mipmaps(void)
{
    /* 256x256 ARGB8888, 4 levels. On RS690 the linear 32bpp width alignment is 16 texels,
     * so strides are 1024/512/256/128 bytes and the level offsets are:
     *   L0 0x00000 (256 rows), L1 0x40000 (128), L2 0x50000 (64), L3 0x54000. */
    static const uint32_t off[4] = { 0x0, 0x40000, 0x50000, 0x54000 };
    static const uint32_t col[4] = { 0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xffffffffu };
    setup(RLG_PROFILE_X1250_AMD);
    for (int l = 0; l < 4; ++l) {
        unsigned sz = 256u >> l;
        for (unsigned i = 0; i < sz * sz; ++i) memcpy(dev->vram + TEX + off[l] + i * 4, &col[l], 4);
    }
    w(RLG_VAP_CNTL_STATUS, RLG_VAP_TCL_BYPASS);
    w(RLG_VAP_VTE_CNTL, RLG_VTX_XY_FMT | RLG_VTX_Z_FMT);
    w(RLG_VAP_OUTPUT_VTX_FMT_0, 1u);
    w(RLG_VAP_OUTPUT_VTX_FMT_1, 4u);
    w(RLG_VAP_PROG_STREAM_CNTL_0, 3u | ((3u | (6u << 8) | (1u << 13)) << 16));
    w(RLG_VAP_PROG_STREAM_CNTL_EXT_0, 0xf688u | (0xf688u << 16));
    w(RLG_VAP_VTX_SIZE, 8);
    w(RLG_RS_COUNT, 4u);
    w(RLG_RS_INST_COUNT, 0);
    w(RLG_RS_IP_0, (1u << 16) | (2u << 19) | (3u << 22));
    w(RLG_RS_INST_0, (1u << 3));
    uint32_t tex = (1u << 6) | (1u << 15);
    Alu mov = alu_mov_out(1);
    load_fs(&mov, 1, &tex, 1);
    w(RLG_TX_ENABLE, 1);
    w(RLG_TX_FILTER0_0, (1u << 9) | (1u << 11) | (1u << 13));   /* nearest, MIP_NEAREST */
    w(RLG_TX_FORMAT0_0, 255u | (255u << 11) | (3u << 26));      /* 256x256, levels 0..3 */
    w(RLG_TX_FORMAT1_0, 0xcu | (3u << 9) | (2u << 12) | (1u << 15));
    w(RLG_TX_OFFSET_0, FB + TEX);
    const float quad[] = {
        0, 0, 0, 1, 0, 0, 0, 1,   64, 0, 0, 1, 1, 0, 0, 1,
        64, 64, 0, 1, 1, 1, 0, 1, 0, 64, 0, 1, 0, 1, 0, 1,
    };
    draw_immd(13, quad, 4, 8);                      /* 256 texels over 64 px: LOD 2 */
    CHECK(px(20, 20) == col[2]);
    const float small[] = {
        0, 0, 0, 1, 0, 0, 0, 1,   32, 0, 0, 1, 1, 0, 0, 1,
        32, 32, 0, 1, 1, 1, 0, 1, 0, 32, 0, 1, 0, 1, 0, 1,
    };
    draw_immd(13, small, 4, 8);                     /* 256 texels over 32 px: LOD 3 */
    CHECK(px(10, 10) == col[3]);
}

/* Back-face culling (SU_CULL_MODE) and stencil (24/8 buffer, REPLACE then EQUAL). */
static void test_cull_stencil(void)
{
    setup(RLG_PROFILE_X1250_AMD);
    setup_bypass_pos_color();
    const float cw[] = { 0, 0, 0, 1, 1, 1, 1, 1,  64, 0, 0, 1, 1, 1, 1, 1,  0, 64, 0, 1, 1, 1, 1, 1 };
    const float ccw[] = { 0, 0, 0, 1, 1, 1, 1, 1,  0, 64, 0, 1, 1, 1, 1, 1,  64, 0, 0, 1, 1, 1, 1, 1 };
    w(RLG_SU_CULL_MODE, RLG_CULL_BACK);             /* front = CCW (on screen, y down) */
    draw_immd(4, cw, 3, 8);
    CHECK(px(5, 5) == 0);                           /* clockwise on screen: back, culled */
    draw_immd(4, ccw, 3, 8);
    CHECK(px(5, 5) == 0xffffffffu);

    setup(RLG_PROFILE_X1250_AMD);
    setup_bypass_pos_color();
    w(RLG_ZB_DEPTHOFFSET, FB + ZBUF);
    w(RLG_ZB_DEPTHPITCH, W);
    w(RLG_ZB_FORMAT, 2);                            /* 24-bit Z + 8-bit stencil */
    w(RLG_ZB_CNTL, 1u);                             /* stencil only */
    w(RLG_ZB_STENCILREFMASK, 5u | (0xffu << 8) | (0xffu << 16));
    /* ALWAYS, zpass REPLACE */
    w(RLG_ZB_ZSTENCILCNTL, (7u << 3) | (2u << 9));
    const float left[] = { 0, 0, 0, 1, 1, 0, 0, 1,  32, 0, 0, 1, 1, 0, 0, 1,
                           32, 64, 0, 1, 1, 0, 0, 1,  0, 64, 0, 1, 1, 0, 0, 1 };
    draw_immd(13, left, 4, 8);                      /* stencil = 5 on the left half */
    w(RLG_ZB_ZSTENCILCNTL, (3u << 3));              /* EQUAL, keep */
    const float all[] = { 0, 0, 0, 1, 0, 1, 0, 1,  64, 0, 0, 1, 0, 1, 0, 1,
                          64, 64, 0, 1, 0, 1, 0, 1,  0, 64, 0, 1, 0, 1, 0, 1 };
    draw_immd(13, all, 4, 8);
    CHECK(px(10, 30) == 0xff00ff00u);               /* stencil 5: green passes */
    CHECK(px(50, 30) == 0);                         /* stencil 0: rejected */
    uint32_t zs;
    memcpy(&zs, dev->vram + ZBUF + (30 * W + 10) * 4, 4);
    CHECK((zs & 0xff) == 5);
}

/* CBZB clear exactly as Mesa r300_clear() sets it up for a 64x64 ARGB8888 surface:
 * blitter rect of 64 x 32, ZB pointed at the midpoint (stride * 32 & ~2047) with the
 * 24/8 format and ZB_DEPTHCLEARVALUE = packed colour, ZB_BW_CNTL.CB_CLEAR. */
static void test_cbzb_clear(void)
{
    setup(RLG_PROFILE_X700);
    setup_bypass_pos_color();
    w(RLG_VAP_OUTPUT_VTX_FMT_0, 1u);
    w(RLG_VAP_PROG_STREAM_CNTL_0, 3u | (1u << 13));
    w(RLG_VAP_VTX_SIZE, 4);
    w(RLG_RS_COUNT, 0);
    w(RLG_RS_INST_0, 0);
    Alu mov = alu_mov_out(SRC_CONST(0));
    load_fs(&mov, 1, NULL, 0);
    set_const(0, 1.0f, 0.0f, 1.0f, 1.0f);           /* magenta = 0xffff00ff */
    w(RLG_ZB_FORMAT, 2);
    w(RLG_ZB_DEPTHOFFSET, (FB + CBUF + W * 4 * 32) & ~2047u);
    w(RLG_ZB_DEPTHPITCH, W);
    w(RLG_ZB_DEPTHCLEARVALUE, 0xffff00ffu);
    w(RLG_ZB_BW_CNTL, RLG_ZB_CB_CLEAR_WRITE_ONLY);
    w(RLG_ZB_CNTL, 0);
    w(RLG_VAP_CLIP_CNTL, RLG_CLIP_DISABLE);
    w(RLG_GA_POINT_SIZE, (32u * 6) | ((64u * 6) << 16));
    const float v[] = { 32, 16, 0, 1 };
    draw_immd(1, v, 1, 4);
    unsigned ok = 0;
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) ok += px(x, y) == 0xffff00ffu;
    CHECK(ok == W * H);                             /* both halves cleared */
    w(RLG_ZB_BW_CNTL, 0);
}

int main(void)
{
    test_float24();
    test_bypass_triangle();
    test_gouraud_quad();
    test_tcl_vbo();
    test_indexed();
    test_depth_blend();
    test_texture();
    test_blitter_clear();
    test_point_stuffing();
    test_fs_alu();
    test_mipmaps();
    test_cull_stencil();
    test_cbzb_clear();
    rlg_destroy(dev);
    if (failures) {
        fprintf(stderr, "%d 3D check(s) failed\n", failures);
        return 1;
    }
    puts("all 3D tests passed");
    return 0;
}
