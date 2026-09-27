#include "radeon_legacy.h"
#include <string.h>

/*
 * PCI IDs match Linux include/drm/drm_pciids.h, names match pci.ids. Clocks
 * and pipe counts are nominal (taken from the VBIOS FirmwareInfo when a ROM
 * was available) and only used for reporting and R400_GB_PIPE_SELECT.
 *
 * Note on "Xpress 1250": the name was used for two different north bridges.
 * RS600 is the Intel-platform chipset; RS690 (AMD 690G/M690) is the one found
 * on Athlon 64 / Athlon 64 X2 boards. Both carry an R4xx-class 3D core with
 * no hardware vertex shaders (the driver runs VS on the CPU).
 *
 * The discrete R3xx/R4xx parts do have hardware TCL, so their command streams
 * carry vertex shader programs.
 */
#define CHIP(p, fam, nm, dev, rev, vram, mhz, ps, tcl, pipes, bs, r4, mob)          \
    [p] = { .profile = (p), .family = (fam), .name = (nm), .vendor_id = RLG_VENDOR_ATI, \
            .device_id = (dev), .revision = (rev), .default_vram_mb = (vram),        \
            .nominal_core_mhz = (mhz), .ps_profile = (ps), .hw_tcl = (tcl),          \
            .pixel_pipes = (pipes), .bus = (bs), .r4xx = (r4), .mobile = (mob) }

static const RLGChipInfo chips[RLG_PROFILE_COUNT] = {
    /* integrated */
    CHIP(RLG_PROFILE_XPRESS_200, RLG_FAMILY_RS400, "ATI Radeon Xpress 200 (RS480)",
         RLG_DEVICE_RS480, 0x01, 128, 300, "ps_2_0", false, 2, RLG_BUS_IGP, false, false),
    CHIP(RLG_PROFILE_XPRESS_200M, RLG_FAMILY_RS400, "ATI Radeon Xpress 200M (RS480M)",
         RLG_DEVICE_RS480M, 0x01, 128, 300, "ps_2_0", false, 2, RLG_BUS_IGP, false, true),
    CHIP(RLG_PROFILE_XPRESS_1250_INTEL, RLG_FAMILY_RS600, "ATI Radeon Xpress 1250 (RS600, Intel)",
         RLG_DEVICE_RS600, 0x00, 256, 400, "ps_2_b", false, 4, RLG_BUS_IGP, false, false),
    CHIP(RLG_PROFILE_XPRESS_1250M_INTEL, RLG_FAMILY_RS600, "ATI Radeon Xpress 1250M (RS600M, Intel)",
         RLG_DEVICE_RS600M, 0x00, 256, 400, "ps_2_b", false, 4, RLG_BUS_IGP, false, true),
    CHIP(RLG_PROFILE_X1250_AMD, RLG_FAMILY_RS690, "ATI Radeon X1250 (RS690 / AMD 690G)",
         RLG_DEVICE_RS690, 0x00, 256, 400, "ps_2_b", false, 4, RLG_BUS_IGP, false, false),
    CHIP(RLG_PROFILE_X1250M_AMD, RLG_FAMILY_RS690, "ATI Radeon X1250M (RS690M / AMD M690)",
         RLG_DEVICE_RS690M, 0x00, 256, 350, "ps_2_b", false, 4, RLG_BUS_IGP, false, true),

    /* discrete R3xx (RV350 / RV370 / RV380) */
    CHIP(RLG_PROFILE_R9550, RLG_FAMILY_R300, "ATI Radeon 9550 (RV350, AGP)",
         RLG_DEVICE_R9550, 0x00, 128, 250, "ps_2_0", true, 4, RLG_BUS_AGP, false, false),
    CHIP(RLG_PROFILE_X300, RLG_FAMILY_R300, "ATI Radeon X300 (RV370)",
         RLG_DEVICE_X300, 0x00, 128, 325, "ps_2_0", true, 4, RLG_BUS_PCIE, false, false),
    CHIP(RLG_PROFILE_X300SE, RLG_FAMILY_R300, "ATI Radeon X300 SE / X600 SE (RV370)",
         RLG_DEVICE_X300SE, 0x00, 128, 325, "ps_2_0", true, 4, RLG_BUS_PCIE, false, false),
    CHIP(RLG_PROFILE_X300M, RLG_FAMILY_R300, "ATI Mobility Radeon X300 (RV370/M22)",
         RLG_DEVICE_X300M, 0x00, 64, 350, "ps_2_0", true, 4, RLG_BUS_PCIE, false, true),
    CHIP(RLG_PROFILE_X600XT, RLG_FAMILY_R300, "ATI Radeon X600 XT (RV380)",
         RLG_DEVICE_X600XT, 0x00, 256, 500, "ps_2_0", true, 4, RLG_BUS_PCIE, false, false),

    /* discrete R4xx (RV410 / R480 / R481) */
    CHIP(RLG_PROFILE_X550XTX, RLG_FAMILY_R300, "ATI Radeon X550 XTX (RV410)",
         RLG_DEVICE_X550XTX, 0x00, 128, 400, "ps_2_b", true, 4, RLG_BUS_PCIE, true, false),
    CHIP(RLG_PROFILE_X700, RLG_FAMILY_R300, "ATI Radeon X700 (RV410)",
         RLG_DEVICE_X700, 0x00, 128, 400, "ps_2_b", true, 8, RLG_BUS_PCIE, true, false),
    CHIP(RLG_PROFILE_X700PRO, RLG_FAMILY_R300, "ATI Radeon X700 PRO (RV410)",
         RLG_DEVICE_X700PRO, 0x00, 128, 425, "ps_2_b", true, 8, RLG_BUS_PCIE, true, false),
    CHIP(RLG_PROFILE_X700XT, RLG_FAMILY_R300, "ATI Radeon X700 XT (RV410)",
         RLG_DEVICE_X700XT, 0x00, 128, 475, "ps_2_b", true, 8, RLG_BUS_PCIE, true, false),
    CHIP(RLG_PROFILE_X700M, RLG_FAMILY_R300, "ATI Mobility Radeon X700 (RV410/M26)",
         RLG_DEVICE_X700M, 0x00, 128, 350, "ps_2_b", true, 8, RLG_BUS_PCIE, true, true),
    CHIP(RLG_PROFILE_X850XT, RLG_FAMILY_R300, "ATI Radeon X850 XT (R480)",
         RLG_DEVICE_X850XT, 0x00, 256, 520, "ps_2_b", true, 16, RLG_BUS_PCIE, true, false),
    CHIP(RLG_PROFILE_X850XT_AGP, RLG_FAMILY_R300, "ATI Radeon X850 XT AGP (R481)",
         RLG_DEVICE_X850XT_AGP, 0x00, 256, 520, "ps_2_b", true, 16, RLG_BUS_AGP, true, false),
};

static const struct { const char *name; RLGProfile profile; } aliases[] = {
    { "rs480", RLG_PROFILE_XPRESS_200 },
    { "xpress200", RLG_PROFILE_XPRESS_200 },
    { "rs480m", RLG_PROFILE_XPRESS_200M },
    { "xpress200m", RLG_PROFILE_XPRESS_200M },
    { "rs600", RLG_PROFILE_XPRESS_1250_INTEL },
    { "xpress1250-intel", RLG_PROFILE_XPRESS_1250_INTEL },
    { "rs600m", RLG_PROFILE_XPRESS_1250M_INTEL },
    { "rs690", RLG_PROFILE_X1250_AMD },
    { "x1250", RLG_PROFILE_X1250_AMD },
    { "xpress1250", RLG_PROFILE_X1250_AMD },
    { "rs690m", RLG_PROFILE_X1250M_AMD },
    { "x1250m", RLG_PROFILE_X1250M_AMD },
    { "r9550", RLG_PROFILE_R9550 },
    { "rv350", RLG_PROFILE_R9550 },
    { "x300", RLG_PROFILE_X300 },
    { "rv370", RLG_PROFILE_X300 },
    { "x300se", RLG_PROFILE_X300SE },
    { "x600se", RLG_PROFILE_X300SE },
    { "x300m", RLG_PROFILE_X300M },
    { "x600xt", RLG_PROFILE_X600XT },
    { "rv380", RLG_PROFILE_X600XT },
    { "x550xtx", RLG_PROFILE_X550XTX },
    { "x700", RLG_PROFILE_X700 },
    { "rv410", RLG_PROFILE_X700 },
    { "x700pro", RLG_PROFILE_X700PRO },
    { "x700xt", RLG_PROFILE_X700XT },
    { "x700m", RLG_PROFILE_X700M },
    { "x850xt", RLG_PROFILE_X850XT },
    { "r480", RLG_PROFILE_X850XT },
    { "x850xt-agp", RLG_PROFILE_X850XT_AGP },
    { "r481", RLG_PROFILE_X850XT_AGP },
};

const RLGChipInfo *rlg_chip_info(RLGProfile p)
{
    if ((unsigned)p >= RLG_PROFILE_COUNT) {
        p = RLG_PROFILE_XPRESS_200;
    }
    return &chips[p];
}

const char *rlg_profile_name(RLGProfile p) { return rlg_chip_info(p)->name; }

bool rlg_profile_parse(const char *n, RLGProfile *out)
{
    if (!n) {
        return false;
    }
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); ++i) {
        if (!strcmp(n, aliases[i].name)) {
            *out = aliases[i].profile;
            return true;
        }
    }
    return false;
}

RLGProfile rlg_profile_from_name(const char *n)
{
    RLGProfile p = RLG_PROFILE_X1250_AMD;
    (void)rlg_profile_parse(n, &p);
    return p;
}

const char *rlg_profile_models(void)
{
    return "rs480, rs480m, rs600, rs600m, rs690, rs690m, r9550, x300, x300se, x300m, x600xt, "
           "x550xtx, x700, x700pro, x700xt, x700m, x850xt, x850xt-agp";
}
