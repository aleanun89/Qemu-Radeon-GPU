#include "radeon_legacy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* ---- fake guest system memory ------------------------------------------------ */

#define SYSMEM_SIZE (8u << 20)
static uint8_t *sysmem;
static bool irq_level;

static int dma_rd(void *o, uint64_t a, void *b, size_t l)
{
    (void)o;
    if (a > SYSMEM_SIZE || l > SYSMEM_SIZE - a) return -1;
    memcpy(b, sysmem + a, l);
    return 0;
}

static int dma_wr(void *o, uint64_t a, const void *b, size_t l)
{
    (void)o;
    if (a > SYSMEM_SIZE || l > SYSMEM_SIZE - a) return -1;
    memcpy(sysmem + a, b, l);
    return 0;
}

static void irq_cb(void *o, bool l) { (void)o; irq_level = l; }

static RLGDevice *make(RLGProfile p)
{
    RLGConfig c = { .profile = p, .vram_size = 16u << 20, .dma_read = dma_rd, .dma_write = dma_wr, .irq = irq_cb };
    memset(sysmem, 0, SYSMEM_SIZE);
    irq_level = false;
    return rlg_create(&c);
}

static void w(RLGDevice *d, uint32_t a, uint32_t v) { rlg_mmio_write(d, a, v, 4); }
static uint32_t r(RLGDevice *d, uint32_t a) { return (uint32_t)rlg_mmio_read(d, a, 4); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static void mc_w(RLGDevice *d, uint32_t reg, uint32_t v)
{
    switch (d->chip->family) {
    case RLG_FAMILY_RS690: w(d, RLG_RS690_MC_INDEX, reg | RLG_RS690_MC_INDEX_WR_EN); w(d, RLG_RS690_MC_DATA, v); break;
    case RLG_FAMILY_RS600: w(d, RLG_RS600_MC_INDEX, reg | RLG_RS600_MC_IND_WR_EN); w(d, RLG_RS600_MC_DATA, v); break;
    case RLG_FAMILY_R300_PCIE: w(d, RLG_PCIE_INDEX, reg); w(d, RLG_PCIE_DATA, v); break;
    default: w(d, RLG_RS480_NB_MC_INDEX, reg | RLG_RS480_NB_MC_IND_WR_EN); w(d, RLG_RS480_NB_MC_DATA, v); break;
    }
}

/* ---- tests -------------------------------------------------------------------- */

static void test_profiles(void)
{
    RLGProfile p = RLG_PROFILE_XPRESS_200;
    CHECK(rlg_profile_parse("rs690", &p) && p == RLG_PROFILE_X1250_AMD);
    CHECK(rlg_chip_info(p)->device_id == 0x791e && rlg_chip_info(p)->family == RLG_FAMILY_RS690);
    CHECK(rlg_profile_parse("xpress1250", &p) && p == RLG_PROFILE_X1250_AMD);
    CHECK(rlg_profile_parse("rs600", &p) && rlg_chip_info(p)->device_id == 0x7941);
    CHECK(!rlg_profile_parse("bogus", &p));
    CHECK(!rlg_chip_info(RLG_PROFILE_X1250_AMD)->hw_tcl);
    CHECK(rlg_profile_parse("x300", &p) && rlg_chip_info(p)->device_id == 0x5b60);
    CHECK(rlg_chip_info(p)->family == RLG_FAMILY_R300_PCIE && rlg_chip_info(p)->hw_tcl);
    CHECK(rlg_profile_parse("x700", &p) && rlg_chip_info(p)->device_id == 0x5e4d);
    CHECK(rlg_profile_parse("x700xt", &p) && rlg_chip_info(p)->device_id == 0x5e4a);
    CHECK(rlg_profile_parse("x700m", &p) && rlg_chip_info(p)->mobile && !rlg_chip_info(p)->integrated);
    CHECK(!rlg_family_is_avivo(RLG_FAMILY_R300_PCIE) && rlg_family_is_avivo(RLG_FAMILY_RS690));
}

static void test_mm_index_guard(void)
{
    RLGDevice *d = make(RLG_PROFILE_XPRESS_200);
    w(d, RLG_MM_INDEX, RLG_MM_DATA);                 /* used to recurse forever */
    CHECK(r(d, RLG_MM_DATA) == 0);
    w(d, RLG_MM_DATA, 0x1234);
    w(d, RLG_MM_INDEX, 0x80000010u);                 /* framebuffer path, byte access */
    rlg_mmio_write(d, RLG_MM_DATA + 1, 0xab, 1);
    CHECK(d->vram[0x11] == 0xab);
    CHECK(rlg_mmio_read(d, RLG_MM_DATA + 1, 1) == 0xab);
    w(d, RLG_MM_INDEX, RLG_DP_WRITE_MASK);           /* register path */
    w(d, RLG_MM_DATA, 0x00ff00ffu);
    CHECK(d->eng2d.write_mask == 0x00ff00ffu);
    rlg_destroy(d);
}

static void test_int_status_w1c(void)
{
    RLGDevice *d = make(RLG_PROFILE_XPRESS_200);
    w(d, RLG_GEN_INT_CNTL, RLG_INT_SW | RLG_INT_CRTC_VBLANK);
    w(d, RLG_GEN_INT_STATUS, RLG_INT_SW_FIRE);       /* fence interrupt */
    rlg_vblank(d);
    CHECK((r(d, RLG_GEN_INT_STATUS) & (RLG_INT_SW | RLG_INT_CRTC_VBLANK)) == (RLG_INT_SW | RLG_INT_CRTC_VBLANK));
    CHECK(irq_level);
    rlg_mmio_write(d, RLG_GEN_INT_STATUS, 0x01, 1);  /* ack vblank only, byte write */
    CHECK(r(d, RLG_GEN_INT_STATUS) & RLG_INT_SW);
    CHECK(!(r(d, RLG_GEN_INT_STATUS) & RLG_INT_CRTC_VBLANK));
    w(d, RLG_GEN_INT_STATUS, RLG_INT_SW);
    CHECK(!irq_level);
    rlg_destroy(d);
}

static void test_legacy_crtc(void)
{
    RLGDevice *d = make(RLG_PROFILE_XPRESS_200);
    RLGDisplayMode m;
    w(d, RLG_CRTC_H_TOTAL_DISP, (1024u / 8u - 1u) << 16);
    w(d, RLG_CRTC_V_TOTAL_DISP, (768u - 1u) << 16);
    w(d, RLG_CRTC_PITCH, 1024u / 8u);
    w(d, RLG_CRTC_OFFSET, 0x1000);
    w(d, RLG_CRTC_GEN_CNTL, RLG_CRTC_EXT_DISP_EN | RLG_CRTC_EN | RLG_CRTC_16BPP);
    m = rlg_get_display_mode(d);
    CHECK(m.enabled && !m.avivo && m.width == 1024 && m.height == 768 && m.bpp == 16);
    CHECK(m.pitch_bytes == 2048 && m.offset_bytes == 0x1000);
    w(d, RLG_CRTC_EXT_CNTL, RLG_CRTC_DISPLAY_DIS);
    CHECK(!rlg_get_display_mode(d).enabled);
    rlg_destroy(d);
}

static void test_2d(void)
{
    RLGDevice *d = make(RLG_PROFILE_XPRESS_200);
    const uint32_t fb = 0xe0000000u, pitch = 256;   /* 64 px * 4 bytes */
    uint32_t gmc = RLG_GMC_DST_PITCH_OFFSET_CNTL | RLG_GMC_SRC_PITCH_OFFSET_CNTL |
                   (RLG_DST_32BPP << RLG_GMC_DST_DATATYPE_SHIFT);

    w(d, RLG_MC_FB_LOCATION, (((fb + d->vram_size - 1u) >> 16) << 16) | (fb >> 16));
    w(d, RLG_DST_PITCH_OFFSET, ((pitch / 64u) << 22) | (fb >> 10));
    w(d, RLG_SRC_PITCH_OFFSET, ((pitch / 64u) << 22) | (fb >> 10));
    w(d, RLG_DP_GUI_MASTER_CNTL, gmc | RLG_ROP3_PATCOPY);
    w(d, RLG_DP_BRUSH_FRGD_CLR, 0x11223344);
    w(d, RLG_DP_CNTL, 3);
    w(d, RLG_DST_Y_X, (3u << 16) | 2u);              /* x=2, y=3 */
    w(d, RLG_DST_HEIGHT_WIDTH, (2u << 16) | 4u);     /* w=4, h=2 */
    CHECK(get32(d->vram + 3 * pitch + 2 * 4) == 0x11223344);
    CHECK(get32(d->vram + 4 * pitch + 5 * 4) == 0x11223344);
    CHECK(get32(d->vram + 5 * pitch + 2 * 4) == 0);
    CHECK(d->eng2d.fills == 1);

    /* scissor: left=3, top=0 -> column 2 must stay untouched */
    w(d, RLG_DP_BRUSH_FRGD_CLR, 0x55);
    w(d, RLG_SC_TOP_LEFT, (0u << 16) | 3u);
    w(d, RLG_DST_X_Y, (2u << 16) | 10u);             /* x=2, y=10 */
    w(d, RLG_DST_WIDTH_HEIGHT, (4u << 16) | 1u);     /* w=4, h=1 */
    CHECK(get32(d->vram + 10 * pitch + 2 * 4) == 0);
    CHECK(get32(d->vram + 10 * pitch + 3 * 4) == 0x55);
    w(d, RLG_SC_TOP_LEFT, 0);

    /* SRCCOPY via DST_WIDTH + DST_HEIGHT trigger */
    w(d, RLG_DP_GUI_MASTER_CNTL, gmc | RLG_ROP3_SRCCOPY);
    w(d, RLG_SRC_Y_X, (3u << 16) | 2u);
    w(d, RLG_DST_Y_X, (20u << 16) | 30u);
    w(d, RLG_DST_WIDTH, 1);
    w(d, RLG_DST_HEIGHT, 1);
    CHECK(get32(d->vram + 20 * pitch + 30 * 4) == 0x11223344);
    CHECK(d->eng2d.blits == 1);
    rlg_destroy(d);
}

/* RS690 with VRAM at 0xC0000000 and a 32 MiB GART at 0xE0000000. */
#define T_FB      0xc0000000u
#define T_GTT     0xe0000000u
#define T_TABLE   0x00100000u   /* sysmem */
#define T_PAGES   0x00200000u   /* sysmem backing for GART page N: T_PAGES + N*4K */

static void setup_rs690_mc(RLGDevice *d)
{
    mc_w(d, RLG_RS690_MCCFG_FB_LOCATION, (((T_FB + d->vram_size - 1u) >> 16) << 16) | (T_FB >> 16));
    mc_w(d, RLG_RS690_MCCFG_AGP_LOCATION, (((T_GTT + (32u << 20) - 1u) >> 16) << 16) | (T_GTT >> 16));
    mc_w(d, RLG_RS480_GART_BASE, T_TABLE);
    for (uint32_t i = 0; i < 16; ++i) {
        put32(sysmem + T_TABLE + i * 4, (T_PAGES + i * 4096u) | RLG_RS400_PTE_READABLE | RLG_RS400_PTE_WRITEABLE);
    }
    mc_w(d, RLG_RS480_AGP_ADDRESS_SPACE_SIZE, 1);   /* GART_EN, 32 MiB */
}

static void test_rs690_mc_gart(void)
{
    RLGDevice *d = make(RLG_PROFILE_X1250_AMD);
    uint32_t v = 0xcafef00d, back = 0;
    setup_rs690_mc(d);
    CHECK(d->gart.enabled && d->gart.aperture_base == T_GTT);
    w(d, RLG_RS690_MC_INDEX, RLG_RS690_MCCFG_FB_LOCATION);
    CHECK(r(d, RLG_RS690_MC_DATA) == d->mc_regs[RLG_RS690_MCCFG_FB_LOCATION]);
    CHECK(rlg_fb_start(d) == T_FB);

    CHECK(rlg_gpu_write(d, T_FB + 0x40, &v, 4) == 0 && get32(d->vram + 0x40) == v);
    CHECK(rlg_gpu_write(d, T_GTT + 4096 + 8, &v, 4) == 0);
    CHECK(get32(sysmem + T_PAGES + 4096 + 8) == v);
    CHECK(rlg_gpu_read(d, T_GTT + 4096 + 8, &back, 4) == 0 && back == v);
    /* write straddling two GART pages */
    uint8_t buf[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(rlg_gpu_write(d, T_GTT + 4096 - 4, buf, 8) == 0);
    CHECK(sysmem[T_PAGES + 4095] == 4 && sysmem[T_PAGES + 4096] == 5);
    /* PTE without R/W is a fault */
    put32(sysmem + T_TABLE + 5 * 4, T_PAGES);
    CHECK(rlg_gpu_read(d, T_GTT + 5 * 4096, &back, 4) != 0);
    /* MC data writes without WR_EN are ignored */
    w(d, RLG_RS690_MC_INDEX, RLG_RS480_GART_BASE);
    w(d, RLG_RS690_MC_DATA, 0xdead0000u);
    CHECK(d->mc_regs[RLG_RS480_GART_BASE] == T_TABLE);
    rlg_destroy(d);
}

static void ring_put(RLGDevice *d, uint32_t *wptr, uint32_t v)
{
    put32(d->vram + 0x10000 + (*wptr) * 4, v);    /* ring lives at T_FB + 64 KiB */
    (*wptr)++;
}

static void test_cp_ring_ib_fence(void)
{
    RLGDevice *d = make(RLG_PROFILE_X1250_AMD);
    uint32_t wp = 0, ib_gpu = T_GTT + 2 * 4096, ib_sys = T_PAGES + 2 * 4096;
    setup_rs690_mc(d);

    /* r100_cp_init-style setup: 4 KiB ring (bufsz 9 -> 1024 dwords) */
    w(d, RLG_CP_RB_CNTL, 9 | RLG_RB_NO_UPDATE | RLG_RB_RPTR_WR_ENA);
    w(d, RLG_CP_RB_BASE, T_FB + 0x10000);
    w(d, RLG_CP_RB_RPTR_WR, 0);
    w(d, RLG_CP_RB_WPTR, 0);
    w(d, RLG_CP_RB_RPTR_ADDR, T_GTT + 3 * 4096);
    w(d, RLG_SCRATCH_ADDR, T_GTT + 3 * 4096 + 0x100);
    w(d, RLG_SCRATCH_UMSK, 0xff);
    w(d, RLG_CP_RB_CNTL, 9);                         /* writeback on */
    w(d, RLG_GEN_INT_CNTL, RLG_INT_SW);

    /* ring test: PACKET0(SCRATCH_REG0) = 0xDEADBEEF, before the CP is enabled */
    ring_put(d, &wp, RLG_PM4_PACKET0(RLG_SCRATCH_REG0, 1));
    ring_put(d, &wp, 0xdeadbeefu);
    w(d, RLG_CP_RB_WPTR, wp);
    CHECK(r(d, RLG_SCRATCH_REG0) == 0);              /* CSQ disabled: nothing ran */
    w(d, RLG_CP_CSQ_CNTL, 4u << RLG_CSQ_MODE_SHIFT); /* PRIBM_INDBM */
    CHECK(r(d, RLG_SCRATCH_REG0) == 0xdeadbeefu);
    CHECK(d->cp.rptr == 2);
    CHECK(get32(sysmem + T_PAGES + 3 * 4096) == 2);  /* RPTR writeback */
    CHECK(get32(sysmem + T_PAGES + 3 * 4096 + 0x100) == 0xdeadbeefu); /* scratch writeback */

    /* IB with a ONE_REG_WR PACKET0 (two writes to the same reg) and a PACKET3 NOP */
    put32(sysmem + ib_sys + 0, RLG_PM4_PACKET0(RLG_SCRATCH_REG0 + 4, 2) | (1u << 15));
    put32(sysmem + ib_sys + 4, 111);
    put32(sysmem + ib_sys + 8, 222);
    put32(sysmem + ib_sys + 12, RLG_PM4_PACKET3(RLG_PM4_NOP, 2));
    put32(sysmem + ib_sys + 16, 0);
    put32(sysmem + ib_sys + 20, 0);
    ring_put(d, &wp, RLG_PM4_PACKET0(RLG_CP_IB_BASE, 2));
    ring_put(d, &wp, ib_gpu);
    ring_put(d, &wp, 6);
    /* fence */
    ring_put(d, &wp, RLG_PM4_PACKET0(RLG_SCRATCH_REG0 + 8, 1));
    ring_put(d, &wp, 42);
    ring_put(d, &wp, RLG_PM4_PACKET0(RLG_GEN_INT_STATUS, 1));
    ring_put(d, &wp, RLG_INT_SW_FIRE);
    ring_put(d, &wp, RLG_PM4_PACKET2);
    w(d, RLG_CP_RB_WPTR, wp);
    CHECK(r(d, RLG_SCRATCH_REG0 + 4) == 222);
    CHECK(r(d, RLG_SCRATCH_REG0 + 8) == 42);
    CHECK(d->cp.indirect_buffers == 1 && d->cp.faults == 0 && d->cp.unknown_packets == 0);
    CHECK(d->cp.rptr == wp);
    CHECK(irq_level && (r(d, RLG_GEN_INT_STATUS) & RLG_INT_SW));

    /* incomplete packet: only the header is visible */
    uint32_t before = d->cp.rptr;
    ring_put(d, &wp, RLG_PM4_PACKET0(RLG_SCRATCH_REG0 + 12, 2));
    w(d, RLG_CP_RB_WPTR, wp);
    CHECK(d->cp.rptr == before);
    ring_put(d, &wp, 7);
    ring_put(d, &wp, 8);
    w(d, RLG_CP_RB_WPTR, wp);
    CHECK(d->cp.rptr == wp && r(d, RLG_SCRATCH_REG0 + 16) == 8);
    rlg_destroy(d);
}

static void test_avivo(void)
{
    RLGDevice *d = make(RLG_PROFILE_X1250_AMD);
    RLGDisplayMode m;
    setup_rs690_mc(d);
    w(d, RLG_D1GRPH_CONTROL, 1 | (1u << 8));         /* 16 bpp RGB565 */
    w(d, RLG_D1GRPH_PRIMARY_SURFACE_ADDR, T_FB + 0x100000);
    w(d, RLG_D1GRPH_PITCH, 1280);
    w(d, RLG_D1GRPH_X_END, 1280);
    w(d, RLG_D1GRPH_Y_END, 1024);
    w(d, RLG_D1GRPH_ENABLE, 1);
    CHECK(!rlg_get_display_mode(d).enabled);         /* CRTC still off */
    w(d, RLG_D1CRTC_CONTROL, 1);
    m = rlg_get_display_mode(d);
    CHECK(m.enabled && m.avivo && m.width == 1280 && m.height == 1024 && m.bpp == 16);
    CHECK(m.pitch_bytes == 2560 && m.offset_bytes == 0x100000);
    CHECK(rlg_is_display_reg(d, RLG_D1GRPH_PITCH) && !rlg_is_display_reg(d, RLG_DST_Y_X));

    rlg_vblank(d);
    CHECK(!irq_level);                               /* masked */
    w(d, RLG_DXMODE_INT_MASK, RLG_D1MODE_VBLANK_INT_MASK);
    rlg_vblank(d);
    CHECK(irq_level && (r(d, RLG_DISP_INTERRUPT_STATUS) & RLG_LB_D1_VBLANK_INTERRUPT));
    w(d, RLG_D1MODE_VBLANK_STATUS, RLG_D1MODE_VBLANK_ACK);
    CHECK(!irq_level && !(r(d, RLG_DISP_INTERRUPT_STATUS) & RLG_LB_D1_VBLANK_INTERRUPT));
    CHECK(r(d, RLG_D1CRTC_STATUS_FRAME_COUNT) == 2);
    CHECK((r(d, RLG_D1CRTC_STATUS) & 1) != (r(d, RLG_D1CRTC_STATUS) & 1)); /* toggles */
    rlg_destroy(d);
}

static void test_rs600_gart(void)
{
    RLGDevice *d = make(RLG_PROFILE_XPRESS_1250_INTEL);
    uint32_t v = 0x600600u, table_off = 0x200000;
    uint64_t pte = (uint64_t)(T_PAGES + 4096u) | 1u;
    mc_w(d, RLG_RS600_MC_FB_LOCATION, (((T_FB + d->vram_size - 1u) >> 16) << 16) | (T_FB >> 16));
    memcpy(d->vram + table_off, &pte, 8);            /* page 0 -> sysmem T_PAGES + 4K */
    mc_w(d, RLG_RS600_MC_PT0_FLAT_BASE_ADDR, T_FB + table_off);
    mc_w(d, RLG_RS600_MC_PT0_FLAT_START_ADDR, T_GTT);
    mc_w(d, RLG_RS600_MC_PT0_FLAT_END_ADDR, T_GTT + (32u << 20) - 1u);
    mc_w(d, RLG_RS600_MC_PT0_CNTL, 1);
    CHECK(!d->gart.enabled);
    mc_w(d, RLG_RS600_MC_CNTL1, 1u << 26);
    CHECK(d->gart.enabled);
    CHECK(rlg_gpu_write(d, T_GTT + 0x20, &v, 4) == 0 && get32(sysmem + T_PAGES + 4096 + 0x20) == v);
    CHECK(rlg_gpu_write(d, T_GTT + 4096, &v, 4) != 0);  /* page 1: PTE not valid */
    rlg_destroy(d);
}

/* rv370_pcie_gart_get_page_entry() */
static uint32_t pcie_pte(uint64_t addr)
{
    return (uint32_t)((addr & 0xffffffffu) >> 8) | (uint32_t)(((addr >> 32) & 0xffu) << 24) |
           RLG_R300_PTE_READABLE | RLG_R300_PTE_WRITEABLE;
}

/* X700 (RV410): VRAM at 0xD0000000, PCIe GART at 0xE0000000, table in VRAM. */
static void test_x700_pcie(void)
{
    RLGDevice *d = make(RLG_PROFILE_X700);
    const uint32_t fb = 0xd0000000u, table_off = 0x800000u, ib_sys = T_PAGES + 4096u;
    uint64_t phys = 0;
    uint32_t v = 0x700700u, wp = 0;
    RLGDisplayMode m;

    CHECK(d->chip->hw_tcl && d->vram_size == (16u << 20));
    w(d, RLG_MC_FB_LOCATION, (((fb + d->vram_size - 1u) >> 16) << 16) | (fb >> 16));

    /* rv370_pcie_gart_enable() sequence */
    for (uint32_t i = 0; i < 16; ++i) {
        put32(d->vram + table_off + i * 4, pcie_pte(T_PAGES + i * 4096u));
    }
    put32(d->vram + table_off + 15 * 4, pcie_pte(0x1234567000ull));   /* > 4 GiB page */
    mc_w(d, RLG_PCIE_TX_GART_CNTL, 3u << 1);                           /* discard, not enabled */
    mc_w(d, RLG_PCIE_TX_GART_START_LO, T_GTT);
    mc_w(d, RLG_PCIE_TX_GART_END_LO, T_GTT + (32u << 20) - 4096u);
    mc_w(d, RLG_PCIE_TX_GART_BASE, fb + table_off);
    CHECK(!d->gart.enabled);
    mc_w(d, RLG_PCIE_TX_GART_CNTL, (3u << 1) | RLG_PCIE_TX_GART_EN);
    CHECK(d->gart.enabled && d->gart.aperture_base == T_GTT && d->gart.aperture_size == (32u << 20));
    w(d, RLG_PCIE_INDEX, RLG_PCIE_TX_GART_BASE);
    CHECK(r(d, RLG_PCIE_DATA) == fb + table_off);                      /* 0x30/0x34 = PCIe, not BUS_CNTL */

    CHECK(rlg_gpu_write(d, T_GTT + 0x10, &v, 4) == 0 && get32(sysmem + T_PAGES + 0x10) == v);
    CHECK(rlg_gart_translate(d, T_GTT + 15 * 4096u + 0x44, &phys) == 0 && phys == 0x1234567044ull);
    CHECK(rlg_gart_translate(d, T_GTT + (32u << 20), &phys) != 0);     /* past the last page */

    /* CP: ring in VRAM, IB in GART, fence IRQ through GEN_INT_CNTL */
    w(d, RLG_CP_RB_CNTL, 9 | RLG_RB_NO_UPDATE);
    w(d, RLG_CP_RB_BASE, fb + 0x10000);
    w(d, RLG_CP_CSQ_CNTL, 4u << RLG_CSQ_MODE_SHIFT);
    w(d, RLG_GEN_INT_CNTL, RLG_INT_SW);
    put32(sysmem + ib_sys, RLG_PM4_PACKET0(RLG_SCRATCH_REG0, 1));
    put32(sysmem + ib_sys + 4, 0x7007);
    ring_put(d, &wp, RLG_PM4_PACKET0(RLG_CP_IB_BASE, 2));
    ring_put(d, &wp, T_GTT + 4096u);
    ring_put(d, &wp, 2);
    ring_put(d, &wp, RLG_PM4_PACKET0(RLG_GEN_INT_STATUS, 1));
    ring_put(d, &wp, RLG_INT_SW_FIRE);
    w(d, RLG_CP_RB_WPTR, wp);
    CHECK(r(d, RLG_SCRATCH_REG0) == 0x7007 && d->cp.faults == 0 && irq_level);

    /* legacy CRTC scanout, with the framebuffer offset relative to the aperture */
    w(d, RLG_CRTC_H_TOTAL_DISP, (800u / 8u - 1u) << 16);
    w(d, RLG_CRTC_V_TOTAL_DISP, (600u - 1u) << 16);
    w(d, RLG_CRTC_PITCH, 800u / 8u);
    w(d, RLG_CRTC_GEN_CNTL, RLG_CRTC_EXT_DISP_EN | RLG_CRTC_EN | RLG_CRTC_32BPP);
    m = rlg_get_display_mode(d);
    CHECK(m.enabled && !m.avivo && m.width == 800 && m.pitch_bytes == 3200);
    CHECK(rlg_is_display_reg(d, RLG_CRTC_PITCH) && !rlg_is_display_reg(d, RLG_D1GRPH_PITCH));
    w(d, RLG_D1GRPH_ENABLE, 1);                                        /* no AVIVO on RV410 */
    w(d, RLG_D1CRTC_CONTROL, 1);
    CHECK(!rlg_get_display_mode(d).avivo);
    rlg_destroy(d);
}

int main(void)
{
    sysmem = calloc(1, SYSMEM_SIZE);
    if (!sysmem) {
        return 1;
    }
    test_profiles();
    test_mm_index_guard();
    test_int_status_w1c();
    test_legacy_crtc();
    test_2d();
    test_rs690_mc_gart();
    test_cp_ring_ib_fence();
    test_avivo();
    test_rs600_gart();
    test_x700_pcie();
    free(sysmem);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("all core tests passed");
    return 0;
}
