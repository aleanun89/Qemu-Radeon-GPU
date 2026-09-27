#pragma once
#include <stdint.h>

/*
 * Focused subset of the public classic Radeon register interface.
 * Offsets and bit positions cross-checked against Linux
 * drivers/gpu/drm/radeon/{radeon_reg.h,r500_reg.h,r100d.h,rs600d.h,rs690d.h}.
 */

/* ---- MMIO: bus / config -------------------------------------------- */
#define RLG_MM_INDEX                    0x0000u
#define RLG_MM_DATA                     0x0004u
#define RLG_CLOCK_CNTL_INDEX            0x0008u
#define RLG_CLOCK_CNTL_DATA             0x000cu
#define RLG_BIOS_0_SCRATCH              0x0010u
#define RLG_BUS_CNTL                    0x0030u  /* AGP/PCI parts only */
#define RLG_PCIE_INDEX                  0x0030u  /* PCIe parts (RV370/RV380/RV410): indirect index */
#define RLG_PCIE_DATA                   0x0034u
#define RLG_GEN_INT_CNTL                0x0040u
#define RLG_GEN_INT_STATUS              0x0044u
#define RLG_CRTC_GEN_CNTL               0x0050u
#define RLG_CRTC_EXT_CNTL               0x0054u
#define RLG_DAC_CNTL                    0x0058u
#define RLG_RS600_MC_INDEX              0x0070u  /* RS600: MC indirect index */
#define RLG_RS600_MC_DATA               0x0074u
#define RLG_RS690_MC_INDEX              0x0078u  /* RS690/RS740: MC indirect index */
#define RLG_RS690_MC_DATA               0x007cu
#define RLG_CNFG_CNTL                   0x00e0u
#define RLG_GEN_RESET_CNTL              0x00f0u  /* RBBM_SOFT_RESET */
#define RLG_CNFG_MEMSIZE                0x00f8u
#define RLG_CONFIG_APER_0_BASE          0x0100u
#define RLG_CONFIG_APER_SIZE            0x0108u
#define RLG_CONFIG_REG_1_BASE           0x010cu
#define RLG_CONFIG_REG_APER_SIZE        0x0110u
#define RLG_HOST_PATH_CNTL              0x0130u
#define RLG_MEM_CNTL                    0x0140u
#define RLG_MC_FB_LOCATION              0x0148u  /* RS400/RS480 */
#define RLG_MC_AGP_LOCATION             0x014cu  /* RS400/RS480 */
#define RLG_MC_STATUS                   0x0150u
#define RLG_RS480_NB_MC_INDEX           0x0168u  /* RS400/RS480: MC indirect index */
#define RLG_RS480_NB_MC_DATA            0x016cu
#define RLG_AGP_BASE                    0x0170u
#define RLG_AGP_CNTL                    0x0174u
#define RLG_AGP_APER_OFFSET             0x0178u
#define RLG_PCI_GART_PAGE               0x017cu

/* ---- MMIO: legacy CRTC (RS400/RS480; also used by the VGA BIOS) ------ */
#define RLG_CRTC_H_TOTAL_DISP           0x0200u
#define RLG_CRTC_H_SYNC_STRT_WID        0x0204u
#define RLG_CRTC_V_TOTAL_DISP           0x0208u
#define RLG_CRTC_V_SYNC_STRT_WID        0x020cu
#define RLG_CRTC_VLINE_CRNT_VLINE       0x0210u
#define RLG_CRTC_OFFSET                 0x0224u
#define RLG_CRTC_OFFSET_CNTL            0x0228u
#define RLG_CRTC_PITCH                  0x022cu  /* units of 8 pixels */

/* ---- MMIO: command processor ---------------------------------------- */
#define RLG_CP_RB_BASE                  0x0700u
#define RLG_CP_RB_CNTL                  0x0704u
#define RLG_CP_RB_RPTR_ADDR             0x070cu
#define RLG_CP_RB_RPTR                  0x0710u
#define RLG_CP_RB_WPTR                  0x0714u
#define RLG_CP_RB_WPTR_DELAY            0x0718u
#define RLG_CP_RB_RPTR_WR               0x071cu
#define RLG_CP_IB_BASE                  0x0738u
#define RLG_CP_IB_BUFSZ                 0x073cu  /* write triggers IB execution */
#define RLG_CP_CSQ_CNTL                 0x0740u
#define RLG_CP_CSQ_MODE                 0x0744u
#define RLG_SCRATCH_UMSK                0x0770u
#define RLG_SCRATCH_ADDR                0x0774u
#define RLG_CP_CSQ_STAT                 0x07f8u
#define RLG_CP_ME_CNTL                  0x07fcu
#define RLG_RBBM_STATUS                 0x0e40u

/* ---- MMIO: 2D engine ------------------------------------------------ */
#define RLG_DST_OFFSET                  0x1404u
#define RLG_DST_PITCH                   0x1408u
#define RLG_DST_WIDTH                   0x140cu
#define RLG_DST_HEIGHT                  0x1410u  /* write triggers blit */
#define RLG_SRC_X                       0x1414u
#define RLG_SRC_Y                       0x1418u
#define RLG_DST_X                       0x141cu
#define RLG_DST_Y                       0x1420u
#define RLG_SRC_PITCH_OFFSET            0x1428u
#define RLG_DST_PITCH_OFFSET            0x142cu
#define RLG_SRC_Y_X                     0x1434u  /* X in 15:0, Y in 31:16 */
#define RLG_DST_Y_X                     0x1438u  /* X in 15:0, Y in 31:16 */
#define RLG_DST_HEIGHT_WIDTH            0x143cu  /* W in 15:0, H in 31:16; triggers */
#define RLG_DP_GUI_MASTER_CNTL          0x146cu
#define RLG_DP_BRUSH_BKGD_CLR           0x1478u
#define RLG_DP_BRUSH_FRGD_CLR           0x147cu
#define RLG_SRC_X_Y                     0x1590u  /* Y in 15:0, X in 31:16 */
#define RLG_DST_X_Y                     0x1594u  /* Y in 15:0, X in 31:16 */
#define RLG_DST_WIDTH_HEIGHT            0x1598u  /* H in 15:0, W in 31:16; triggers */
#define RLG_SRC_OFFSET                  0x15acu
#define RLG_SRC_PITCH                   0x15b0u
#define RLG_DP_SRC_FRGD_CLR             0x15d8u
#define RLG_DP_SRC_BKGD_CLR             0x15dcu
#define RLG_SCRATCH_REG0                0x15e0u  /* SCRATCH_REG0..7 */
#define RLG_SC_LEFT                     0x1640u
#define RLG_SC_RIGHT                    0x1644u
#define RLG_SC_TOP                      0x1648u
#define RLG_SC_BOTTOM                   0x164cu
#define RLG_DP_CNTL                     0x16c0u
#define RLG_DP_DATATYPE                 0x16c4u
#define RLG_DP_MIX                      0x16c8u
#define RLG_DP_WRITE_MASK               0x16ccu
#define RLG_DEFAULT_PITCH_OFFSET        0x16e0u  /* same layout as DST_PITCH_OFFSET */
#define RLG_DEFAULT_SC_BOTTOM_RIGHT     0x16e8u
#define RLG_SC_TOP_LEFT                 0x16ecu  /* left 13:0, top 29:16 */
#define RLG_SC_BOTTOM_RIGHT             0x16f0u  /* right 13:0, bottom 29:16 */
#define RLG_WAIT_UNTIL                  0x1720u
#define RLG_GUI_STAT                    0x1740u

/* ---- MMIO: AVIVO display (RS600/RS690) ------------------------------ */
#define RLG_AVIVO_BASE                  0x6000u
#define RLG_AVIVO_END                   0x8000u
#define RLG_D1CRTC_H_TOTAL              0x6000u
#define RLG_D1CRTC_V_TOTAL              0x6020u
#define RLG_D1CRTC_CONTROL              0x6080u  /* bit0 master enable */
#define RLG_D1CRTC_STATUS               0x609cu
#define RLG_D1CRTC_STATUS_FRAME_COUNT   0x60a4u
#define RLG_D1GRPH_ENABLE               0x6100u
#define RLG_D1GRPH_CONTROL              0x6104u  /* 1:0 depth, 10:8 format */
#define RLG_D1GRPH_PRIMARY_SURFACE_ADDR 0x6110u
#define RLG_D1GRPH_PITCH                0x6120u  /* pixels */
#define RLG_D1GRPH_SURFACE_OFFSET_X     0x6124u
#define RLG_D1GRPH_SURFACE_OFFSET_Y     0x6128u
#define RLG_D1GRPH_X_START              0x612cu
#define RLG_D1GRPH_Y_START              0x6130u
#define RLG_D1GRPH_X_END                0x6134u
#define RLG_D1GRPH_Y_END                0x6138u
#define RLG_D1GRPH_UPDATE               0x6144u
#define RLG_D1MODE_VBLANK_STATUS        0x6534u
#define RLG_DXMODE_INT_MASK             0x6540u
#define RLG_D1MODE_VIEWPORT_START       0x6580u
#define RLG_D1MODE_VIEWPORT_SIZE        0x6584u
#define RLG_DISP_INTERRUPT_STATUS       0x7edcu

/* ---- MC indirect space --------------------------------------------- */
#define RLG_RS480_NB_MC_IND_WR_EN       (1u << 8)
#define RLG_RS480_MC_MISC_CNTL          0x18u
#define RLG_RS480_GART_FEATURE_ID       0x2bu
#define RLG_RS480_GART_BASE             0x2cu
#define RLG_RS480_GART_CACHE_CNTRL      0x2eu
#define RLG_RS480_AGP_ADDRESS_SPACE_SIZE 0x38u   /* bit0 GART_EN, 3:1 VA size */
#define RLG_RS690_MC_INDEX_MASK         0x1ffu
#define RLG_RS690_MC_INDEX_WR_EN        (1u << 9)
#define RLG_RS690_MC_SYSTEM_STATUS      0x90u    /* bit0 idle */
#define RLG_RS690_MCCFG_FB_LOCATION     0x100u
#define RLG_RS690_MCCFG_AGP_LOCATION    0x101u
#define RLG_RS690_MCCFG_AGP_BASE        0x102u
#define RLG_RS600_MC_ADDR_MASK          0xffffu
#define RLG_RS600_MC_IND_WR_EN          (1u << 23)
#define RLG_RS600_MC_STATUS             0x000u
#define RLG_RS600_MC_FB_LOCATION        0x004u
#define RLG_RS600_MC_AGP_LOCATION       0x005u
#define RLG_RS600_MC_CNTL1              0x009u   /* bit26 ENABLE_PAGE_TABLES */
#define RLG_RS600_MC_PT0_CNTL           0x100u   /* bit0 ENABLE_PT */
#define RLG_RS600_MC_PT0_FLAT_BASE_ADDR 0x12cu
#define RLG_RS600_MC_PT0_FLAT_START_ADDR 0x13cu
#define RLG_RS600_MC_PT0_FLAT_END_ADDR  0x14cu
#define RLG_MC_REGS                     0x200u   /* size of emulated MC space */

/* ---- PCIe indirect space (RV370/RV380/RV410; Linux r300.c) ----------- */
#define RLG_PCIE_REG_MASK               0xffu
#define RLG_PCIE_TX_GART_CNTL           0x10u    /* bit0 GART_EN, bit8 INVALIDATE_TLB */
#define RLG_PCIE_TX_GART_EN             (1u << 0)
#define RLG_PCIE_TX_GART_BASE           0x13u    /* MC address of the table (in VRAM) */
#define RLG_PCIE_TX_GART_START_LO       0x14u
#define RLG_PCIE_TX_GART_START_HI       0x15u
#define RLG_PCIE_TX_GART_END_LO         0x16u    /* start of the last page */
#define RLG_PCIE_TX_GART_END_HI         0x17u
#define RLG_PCIE_TX_GART_ERROR          0x18u

/* ---- bits ------------------------------------------------------------ */
#define RLG_INT_CRTC_VBLANK             (1u << 0)   /* R100: CRTC_VBLANK_STAT; RS600+: DISPLAY_INT_STAT */
#define RLG_INT_GUI_IDLE                (1u << 19)
#define RLG_INT_SW                      (1u << 25)  /* SW_INT_TEST / SW_INT_ENABLE */
#define RLG_INT_SW_FIRE                 (1u << 26)  /* write-only trigger in GEN_INT_STATUS */
#define RLG_SOFT_RESET_CP               (1u << 0)

#define RLG_RB_BUFSZ_MASK               0x3fu
#define RLG_RB_NO_UPDATE                (1u << 27)
#define RLG_RB_RPTR_WR_ENA              (1u << 31)
#define RLG_CSQ_MODE_SHIFT              28
#define RLG_RBBM_ACTIVE                 (1u << 31)

#define RLG_CRTC_DISPLAY_DIS            (1u << 10)  /* in CRTC_EXT_CNTL */
#define RLG_CRTC_EXT_DISP_EN            (1u << 24)  /* in CRTC_GEN_CNTL */
#define RLG_CRTC_EN                     (1u << 25)
#define RLG_CRTC_PIX_WIDTH_MASK         0x00000700u
#define RLG_CRTC_8BPP                   0x00000200u
#define RLG_CRTC_15BPP                  0x00000300u
#define RLG_CRTC_16BPP                  0x00000400u
#define RLG_CRTC_24BPP                  0x00000500u
#define RLG_CRTC_32BPP                  0x00000600u

#define RLG_D1MODE_VBLANK_OCCURRED      (1u << 0)
#define RLG_D1MODE_VBLANK_ACK           (1u << 4)
#define RLG_D1MODE_VBLANK_STAT          (1u << 12)
#define RLG_D1MODE_VBLANK_INTERRUPT     (1u << 16)
#define RLG_D1MODE_VBLANK_INT_MASK      (1u << 0)
#define RLG_LB_D1_VBLANK_INTERRUPT      (1u << 4)

#define RLG_DST_8BPP                    0x2u
#define RLG_DST_15BPP                   0x3u
#define RLG_DST_16BPP                   0x4u
#define RLG_DST_24BPP                   0x5u
#define RLG_DST_32BPP                   0x6u
#define RLG_GMC_SRC_PITCH_OFFSET_CNTL   (1u << 0)
#define RLG_GMC_DST_PITCH_OFFSET_CNTL   (1u << 1)
#define RLG_GMC_DST_DATATYPE_SHIFT      8
#define RLG_GMC_DST_DATATYPE_MASK       (0xfu << 8)
#define RLG_GMC_ROP3_MASK               0x00ff0000u
#define RLG_ROP3_BLACKNESS              0x00000000u
#define RLG_ROP3_SRCCOPY                0x00cc0000u
#define RLG_ROP3_PATCOPY                0x00f00000u
#define RLG_ROP3_WHITENESS              0x00ff0000u
#define RLG_DST_X_LEFT_TO_RIGHT         0x00000001u
#define RLG_DST_Y_TOP_TO_BOTTOM         0x00000002u

/* RS400-style GART PTE (32-bit) */
#define RLG_RS400_PTE_UNSNOOPED         (1u << 0)
#define RLG_RS400_PTE_WRITEABLE         (1u << 2)
#define RLG_RS400_PTE_READABLE          (1u << 3)
/* RV370 PCIe GART PTE (32-bit): addr[31:12] in 23:4, addr[39:32] in 31:24 */
#define RLG_R300_PTE_UNSNOOPED          (1u << 0)
#define RLG_R300_PTE_WRITEABLE          (1u << 2)
#define RLG_R300_PTE_READABLE           (1u << 3)
/* RS600 flat page table PTE (64-bit) */
#define RLG_RS600_PTE_VALID             (1ull << 0)

/* ---- PM4 ------------------------------------------------------------ */
#define RLG_PM4_TYPE(h)                 (((h) >> 30) & 3u)
#define RLG_PM4_COUNT(h)                ((((h) >> 16) & 0x3fffu) + 1u)
#define RLG_PM4_PKT0_REG(h)             (((h) & 0x1fffu) << 2)
#define RLG_PM4_PKT0_ONE_REG_WR(h)      (((h) >> 15) & 1u)
#define RLG_PM4_PKT1_REG0(h)            (((h) & 0x7ffu) << 2)
#define RLG_PM4_PKT1_REG1(h)            ((((h) >> 11) & 0x7ffu) << 2)
#define RLG_PM4_PKT3_OPCODE(h)          (((h) >> 8) & 0xffu)
#define RLG_PM4_PACKET0(reg, n)         ((((uint32_t)(reg) >> 2) & 0x1fffu) | ((uint32_t)((n) - 1u) << 16))
#define RLG_PM4_PACKET2                 0x80000000u
#define RLG_PM4_PACKET3(op, n)          (0xc0000000u | ((uint32_t)(op) << 8) | ((uint32_t)((n) - 1u) << 16))

#define RLG_PM4_NOP                     0x10u
#define RLG_PM4_WAIT_FOR_IDLE           0x26u
#define RLG_PM4_3D_DRAW_VBUF            0x28u
#define RLG_PM4_3D_DRAW_IMMD            0x29u
#define RLG_PM4_3D_DRAW_INDX            0x2au
#define RLG_PM4_3D_LOAD_VBPNTR          0x2fu
#define RLG_PM4_3D_CLEAR_ZMASK          0x32u
#define RLG_PM4_INDX_BUFFER             0x33u
#define RLG_PM4_3D_DRAW_VBUF_2          0x34u
#define RLG_PM4_3D_DRAW_IMMD_2          0x35u
#define RLG_PM4_3D_DRAW_INDX_2          0x36u
