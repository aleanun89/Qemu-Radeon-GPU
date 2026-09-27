/* SPDX-License-Identifier: GPL-2.0-or-later
 * QEMU PCI adapter for radeon-legacy-vgpu. Target: QEMU 11.1.x
 * (API usage mirrors hw/display/ati.c of that release).
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pcie.h"
#include "hw/display/vga.h"
#include "vga_int.h"
#include "ui/console.h"
#include "system/dma.h"
#include "qom/object.h"
#include "radeon_legacy/radeon_legacy.h"

#define TYPE_RADEON_LEGACY_VGA "radeon-legacy-vga"
OBJECT_DECLARE_SIMPLE_TYPE(RadeonLegacyVGAState, RADEON_LEGACY_VGA)

typedef struct RadeonLegacyVGAState {
    PCIDevice parent_obj;
    VGACommonState vga;
    MemoryRegion mmio;
    MemoryRegion io;
    MemoryRegion linear_aper;
    RLGDevice *core;
    QEMUTimer vblank_timer;
    char *model;
    uint64_t linear_aper_sz;
    bool verbose;
    bool scanout_active;        /* VBE currently driven by the Radeon CRTC */
    RLGDisplayMode scanout;     /* last mode pushed to VBE */
} RadeonLegacyVGAState;

static int rlg_qemu_dma_read(void *opaque, uint64_t addr, void *buf, size_t len)
{
    RadeonLegacyVGAState *s = opaque;
    return pci_dma_read(&s->parent_obj, addr, buf, len) == MEMTX_OK ? 0 : -1;
}

static int rlg_qemu_dma_write(void *opaque, uint64_t addr, const void *buf, size_t len)
{
    RadeonLegacyVGAState *s = opaque;
    return pci_dma_write(&s->parent_obj, addr, buf, len) == MEMTX_OK ? 0 : -1;
}

static void rlg_qemu_irq(void *opaque, bool level)
{
    RadeonLegacyVGAState *s = opaque;
    pci_set_irq(&s->parent_obj, level);
}

static void rlg_qemu_dirty(void *opaque, uint32_t off, uint32_t len)
{
    RadeonLegacyVGAState *s = opaque;
    if (off >= s->vga.vram_size) {
        return;
    }
    if (len > s->vga.vram_size - off) {
        len = s->vga.vram_size - off;
    }
    memory_region_set_dirty(&s->vga.vram, off, len);
}

static void rlg_qemu_log(void *opaque, const char *msg)
{
    RadeonLegacyVGAState *s = opaque;
    if (s->verbose) {
        qemu_log_mask(LOG_UNIMP, "radeon-legacy: %s\n", msg);
    }
}

static bool rlg_mode_equal(const RLGDisplayMode *a, const RLGDisplayMode *b)
{
    return a->width == b->width && a->height == b->height && a->bpp == b->bpp &&
           a->pitch_bytes == b->pitch_bytes && a->offset_bytes == b->offset_bytes;
}

/* Mirror the Radeon CRTC (legacy or AVIVO D1) into QEMU's VBE scanout. */
static void rlg_qemu_sync_scanout(RadeonLegacyVGAState *s)
{
    RLGDisplayMode m = rlg_get_display_mode(s->core);
    unsigned bypp;

    if (!m.enabled || !m.width || !m.height || !m.bpp || (m.bpp == 24 && m.pitch_bytes % 3)) {
        if (s->scanout_active) {
            /* Hand the screen back to legacy VGA (text mode, VGA BIOS). */
            vbe_ioport_write_index(&s->vga, 0, VBE_DISPI_INDEX_ENABLE);
            vbe_ioport_write_data(&s->vga, 0, VBE_DISPI_DISABLED);
            s->scanout_active = false;
        }
        return;
    }
    if (s->scanout_active && rlg_mode_equal(&m, &s->scanout)) {
        return;
    }

    vbe_ioport_write_index(&s->vga, 0, VBE_DISPI_INDEX_ENABLE);
    vbe_ioport_write_data(&s->vga, 0, VBE_DISPI_DISABLED);
    s->vga.vbe_regs[VBE_DISPI_INDEX_XRES] = m.width;
    s->vga.vbe_regs[VBE_DISPI_INDEX_YRES] = m.height;
    s->vga.vbe_regs[VBE_DISPI_INDEX_BPP] = m.bpp;
    vbe_ioport_write_index(&s->vga, 0, VBE_DISPI_INDEX_ENABLE);
    vbe_ioport_write_data(&s->vga, 0, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED |
                                      VBE_DISPI_NOCLEARMEM);
    bypp = DIV_ROUND_UP(m.bpp, BITS_PER_BYTE);
    vbe_ioport_write_index(&s->vga, 0, VBE_DISPI_INDEX_VIRT_WIDTH);
    vbe_ioport_write_data(&s->vga, 0, m.pitch_bytes / bypp);
    s->vga.vbe_start_addr = m.offset_bytes / 4;
    s->vga.full_update_gfx = true;

    s->scanout = m;
    s->scanout_active = true;
}

static uint64_t rlg_qemu_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    RadeonLegacyVGAState *s = opaque;
    return rlg_mmio_read(s->core, addr, size);
}

static void rlg_qemu_mmio_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    RadeonLegacyVGAState *s = opaque;
    rlg_mmio_write(s->core, addr, value, size);
    if (rlg_is_display_reg(s->core, addr & ~3u)) {
        rlg_qemu_sync_scanout(s);
    }
}

static const MemoryRegionOps rlg_qemu_mmio_ops = {
    .read = rlg_qemu_mmio_read,
    .write = rlg_qemu_mmio_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void rlg_qemu_vblank(void *opaque)
{
    RadeonLegacyVGAState *s = opaque;
    rlg_vblank(s->core);
    timer_mod(&s->vblank_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND / 60);
}

static void rlg_qemu_reset(DeviceState *dev)
{
    RadeonLegacyVGAState *s = RADEON_LEGACY_VGA(dev);
    if (s->core) {
        rlg_reset(s->core);
    }
    s->scanout_active = false;
    vga_common_reset(&s->vga);
}

static bool rlg_qemu_profile(RadeonLegacyVGAState *s, RLGProfile *profile, Error **errp)
{
    *profile = RLG_PROFILE_X1250_AMD;
    if (s->model && !rlg_profile_parse(s->model, profile)) {
        error_setg(errp, "unknown model '%s' (%s)", s->model, rlg_profile_models());
        return false;
    }
    return true;
}

static DeviceRealize rlg_qemu_parent_dc_realize;

/*
 * Hybrid PCI/PCIe device. PCIe cards (X300, X600, X700, X850 XT...) get a PCI
 * Express capability when plugged into a PCIe bus, so radeon picks the PCIe
 * GART (it checks pci_is_pcie() in radeon_kms.c). This has to be decided
 * before pci_qdev_realize() sizes the config space. AGP cards and PCIe cards
 * on a conventional bus end up as plain PCI and use the R100 PCI GART.
 */
static void rlg_qemu_dc_realize(DeviceState *dev, Error **errp)
{
    RadeonLegacyVGAState *s = RADEON_LEGACY_VGA(dev);
    RLGProfile profile;

    if (!rlg_qemu_profile(s, &profile, errp)) {
        return;
    }
    if (rlg_chip_info(profile)->bus == RLG_BUS_PCIE) {
        s->parent_obj.cap_present |= QEMU_PCI_CAP_EXPRESS;
    }
    rlg_qemu_parent_dc_realize(dev, errp);
}

static void rlg_qemu_realize(PCIDevice *pdev, Error **errp)
{
    RadeonLegacyVGAState *s = RADEON_LEGACY_VGA(pdev);
    RLGProfile profile;
    const RLGChipInfo *chip;
    RLGConfig cfg;

    if (!rlg_qemu_profile(s, &profile, errp)) {
        return;
    }
    chip = rlg_chip_info(profile);
    if (!s->vga.vram_size_mb) {
        s->vga.vram_size_mb = chip->default_vram_mb;
    }

    pci_set_word(pdev->config + PCI_VENDOR_ID, chip->vendor_id);
    pci_set_word(pdev->config + PCI_DEVICE_ID, chip->device_id);
    pci_set_byte(pdev->config + PCI_REVISION_ID, chip->revision);
    pdev->config[PCI_INTERRUPT_PIN] = 1;

    if (pci_is_express(pdev)) {
        if (pci_bus_is_express(pci_get_bus(pdev))) {
            if (pcie_endpoint_cap_init(pdev, 0) < 0) {
                error_setg(errp, "failed to add the PCI Express capability");
                return;
            }
        } else {
            /* Conventional bus: behave as plain PCI (driver uses the PCI GART). */
            pdev->cap_present &= ~QEMU_PCI_CAP_EXPRESS;
        }
    }

    if (!vga_common_init(&s->vga, OBJECT(s), errp)) {
        return;
    }
    if (!s->linear_aper_sz) {
        s->linear_aper_sz = MAX((uint64_t)s->vga.vram_size, 128 * MiB);
    }
    if (s->linear_aper_sz < s->vga.vram_size || s->linear_aper_sz > 512 * MiB ||
        (s->linear_aper_sz & (s->linear_aper_sz - 1))) {
        error_setg(errp, "linear aperture must be power-of-two, >= VRAM and <= 512 MiB");
        return;
    }

    vga_init(&s->vga, OBJECT(s), pci_address_space(pdev), pci_address_space_io(pdev), true);
    s->vga.con = qemu_graphic_console_create(DEVICE(s), 0, s->vga.hw_ops, &s->vga);

    cfg = (RLGConfig) {
        .profile = profile,
        .backend = RLG_BACKEND_SOFTWARE,
        .vram_size = s->vga.vram_size,
        .verbose = s->verbose,
        .opaque = s,
        .dma_read = rlg_qemu_dma_read,
        .dma_write = rlg_qemu_dma_write,
        .irq = rlg_qemu_irq,
        .dirty = rlg_qemu_dirty,
        .log = rlg_qemu_log,
    };
    s->core = rlg_create_with_vram(&cfg, s->vga.vram_ptr, s->vga.vram_size);
    if (!s->core) {
        error_setg(errp, "failed to create Radeon legacy core");
        return;
    }

    memory_region_init_io(&s->mmio, OBJECT(s), &rlg_qemu_mmio_ops, s,
                          "radeon-legacy.mmio", RLG_MMIO_SIZE);
    memory_region_init_alias(&s->io, OBJECT(s), "radeon-legacy.io", &s->mmio, 0, 0x100);
    memory_region_init(&s->linear_aper, OBJECT(s), "radeon-legacy.linear-aperture",
                       s->linear_aper_sz);
    memory_region_add_subregion(&s->linear_aper, 0, &s->vga.vram);

    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_MEM_PREFETCH, &s->linear_aper);
    pci_register_bar(pdev, 1, PCI_BASE_ADDRESS_SPACE_IO, &s->io);
    pci_register_bar(pdev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);

    timer_init_ns(&s->vblank_timer, QEMU_CLOCK_VIRTUAL, rlg_qemu_vblank, s);
    timer_mod(&s->vblank_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND / 60);
}

static void rlg_qemu_exit(PCIDevice *pdev)
{
    RadeonLegacyVGAState *s = RADEON_LEGACY_VGA(pdev);
    timer_del(&s->vblank_timer);
    if (s->core) {
        rlg_destroy(s->core);
        s->core = NULL;
    }
    if (s->vga.con) {
        qemu_graphic_console_close(s->vga.con);
    }
}

static const Property rlg_qemu_properties[] = {
    DEFINE_PROP_STRING("model", RadeonLegacyVGAState, model),
    DEFINE_PROP_UINT32("vgamem_mb", RadeonLegacyVGAState, vga.vram_size_mb, 0),
    DEFINE_PROP_UINT64("x-linear-aper-size", RadeonLegacyVGAState, linear_aper_sz, 0),
    DEFINE_PROP_BOOL("verbose", RadeonLegacyVGAState, verbose, false),
};

static void rlg_qemu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);

    device_class_set_parent_realize(dc, rlg_qemu_dc_realize, &rlg_qemu_parent_dc_realize);
    device_class_set_legacy_reset(dc, rlg_qemu_reset);
    device_class_set_props(dc, rlg_qemu_properties);
    dc->hotpluggable = false;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);

    pc->vendor_id = RLG_VENDOR_ATI;
    pc->device_id = RLG_DEVICE_RS690;
    pc->revision = 0;
    pc->class_id = PCI_CLASS_DISPLAY_VGA;
    pc->romfile = "vgabios-ati.bin";
    pc->realize = rlg_qemu_realize;
    pc->exit = rlg_qemu_exit;
}

static const TypeInfo rlg_qemu_info = {
    .name = TYPE_RADEON_LEGACY_VGA,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(RadeonLegacyVGAState),
    .class_init = rlg_qemu_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { INTERFACE_PCIE_DEVICE },          /* hybrid: PCIe only for X300/X700 */
        { },
    },
};

static void rlg_qemu_register_types(void)
{
    type_register_static(&rlg_qemu_info);
}

type_init(rlg_qemu_register_types)
