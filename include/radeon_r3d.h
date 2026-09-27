#pragma once
/*
 * R300/R400 3D engine: software reference implementation.
 *
 * The pipeline mirrors the hardware blocks, and every encoding below follows
 * the Mesa r300 driver (src/gallium/drivers/r300 and its compiler/):
 *
 *   CP 3D_DRAW_*  ->  VAP (vertex fetch, PVS or TCL bypass, VTE)
 *                 ->  GA/SU (primitive assembly, clipping, culling, points)
 *                 ->  SC/RS (scan conversion, interpolators -> FS temps)
 *                 ->  US (fragment program) + TX (texture units)
 *                 ->  FG/ZB/RB3D (alpha test, depth/stencil, blend, write)
 *
 * Surfaces are stored linearly regardless of the tiling bits: the CPU only
 * ever sees tiled surfaces de-tiled (surface registers), and every tiled
 * access goes through this emulated GPU, so a linear layout is consistent.
 */
#include "radeon_legacy_int.h"

#define R3D_PVS_VECTORS     1536u   /* code 0-511, constants 512-1023, UCP 1024+ */
#define R3D_PVS_CONST_START 512u
#define R3D_US_ALU_MAX      512u    /* 64 per bank, R400 r390 mode has 8 banks */
#define R3D_US_TEX_MAX      128u
#define R3D_MAX_VARRAYS     16u
#define R3D_MAX_TEMPS       64u
#define R3D_MAX_TEXCOORD    8u
#define R3D_MAX_COLORS      4u

typedef struct {
    uint32_t addr;          /* GPU address */
    uint32_t size_dw;
    uint32_t stride_dw;
} R3DVertexArray;

/* Post-VTE vertex: screen position, 1/w, and the attributes the RS can read. */
typedef struct {
    float pos[4];           /* clip space until VTE, then x, y (pixels), z (0..1), w */
    float rhw;              /* 1/w for perspective-correct interpolation */
    float color[R3D_MAX_COLORS][4];
    float tex[R3D_MAX_TEXCOORD][4];
    float psize;
} R3DVertex;

typedef struct {
    uint64_t draws, prims, fragments, killed, unsupported;
} R3DStats;

struct RLG3D {
    uint32_t pvs[R3D_PVS_VECTORS][4];
    uint32_t pvs_index;     /* vector */
    uint32_t pvs_sub;       /* dword within the vector */

    uint32_t alu_rgb_inst[R3D_US_ALU_MAX], alu_rgb_addr[R3D_US_ALU_MAX];
    uint32_t alu_alpha_inst[R3D_US_ALU_MAX], alu_alpha_addr[R3D_US_ALU_MAX];
    uint32_t alu_ext[R3D_US_ALU_MAX];
    uint32_t tex_inst[R3D_US_TEX_MAX];

    R3DVertexArray varray[R3D_MAX_VARRAYS];
    unsigned varray_count;

    /* Indexed draw waiting for its indices (DRAW_INDX_2 + INDX_BUFFER/PORT_IDX0). */
    bool idx_pending;
    uint32_t idx_vf_cntl;
    uint32_t idx_total, idx_have;
    uint32_t *idx_buf;

    R3DStats stats;
};

/* r3d_state.c */
int r3d_init(RLGDevice *d);
void r3d_fini(RLGDevice *d);
void r3d_reset(RLGDevice *d);
/* Register side effects for the 3D block; returns true when handled. */
bool r3d_reg_write(RLGDevice *d, uint32_t addr, uint32_t value);
float r3d_float24(uint32_t v);
float r3d_half(uint16_t h);
static inline float r3d_regf(const RLGDevice *d, uint32_t addr)
{
    union { uint32_t u; float f; } c;
    c.u = rlg_reg_read32(d, addr);
    return c.f;
}

/* r3d_draw.c: PACKET3 handlers; `payload` holds `count` dwords. */
void r3d_packet3(RLGDevice *d, uint32_t opcode, const uint32_t *payload, uint32_t count);
void r3d_port_idx(RLGDevice *d, uint32_t value);

/* r3d_vertex.c */
typedef struct {
    bool tcl;               /* PVS enabled (not TCL_BYPASS) */
    uint32_t stream[16];    /* PROG_STREAM_CNTL halves */
    uint32_t ext[16];       /* PROG_STREAM_CNTL_EXT halves */
    unsigned nstreams;
    uint32_t pvs_first, pvs_last;
    uint32_t const_base;
} R3DVertexSetup;
void r3d_vertex_setup(RLGDevice *d, R3DVertexSetup *vs);
/* Fetch + shade one vertex. `imm` points to embedded data (IMMD) or NULL. */
int r3d_vertex_process(RLGDevice *d, const R3DVertexSetup *vs, uint32_t index,
                       const uint32_t *imm, R3DVertex *out);
/* Perspective divide + viewport transform (VAP_VTE_CNTL, SE_VPORT_*). */
void r3d_vertex_vte(RLGDevice *d, R3DVertex *v);

/* r3d_fs.c */
typedef struct R3DFragProg R3DFragProg;
R3DFragProg *r3d_fs_build(RLGDevice *d);
void r3d_fs_free(R3DFragProg *p);
/* Runs the program on a 2x2 quad (lanes: (x,y), (x+1,y), (x,y+1), (x+1,y+1)).
 * temps[] holds the RS-initialised inputs on entry; KIL clears alive[]. */
void r3d_fs_run_quad(RLGDevice *d, const R3DFragProg *p, float temps[4][R3D_MAX_TEMPS][4],
                     float out[4][4][4], float depth[4], bool depth_written[4], bool alive[4]);

/* r3d_tex.c */
void r3d_tex_sample(RLGDevice *d, unsigned unit, const float coord[4], float lod,
                    float out[4]);
float r3d_tex_lod(RLGDevice *d, unsigned unit, const float dx[2], const float dy[2]);

/* r3d_raster.c */
void r3d_draw_prims(RLGDevice *d, uint32_t prim, R3DVertex *verts, uint32_t count);
