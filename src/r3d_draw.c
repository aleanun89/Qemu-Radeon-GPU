#include "radeon_r3d.h"
#include <stdlib.h>
#include <string.h>

/* CP 3D packets (Mesa r300_render.c / r300_emit.c emit these). */

#define VF_PRIM(v)      ((v) & 0xfu)
#define VF_WALK(v)      (((v) >> 4) & 3u)
#define VF_INDEX32      (1u << 11)
#define VF_COUNT(v)     ((v) >> 16)
enum { WALK_INDICES = 1, WALK_LIST = 2, WALK_EMBEDDED = 3 };

static void run_draw(RLGDevice *d, uint32_t vf, const uint32_t *indices, const uint32_t *imm,
                     uint32_t imm_dwords)
{
    struct RLG3D *r = d->r3d;
    uint32_t n = VF_COUNT(vf), vtx_size = rlg_reg_read32(d, RLG_VAP_VTX_SIZE) & 0x7fu;
    uint32_t max_idx = rlg_reg_read32(d, RLG_VAP_VF_MAX_VTX_INDX) & 0xffffffu;
    uint32_t min_idx = rlg_reg_read32(d, RLG_VAP_VF_MIN_VTX_INDX) & 0xffffffu;
    R3DVertexSetup vs;
    R3DVertex *verts;

    if (!n) {
        return;
    }
    if (imm && (uint64_t)n * vtx_size > imm_dwords) {
        r->stats.unsupported++;
        return;
    }
    verts = calloc(n, sizeof(*verts));
    if (!verts) {
        return;
    }
    r3d_vertex_setup(d, &vs);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t idx = indices ? indices[i] : i;
        if (indices && max_idx && (idx > max_idx || idx < min_idx)) {
            idx = idx > max_idx ? max_idx : min_idx;
        }
        if (r3d_vertex_process(d, &vs, idx, imm ? imm + (size_t)i * vtx_size : NULL, &verts[i]) != 0) {
            rlg__log(d, "3D: vertex fetch fault (vertex %u, index %u)", i, idx);
            free(verts);
            r->stats.unsupported++;
            return;
        }
    }
    r->stats.draws++;
    r3d_draw_prims(d, VF_PRIM(vf), verts, n);
    free(verts);
}

static void finish_indexed(RLGDevice *d)
{
    struct RLG3D *r = d->r3d;
    r->idx_pending = false;
    run_draw(d, r->idx_vf_cntl, r->idx_buf, NULL, 0);
    free(r->idx_buf);
    r->idx_buf = NULL;
}

/* Append index data (one dword = two 16-bit or one 32-bit index). */
static void push_index_dword(RLGDevice *d, uint32_t v)
{
    struct RLG3D *r = d->r3d;
    if (!r->idx_pending) {
        return;
    }
    if (r->idx_vf_cntl & VF_INDEX32) {
        r->idx_buf[r->idx_have++] = v;
    } else {
        r->idx_buf[r->idx_have++] = v & 0xffffu;
        if (r->idx_have < r->idx_total) {
            r->idx_buf[r->idx_have++] = v >> 16;
        }
    }
    if (r->idx_have >= r->idx_total) {
        finish_indexed(d);
    }
}

void r3d_port_idx(RLGDevice *d, uint32_t value)
{
    push_index_dword(d, value);
}

static void begin_indexed(RLGDevice *d, uint32_t vf, const uint32_t *inline_data, uint32_t ndw)
{
    struct RLG3D *r = d->r3d;
    uint32_t n = VF_COUNT(vf);

    free(r->idx_buf);
    r->idx_buf = calloc(n ? n : 1, sizeof(uint32_t));
    if (!r->idx_buf) {
        r->idx_pending = false;
        return;
    }
    r->idx_pending = true;
    r->idx_vf_cntl = vf;
    r->idx_total = n;
    r->idx_have = 0;
    if (!n) {
        finish_indexed(d);
        return;
    }
    for (uint32_t i = 0; i < ndw && r->idx_pending; ++i) {
        push_index_dword(d, inline_data[i]);
    }
}

void r3d_packet3(RLGDevice *d, uint32_t op, const uint32_t *p, uint32_t count)
{
    struct RLG3D *r = d->r3d;

    if (!r || !count) {
        return;
    }
    switch (op) {
    case RLG_PM4_3D_LOAD_VBPNTR: {
        unsigned n = p[0] & 0x1fu, k = 1;
        r->varray_count = 0;
        for (unsigned i = 0; i < n && i < R3D_MAX_VARRAYS && k < count; i += 2) {
            uint32_t w = p[k++];
            r->varray[i].size_dw = w & 0x7fu;
            r->varray[i].stride_dw = (w >> 8) & 0x7fu;
            r->varray[i].addr = k < count ? p[k++] : 0;
            r->varray_count = i + 1;
            if (i + 1 < n && i + 1 < R3D_MAX_VARRAYS) {
                r->varray[i + 1].size_dw = (w >> 16) & 0x7fu;
                r->varray[i + 1].stride_dw = (w >> 24) & 0x7fu;
                r->varray[i + 1].addr = k < count ? p[k++] : 0;
                r->varray_count = i + 2;
            }
        }
        break;
    }
    case RLG_PM4_3D_DRAW_VBUF_2:
        run_draw(d, p[0], NULL, NULL, 0);
        break;
    case RLG_PM4_3D_DRAW_IMMD_2:
        if (VF_WALK(p[0]) == WALK_EMBEDDED) {
            run_draw(d, p[0], NULL, p + 1, count - 1);
        } else {
            r->stats.unsupported++;
        }
        break;
    case RLG_PM4_3D_DRAW_INDX_2:
        begin_indexed(d, p[0], p + 1, count - 1);
        break;
    case RLG_PM4_INDX_BUFFER: {
        /* ONE_REG_WR to VAP_PORT_IDX0: DMA `p[2]` dwords from GPU address p[1]. */
        uint32_t addr = p[1] & ~3u, ndw = count > 2 ? p[2] : 0;
        for (uint32_t i = 0; i < ndw && r->idx_pending; ++i) {
            uint32_t v;
            if (rlg_gpu_read(d, (uint64_t)addr + i * 4u, &v, 4) != 0) {
                rlg__log(d, "3D: index buffer fault at 0x%08x", addr + i * 4u);
                r->idx_pending = false;
                break;
            }
            push_index_dword(d, v);
        }
        break;
    }
    case RLG_PM4_3D_DRAW_VBUF: case RLG_PM4_3D_DRAW_IMMD: case RLG_PM4_3D_DRAW_INDX:
    default:
        r->stats.unsupported++;         /* pre-R300 draw packets */
        break;
    }
}
