#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "radeon_regs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RLG_VENDOR_ATI                  0x1002u
#define RLG_DEVICE_RS480                0x5954u
#define RLG_DEVICE_RS480M               0x5955u
#define RLG_DEVICE_RS600                0x7941u
#define RLG_DEVICE_RS600M               0x7942u
#define RLG_DEVICE_RS690                0x791eu
#define RLG_DEVICE_RS690M               0x791fu
#define RLG_DEVICE_X300                 0x5b60u  /* RV370 */
#define RLG_DEVICE_X300M                0x5460u  /* RV370/M22 */
#define RLG_DEVICE_X300SE               0x5b62u  /* RV370, sold as X300 SE / X600 SE */
#define RLG_DEVICE_X700                 0x5e4du  /* RV410 */
#define RLG_DEVICE_X700PRO              0x5e4bu
#define RLG_DEVICE_X700XT               0x5e4au
#define RLG_DEVICE_X700M                0x5652u  /* RV410/M26 */
#define RLG_DEVICE_R9550                0x4153u  /* RV350, AGP */
#define RLG_DEVICE_X600XT               0x3e50u  /* RV380, PCIe */
#define RLG_DEVICE_X550XTX              0x5657u  /* RV410, PCIe */
#define RLG_DEVICE_X850XT               0x5d52u  /* R480, PCIe */
#define RLG_DEVICE_X850XT_AGP           0x4b49u  /* R481, AGP */
#define RLG_MMIO_SIZE                   0x10000u
#define RLG_PAGE_SIZE                   4096u
#define RLG_NUM_SCRATCH                 8u

typedef enum {
    RLG_PROFILE_XPRESS_200 = 0,     /* RS480, AMD K8 platform */
    RLG_PROFILE_XPRESS_200M,
    RLG_PROFILE_XPRESS_1250_INTEL,  /* RS600, Intel platform */
    RLG_PROFILE_XPRESS_1250M_INTEL,
    RLG_PROFILE_X1250_AMD,          /* RS690 / AMD 690G, AMD K8 platform */
    RLG_PROFILE_X1250M_AMD,         /* RS690M / AMD M690 */
    RLG_PROFILE_X300,               /* RV370, discrete PCIe */
    RLG_PROFILE_X300M,
    RLG_PROFILE_X700,               /* RV410, discrete PCIe */
    RLG_PROFILE_X700PRO,
    RLG_PROFILE_X700XT,
    RLG_PROFILE_X700M,
    RLG_PROFILE_R9550,              /* RV350, discrete AGP */
    RLG_PROFILE_X600XT,             /* RV380, discrete PCIe */
    RLG_PROFILE_X550XTX,            /* RV410, discrete PCIe */
    RLG_PROFILE_X850XT,             /* R480, discrete PCIe */
    RLG_PROFILE_X850XT_AGP,         /* R481, discrete AGP */
    RLG_PROFILE_X300SE,             /* RV370, discrete PCIe */
    RLG_PROFILE_COUNT
} RLGProfile;

/* Register-interface family: decides MC access, GART format and display. */
typedef enum {
    RLG_FAMILY_RS400 = 0,   /* legacy CRTC, MC_FB_LOCATION @0x148, RS400 GART */
    RLG_FAMILY_RS600,       /* AVIVO, MC indirect @0x70, flat page table GART */
    RLG_FAMILY_RS690,       /* AVIVO, MC indirect @0x78, RS400 GART */
    RLG_FAMILY_R300,        /* discrete R3xx/R4xx: legacy CRTC, PCIe GART (@0x30 indirect)
                               or R100 PCI GART (AIC_*), whichever the driver enables */
} RLGFamily;

/* Bus of the real card. QEMU has no AGP: AGP parts are exposed as plain PCI and
 * the radeon driver then falls back to the R100 PCI GART. */
typedef enum {
    RLG_BUS_IGP = 0,
    RLG_BUS_AGP,
    RLG_BUS_PCIE,
} RLGBus;

static inline bool rlg_family_is_avivo(RLGFamily f)
{
    return f == RLG_FAMILY_RS600 || f == RLG_FAMILY_RS690;
}

typedef enum {
    RLG_BACKEND_SOFTWARE = 0,
    RLG_BACKEND_VULKAN = 1,
} RLGBackend;

typedef int (*RLGDMARead)(void *opaque, uint64_t guest_phys, void *buf, size_t len);
typedef int (*RLGDMAWrite)(void *opaque, uint64_t guest_phys, const void *buf, size_t len);
typedef void (*RLGIRQFn)(void *opaque, bool level);
typedef void (*RLGDirtyFn)(void *opaque, uint32_t vram_offset, uint32_t length);
typedef void (*RLGLogFn)(void *opaque, const char *message);

typedef struct {
    RLGProfile profile;
    RLGFamily family;
    const char *name;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t revision;
    uint32_t default_vram_mb;
    uint32_t nominal_core_mhz;
    const char *ps_profile;     /* D3D pixel shader profile exposed by the driver */
    bool hw_tcl;                /* false: vertex shading runs on the CPU (driver) */
    uint8_t pixel_pipes;        /* nominal */
    RLGBus bus;
    bool r4xx;                  /* R420/RV410 class: exposes R400_GB_PIPE_SELECT */
    bool mobile;
} RLGChipInfo;

typedef struct {
    RLGProfile profile;
    RLGBackend backend;
    uint32_t vram_size;
    bool verbose;
    void *opaque;
    RLGDMARead dma_read;
    RLGDMAWrite dma_write;
    RLGIRQFn irq;
    RLGDirtyFn dirty;
    RLGLogFn log;
} RLGConfig;

typedef struct {
    uint32_t base;
    uint32_t cntl;
    uint32_t rptr_addr;
    uint32_t rptr;
    uint32_t wptr;
    uint32_t csq_cntl;
    uint32_t ib_base;
    uint32_t scratch_umsk;
    uint32_t scratch_addr;
    unsigned depth;             /* nesting guard: ring -> IB */
    bool busy;
    uint64_t packets;
    uint64_t dwords;
    uint64_t draw_calls;
    uint64_t indirect_buffers;
    uint64_t unknown_packets;
    uint64_t faults;
} RLGCPState;

typedef enum {
    RLG_GART_NONE = 0,
    RLG_GART_RS400,     /* RS480/RS690 IGP GART, table in system memory */
    RLG_GART_RS600,     /* flat 64-bit table in VRAM */
    RLG_GART_PCIE,      /* RV370 PCIe GART, table in VRAM */
    RLG_GART_PCI,       /* R100 PCI GART (AIC_*), table in system memory */
} RLGGARTKind;

typedef struct {
    RLGGARTKind kind;
    uint64_t table_base;
    uint64_t aperture_base;
    uint64_t aperture_size;
    bool enabled;
    uint64_t translations;
    uint64_t faults;
} RLGGARTState;

typedef struct {
    uint32_t crtc_gen_cntl;
    uint32_t crtc_ext_cntl;
    uint32_t h_total_disp;
    uint32_t h_sync;
    uint32_t v_total_disp;
    uint32_t v_sync;
    uint32_t offset;
    uint32_t offset_cntl;
    uint32_t pitch;
} RLGDisplayRegs;

typedef struct {
    bool enabled;
    bool avivo;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t pitch_bytes;
    uint32_t offset_bytes;      /* offset inside VRAM */
    uint32_t refresh_hz;
} RLGDisplayMode;

typedef struct {
    uint32_t dst_offset, dst_pitch;
    uint32_t src_offset, src_pitch;
    uint32_t default_offset, default_pitch;
    uint32_t src_x, src_y, dst_x, dst_y;
    uint32_t dst_width, dst_height;
    uint32_t gui_master_cntl;
    uint32_t brush_fg, brush_bg;
    uint32_t src_fg, src_bg;
    uint32_t dp_cntl, dp_datatype, dp_mix, write_mask;
    uint32_t sc_left, sc_right, sc_top, sc_bottom;
    uint64_t fills, blits, rejected;
} RLG2DState;

typedef struct RLGHost RLGHost;

typedef struct RLGDevice {
    RLGConfig cfg;
    const RLGChipInfo *chip;
    uint8_t *vram;
    uint32_t vram_size;
    bool owns_vram;
    uint32_t *regs;             /* backing store for plain registers */
    uint32_t mc_regs[RLG_MC_REGS];
    uint32_t mc_index;
    uint32_t mm_index;
    uint32_t int_cntl;
    uint32_t int_status;
    uint32_t disp_int_status;
    uint32_t d1_vblank_status;
    uint32_t dxmode_int_mask;
    uint32_t d1_frame_count;
    bool irq_level;
    RLGDisplayRegs display;
    RLG2DState eng2d;
    RLGCPState cp;
    RLGGARTState gart;
    struct RLG3D *r3d;          /* 3D engine state (radeon_r3d.h) */
    RLGHost *host;
} RLGDevice;

const RLGChipInfo *rlg_chip_info(RLGProfile profile);
/* Returns false and leaves *out untouched for an unknown name. */
bool rlg_profile_parse(const char *name, RLGProfile *out);
RLGProfile rlg_profile_from_name(const char *name);
/* Human-readable list of the canonical model= names. */
const char *rlg_profile_models(void);
const char *rlg_profile_name(RLGProfile profile);

RLGDevice *rlg_create(const RLGConfig *cfg);
RLGDevice *rlg_create_with_vram(const RLGConfig *cfg, uint8_t *vram, uint32_t size);
void rlg_destroy(RLGDevice *dev);
void rlg_reset(RLGDevice *dev);

uint64_t rlg_mmio_read(RLGDevice *dev, uint32_t addr, unsigned size);
void rlg_mmio_write(RLGDevice *dev, uint32_t addr, uint64_t value, unsigned size);

/* GPU (MC) address space: VRAM aperture per MC FB location, else GART. */
bool rlg_mc_to_vram(const RLGDevice *dev, uint64_t mc_addr, uint64_t len, uint32_t *vram_off);
uint64_t rlg_fb_start(const RLGDevice *dev);
int rlg_gpu_read(RLGDevice *dev, uint64_t gpu_addr, void *buf, size_t len);
int rlg_gpu_write(RLGDevice *dev, uint64_t gpu_addr, const void *buf, size_t len);
int rlg_gart_translate(RLGDevice *dev, uint64_t gpu_addr, uint64_t *guest_phys);

int rlg_cp_process(RLGDevice *dev);
int rlg_2d_execute(RLGDevice *dev);
void rlg_vblank(RLGDevice *dev);
RLGDisplayMode rlg_get_display_mode(const RLGDevice *dev);
/* True when a register write at this MMIO offset may change the scanout. */
bool rlg_is_display_reg(const RLGDevice *dev, uint32_t addr);

int rlg_host_init(RLGDevice *dev);
void rlg_host_destroy(RLGDevice *dev);
bool rlg_host_is_active(const RLGDevice *dev);

uint32_t rlg_reg_read32(const RLGDevice *dev, uint32_t addr);
void rlg_reg_write32(RLGDevice *dev, uint32_t addr, uint32_t value);

#ifdef __cplusplus
}
#endif
