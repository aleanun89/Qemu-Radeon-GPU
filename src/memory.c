#include "radeon_legacy_int.h"
#include <string.h>

static int dma_read(RLGDevice *d, uint64_t a, void *b, size_t l)
{
    return d->cfg.dma_read ? d->cfg.dma_read(d->cfg.opaque, a, b, l) : -1;
}

static int dma_write(RLGDevice *d, uint64_t a, const void *b, size_t l)
{
    return d->cfg.dma_write ? d->cfg.dma_write(d->cfg.opaque, a, b, l) : -1;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- MC indirect register space ---------------------------------------- */

uint32_t rlg__mc_read(RLGDevice *d, uint32_t reg)
{
    return reg < RLG_MC_REGS ? d->mc_regs[reg] : 0;
}

void rlg__mc_write(RLGDevice *d, uint32_t reg, uint32_t value)
{
    if (reg >= RLG_MC_REGS) {
        rlg__log(d, "MC write to unmodelled reg 0x%x = 0x%08x", reg, value);
        return;
    }
    d->mc_regs[reg] = value;
    rlg__gart_update(d);
}

/* ---- VRAM aperture in MC address space ----------------------------------- */

static uint32_t fb_location(const RLGDevice *d)
{
    switch (d->chip->family) {
    case RLG_FAMILY_RS600:
        return d->mc_regs[RLG_RS600_MC_FB_LOCATION];
    case RLG_FAMILY_RS690:
        return d->mc_regs[RLG_RS690_MCCFG_FB_LOCATION];
    case RLG_FAMILY_RS400:
    default:
        return rlg_reg_read32(d, RLG_MC_FB_LOCATION);
    }
}

uint64_t rlg_fb_start(const RLGDevice *d)
{
    return (uint64_t)(fb_location(d) & 0xffffu) << 16;
}

bool rlg_mc_to_vram(const RLGDevice *d, uint64_t mc, uint64_t len, uint32_t *off)
{
    uint32_t loc = fb_location(d);
    uint64_t start = (uint64_t)(loc & 0xffffu) << 16;
    uint64_t top = (uint64_t)(loc & 0xffff0000u) | 0xffffu;

    if (!len || top < start || mc < start || mc > top || len - 1 > top - mc) {
        return false;
    }
    if (mc - start > d->vram_size || len > d->vram_size - (mc - start)) {
        return false;
    }
    if (off) {
        *off = (uint32_t)(mc - start);
    }
    return true;
}

/* ---- GART ------------------------------------------------------------------ */

void rlg__gart_update(RLGDevice *d)
{
    RLGGARTState *g = &d->gart;

    if (d->chip->family == RLG_FAMILY_RS600) {
        uint32_t start = d->mc_regs[RLG_RS600_MC_PT0_FLAT_START_ADDR];
        uint32_t end = d->mc_regs[RLG_RS600_MC_PT0_FLAT_END_ADDR];
        g->kind = RLG_GART_RS600;
        g->enabled = (d->mc_regs[RLG_RS600_MC_CNTL1] & (1u << 26)) &&
                     (d->mc_regs[RLG_RS600_MC_PT0_CNTL] & 1u) && end > start;
        g->table_base = d->mc_regs[RLG_RS600_MC_PT0_FLAT_BASE_ADDR];
        g->aperture_base = start;
        g->aperture_size = end > start ? (uint64_t)end - start + 1u : 0;
        return;
    }

    if (d->chip->family == RLG_FAMILY_R300) {
        uint32_t aic = rlg_reg_read32(d, RLG_AIC_CNTL);
        if (aic & RLG_PCIGART_TRANSLATE_EN) {
            /* R100 PCI GART (r100_pci_gart_enable): used for AGP parts and for PCIe
             * parts on a conventional PCI bus. HI_ADDR is the last byte. */
            uint32_t lo = rlg_reg_read32(d, RLG_AIC_LO_ADDR);
            uint32_t hi = rlg_reg_read32(d, RLG_AIC_HI_ADDR);
            g->kind = RLG_GART_PCI;
            g->enabled = hi > lo;
            g->table_base = rlg_reg_read32(d, RLG_AIC_PT_BASE) & ~(RLG_PAGE_SIZE - 1u);
            g->aperture_base = lo;
            g->aperture_size = hi > lo ? (uint64_t)hi - lo + 1u : 0;
            return;
        }
        /* PCIe GART (rv370_pcie_gart_enable): END_LO is the start of the last page. */
        uint32_t start = d->mc_regs[RLG_PCIE_TX_GART_START_LO];
        uint32_t end = d->mc_regs[RLG_PCIE_TX_GART_END_LO];
        g->kind = RLG_GART_PCIE;
        g->enabled = (d->mc_regs[RLG_PCIE_TX_GART_CNTL] & RLG_PCIE_TX_GART_EN) && end >= start;
        g->table_base = d->mc_regs[RLG_PCIE_TX_GART_BASE];
        g->aperture_base = start;
        g->aperture_size = end >= start ? (uint64_t)end - start + RLG_PAGE_SIZE : 0;
        return;
    }

    /* RS400/RS480/RS690: see Linux rs400_gart_enable(). */
    g->kind = RLG_GART_RS400;
    uint32_t size_reg = d->mc_regs[RLG_RS480_AGP_ADDRESS_SPACE_SIZE];
    uint32_t base = d->mc_regs[RLG_RS480_GART_BASE];
    uint32_t agp_loc = d->chip->family == RLG_FAMILY_RS690
                     ? d->mc_regs[RLG_RS690_MCCFG_AGP_LOCATION]
                     : rlg_reg_read32(d, RLG_MC_AGP_LOCATION);
    unsigned va = (size_reg >> 1) & 7u;

    g->enabled = (size_reg & 1u) && va <= 6;
    g->table_base = (uint64_t)(base & 0xfffff000u) | ((uint64_t)((base >> 4) & 0xffu) << 32);
    g->aperture_base = (uint64_t)(agp_loc & 0xffffu) << 16;
    g->aperture_size = (32ull << 20) << va;
}

int rlg_gart_translate(RLGDevice *d, uint64_t gpu, uint64_t *phys)
{
    RLGGARTState *g;
    uint64_t rel, page;

    if (!d || !phys) {
        return -1;
    }
    g = &d->gart;
    if (!g->enabled || gpu < g->aperture_base || gpu - g->aperture_base >= g->aperture_size) {
        g->faults++;
        return -1;
    }
    rel = gpu - g->aperture_base;
    page = rel / RLG_PAGE_SIZE;

    if (g->kind == RLG_GART_PCI) {
        /* r100_pci_gart_get_page_entry(): the entry is the bus address itself. */
        uint8_t raw[4];
        if (dma_read(d, g->table_base + page * 4u, raw, 4) != 0) {
            g->faults++;
            return -2;
        }
        *phys = (uint64_t)(le32(raw) & ~(RLG_PAGE_SIZE - 1u)) + (rel & (RLG_PAGE_SIZE - 1u));
    } else if (g->kind == RLG_GART_RS600) {
        /* Flat table of 64-bit PTEs, located in VRAM (MC address). */
        uint8_t raw[8];
        uint32_t off;
        uint64_t pte;
        if (!rlg_mc_to_vram(d, g->table_base + page * 8u, 8, &off)) {
            g->faults++;
            return -2;
        }
        memcpy(raw, d->vram + off, 8);
        pte = (uint64_t)le32(raw) | ((uint64_t)le32(raw + 4) << 32);
        if (!(pte & RLG_RS600_PTE_VALID)) {
            g->faults++;
            return -3;
        }
        *phys = (pte & 0xfffffffffffff000ull) + (rel & (RLG_PAGE_SIZE - 1u));
    } else if (g->kind == RLG_GART_PCIE) {
        /* 32-bit PTEs in VRAM: addr[31:12] in 23:4, addr[39:32] in 31:24, flags in 3:0. */
        uint32_t off, pte;
        if (!rlg_mc_to_vram(d, g->table_base + page * 4u, 4, &off)) {
            g->faults++;
            return -2;
        }
        pte = le32(d->vram + off);
        if (!(pte & (RLG_R300_PTE_READABLE | RLG_R300_PTE_WRITEABLE))) {
            g->faults++;
            return -3;
        }
        *phys = ((uint64_t)(pte & 0x00fffff0u) << 8) | ((uint64_t)(pte >> 24) << 32);
        *phys += rel & (RLG_PAGE_SIZE - 1u);
    } else {
        /* 32-bit PTEs in system memory: addr[31:12] | addr[39:32] << 4 | flags. */
        uint8_t raw[4];
        uint32_t pte;
        if (dma_read(d, g->table_base + page * 4u, raw, 4) != 0) {
            g->faults++;
            return -2;
        }
        pte = le32(raw);
        if (!(pte & (RLG_RS400_PTE_READABLE | RLG_RS400_PTE_WRITEABLE))) {
            g->faults++;
            return -3;
        }
        *phys = (uint64_t)(pte & 0xfffff000u) | ((uint64_t)((pte >> 4) & 0xffu) << 32);
        *phys += rel & (RLG_PAGE_SIZE - 1u);
    }
    g->translations++;
    return 0;
}

/* ---- GPU accesses ---------------------------------------------------------- */

int rlg_gpu_read(RLGDevice *d, uint64_t a, void *b, size_t l)
{
    uint8_t *out = b;

    if (!d || !b) {
        return -1;
    }
    while (l) {
        size_t c = RLG_PAGE_SIZE - (size_t)(a & (RLG_PAGE_SIZE - 1u));
        uint32_t off;
        uint64_t p;
        if (c > l) {
            c = l;
        }
        if (rlg_mc_to_vram(d, a, c, &off)) {
            memcpy(out, d->vram + off, c);
        } else if (rlg_gart_translate(d, a, &p) != 0) {
            return -2;
        } else if (dma_read(d, p, out, c) != 0) {
            return -3;
        }
        a += c;
        out += c;
        l -= c;
    }
    return 0;
}

int rlg_gpu_write(RLGDevice *d, uint64_t a, const void *b, size_t l)
{
    const uint8_t *in = b;

    if (!d || !b) {
        return -1;
    }
    while (l) {
        size_t c = RLG_PAGE_SIZE - (size_t)(a & (RLG_PAGE_SIZE - 1u));
        uint32_t off;
        uint64_t p;
        if (c > l) {
            c = l;
        }
        if (rlg_mc_to_vram(d, a, c, &off)) {
            memcpy(d->vram + off, in, c);
            if (d->cfg.dirty) {
                d->cfg.dirty(d->cfg.opaque, off, (uint32_t)c);
            }
        } else if (rlg_gart_translate(d, a, &p) != 0) {
            return -2;
        } else if (dma_write(d, p, in, c) != 0) {
            return -3;
        }
        a += c;
        in += c;
        l -= c;
    }
    return 0;
}
