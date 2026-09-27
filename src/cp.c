#include "radeon_legacy_int.h"

/*
 * R100-R500 command processor: primary ring + one level of indirect buffers
 * (PACKET0 writes to CP_IB_BASE/CP_IB_BUFSZ, as emitted by r100_ring_ib_execute).
 */

#define RLG_IB_MAX_DWORDS (1u << 20)

typedef struct {
    bool ring;
    uint64_t base;      /* GPU (MC) address */
    uint32_t mask;      /* ring only */
    uint32_t pos;       /* dword index */
    uint32_t end;       /* IB only: dword count */
} Stream;

static uint32_t stream_avail(const RLGDevice *d, const Stream *st)
{
    if (st->ring) {
        return (d->cp.wptr - st->pos) & st->mask;
    }
    return st->end - st->pos;
}

static int stream_read(RLGDevice *d, Stream *st, uint32_t *v)
{
    uint32_t idx = st->ring ? (st->pos & st->mask) : st->pos;
    if (rlg_gpu_read(d, st->base + (uint64_t)idx * 4u, v, 4) != 0) {
        return -1;
    }
    st->pos = st->ring ? ((st->pos + 1u) & st->mask) : st->pos + 1u;
    d->cp.dwords++;
    return 0;
}

static void cp_reg_write(RLGDevice *d, uint32_t reg, uint32_t v)
{
    if (reg >= RLG_MMIO_SIZE) {
        rlg__log(d, "CP: PACKET write outside MMIO 0x%x", reg);
        d->cp.faults++;
        return;
    }
    rlg_mmio_write(d, reg, v, 4);
}

static bool is_3d_draw(uint32_t op)
{
    switch (op) {
    case RLG_PM4_3D_DRAW_VBUF:
    case RLG_PM4_3D_DRAW_IMMD:
    case RLG_PM4_3D_DRAW_INDX:
    case RLG_PM4_3D_DRAW_VBUF_2:
    case RLG_PM4_3D_DRAW_IMMD_2:
    case RLG_PM4_3D_DRAW_INDX_2:
        return true;
    default:
        return false;
    }
}

/* Returns 0 when the stream was drained (or is waiting for more data), <0 on fault. */
static int exec_stream(RLGDevice *d, Stream *st, uint32_t budget)
{
    while (stream_avail(d, st) && budget) {
        uint32_t start = st->pos, h, count, v, v2;

        if (stream_read(d, st, &h) != 0) {
            goto fault;
        }
        switch (RLG_PM4_TYPE(h)) {
        case 0:
            count = RLG_PM4_COUNT(h);
            if (count > stream_avail(d, st)) {
                st->pos = start;            /* incomplete packet: wait for the rest */
                return 0;
            }
            for (uint32_t i = 0; i < count; ++i) {
                if (stream_read(d, st, &v) != 0) {
                    goto fault;
                }
                cp_reg_write(d, RLG_PM4_PKT0_REG(h) + (RLG_PM4_PKT0_ONE_REG_WR(h) ? 0u : i * 4u), v);
            }
            break;
        case 1:
            if (stream_avail(d, st) < 2) {
                st->pos = start;
                return 0;
            }
            if (stream_read(d, st, &v) != 0 || stream_read(d, st, &v2) != 0) {
                goto fault;
            }
            cp_reg_write(d, RLG_PM4_PKT1_REG0(h), v);
            cp_reg_write(d, RLG_PM4_PKT1_REG1(h), v2);
            break;
        case 2:
            break;
        case 3: {
            uint32_t op = RLG_PM4_PKT3_OPCODE(h);
            count = RLG_PM4_COUNT(h);
            if (count > stream_avail(d, st)) {
                st->pos = start;
                return 0;
            }
            if (is_3d_draw(op)) {
                d->cp.draw_calls++;         /* TODO: hand state + vertices to the 3D backend */
            } else if (op != RLG_PM4_NOP && op != RLG_PM4_WAIT_FOR_IDLE &&
                       op != RLG_PM4_3D_LOAD_VBPNTR && op != RLG_PM4_INDX_BUFFER &&
                       op != RLG_PM4_3D_CLEAR_ZMASK) {
                d->cp.unknown_packets++;
                rlg__log(d, "CP: unhandled PACKET3 opcode 0x%02x (%u dwords)", op, count);
            }
            for (uint32_t i = 0; i < count; ++i) {
                if (stream_read(d, st, &v) != 0) {
                    goto fault;
                }
            }
            break;
        }
        }
        d->cp.packets++;
        budget--;
        if (st->ring) {
            d->cp.rptr = st->pos;
        }
    }
    return 0;

fault:
    d->cp.faults++;
    rlg__log(d, "CP: fetch fault at %s dword %u", st->ring ? "ring" : "IB", st->pos);
    return -1;
}

int rlg__cp_exec_ib(RLGDevice *d, uint32_t gpu_addr, uint32_t ndw)
{
    Stream st = { .ring = false, .base = gpu_addr & ~3u, .pos = 0, .end = ndw };
    int r;

    if (d->cp.depth > 0 || ndw > RLG_IB_MAX_DWORDS) {
        rlg__log(d, "CP: rejected IB at 0x%08x (%u dwords, depth %u)", gpu_addr, ndw, d->cp.depth);
        d->cp.faults++;
        return -1;
    }
    d->cp.indirect_buffers++;
    d->cp.depth++;
    r = exec_stream(d, &st, ndw);
    d->cp.depth--;
    return r;
}

void rlg__scratch_writeback(RLGDevice *d, unsigned index, uint32_t value)
{
    if (index >= RLG_NUM_SCRATCH || !(d->cp.scratch_umsk & (1u << index)) || !d->cp.scratch_addr) {
        return;
    }
    if (rlg_gpu_write(d, (uint64_t)d->cp.scratch_addr + index * 4u, &value, 4) != 0) {
        d->cp.faults++;
    }
}

int rlg_cp_process(RLGDevice *d)
{
    uint32_t bufsz, n;
    Stream st;
    int r;

    if (!d || d->cp.busy) {
        return 0;
    }
    if ((d->cp.csq_cntl >> RLG_CSQ_MODE_SHIFT) == 0) {
        return 0;                           /* CSQ_PRIDIS_INDDIS: CP disabled */
    }
    bufsz = d->cp.cntl & RLG_RB_BUFSZ_MASK;
    if (bufsz > 22) {
        d->cp.faults++;
        return -1;
    }
    n = 2u << bufsz;                        /* ring size in dwords (r100_cp_init) */
    st = (Stream){ .ring = true, .base = d->cp.base, .mask = n - 1u, .pos = d->cp.rptr & (n - 1u) };

    d->cp.busy = true;
    r = exec_stream(d, &st, 2u * n);
    d->cp.busy = false;

    if (!(d->cp.cntl & RLG_RB_NO_UPDATE) && d->cp.rptr_addr) {
        if (rlg_gpu_write(d, d->cp.rptr_addr & ~3u, &d->cp.rptr, 4) != 0) {
            d->cp.faults++;
        }
    }
    return r;
}
