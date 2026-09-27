#include "radeon_legacy.h"
#include <string.h>

/*
 * PCI IDs match Linux include/drm/drm_pciids.h. Clocks and pipe counts are
 * nominal/approximate and only used for reporting.
 *
 * Note on "Xpress 1250": the name was used for two different north bridges.
 * RS600 is the Intel-platform chipset; RS690 (AMD 690G/M690) is the one found
 * on Athlon 64 / Athlon 64 X2 boards. Both carry an R4xx-class 3D core with
 * no hardware vertex shaders (the driver runs VS on the CPU).
 *
 * The discrete X300 (RV370, Linux CHIP_RV380) and X700 (RV410) do have
 * hardware TCL, so their command streams carry vertex shader programs.
 */
static const RLGChipInfo chips[RLG_PROFILE_COUNT] = {
    { RLG_PROFILE_XPRESS_200, RLG_FAMILY_RS400, "ATI Radeon Xpress 200 (RS480)",
      RLG_VENDOR_ATI, RLG_DEVICE_RS480, 0x01, 128, 300, "ps_2_0", false, 2, true, false },
    { RLG_PROFILE_XPRESS_200M, RLG_FAMILY_RS400, "ATI Radeon Xpress 200M (RS480M)",
      RLG_VENDOR_ATI, RLG_DEVICE_RS480M, 0x01, 128, 300, "ps_2_0", false, 2, true, true },
    { RLG_PROFILE_XPRESS_1250_INTEL, RLG_FAMILY_RS600, "ATI Radeon Xpress 1250 (RS600, Intel)",
      RLG_VENDOR_ATI, RLG_DEVICE_RS600, 0x00, 256, 400, "ps_2_b", false, 4, true, false },
    { RLG_PROFILE_XPRESS_1250M_INTEL, RLG_FAMILY_RS600, "ATI Radeon Xpress 1250M (RS600M, Intel)",
      RLG_VENDOR_ATI, RLG_DEVICE_RS600M, 0x00, 256, 400, "ps_2_b", false, 4, true, true },
    { RLG_PROFILE_X1250_AMD, RLG_FAMILY_RS690, "ATI Radeon X1250 (RS690 / AMD 690G)",
      RLG_VENDOR_ATI, RLG_DEVICE_RS690, 0x00, 256, 400, "ps_2_b", false, 4, true, false },
    { RLG_PROFILE_X1250M_AMD, RLG_FAMILY_RS690, "ATI Radeon X1250M (RS690M / AMD M690)",
      RLG_VENDOR_ATI, RLG_DEVICE_RS690M, 0x00, 256, 350, "ps_2_b", false, 4, true, true },
    { RLG_PROFILE_X300, RLG_FAMILY_R300_PCIE, "ATI Radeon X300 (RV370)",
      RLG_VENDOR_ATI, RLG_DEVICE_X300, 0x00, 128, 325, "ps_2_0", true, 4, false, false },
    { RLG_PROFILE_X300M, RLG_FAMILY_R300_PCIE, "ATI Mobility Radeon X300 (RV370/M22)",
      RLG_VENDOR_ATI, RLG_DEVICE_X300M, 0x00, 64, 350, "ps_2_0", true, 4, false, true },
    { RLG_PROFILE_X700, RLG_FAMILY_R300_PCIE, "ATI Radeon X700 (RV410)",
      RLG_VENDOR_ATI, RLG_DEVICE_X700, 0x00, 128, 400, "ps_2_b", true, 8, false, false },
    { RLG_PROFILE_X700PRO, RLG_FAMILY_R300_PCIE, "ATI Radeon X700 PRO (RV410)",
      RLG_VENDOR_ATI, RLG_DEVICE_X700PRO, 0x00, 128, 425, "ps_2_b", true, 8, false, false },
    { RLG_PROFILE_X700XT, RLG_FAMILY_R300_PCIE, "ATI Radeon X700 XT (RV410)",
      RLG_VENDOR_ATI, RLG_DEVICE_X700XT, 0x00, 128, 475, "ps_2_b", true, 8, false, false },
    { RLG_PROFILE_X700M, RLG_FAMILY_R300_PCIE, "ATI Mobility Radeon X700 (RV410/M26)",
      RLG_VENDOR_ATI, RLG_DEVICE_X700M, 0x00, 128, 350, "ps_2_b", true, 8, false, true },
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
    { "x300", RLG_PROFILE_X300 },
    { "rv370", RLG_PROFILE_X300 },
    { "x300m", RLG_PROFILE_X300M },
    { "x700", RLG_PROFILE_X700 },
    { "rv410", RLG_PROFILE_X700 },
    { "x700pro", RLG_PROFILE_X700PRO },
    { "x700xt", RLG_PROFILE_X700XT },
    { "x700m", RLG_PROFILE_X700M },
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
