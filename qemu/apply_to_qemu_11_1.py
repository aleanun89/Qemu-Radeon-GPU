#!/usr/bin/env python3
"""Copy the radeon-legacy device into a QEMU 11.1.x source tree (idempotent).

The core is taken straight from ../src and ../include so there is a single
source of truth; only the QEMU adapter lives under overlay/.
"""
from pathlib import Path
import re
import shutil
import sys

CORE_SOURCES = ["chip.c", "device.c", "memory.c", "irq.c", "display.c",
                "engine2d.c", "cp.c", "mmio.c"]
BEGIN = "# BEGIN radeon-legacy-vga\n"
END = "# END radeon-legacy-vga\n"


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: apply_to_qemu_11_1.py /path/to/qemu-11.1.x")
        return 2
    q = Path(sys.argv[1]).resolve()
    here = Path(__file__).resolve().parent
    root = here.parent
    required = [q / "hw/display/meson.build", q / "hw/display/Kconfig",
                q / "hw/display/vga_int.h"]
    missing = [str(x) for x in required if not x.exists()]
    if missing:
        print("Not a QEMU source tree; missing:")
        for x in missing:
            print("  " + x)
        return 1

    dst_dir = q / "hw/display"
    core_dir = dst_dir / "radeon_legacy"
    core_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(here / "overlay/hw/display/radeon-legacy-vga.c", dst_dir)
    print("copy hw/display/radeon-legacy-vga.c")
    for name in CORE_SOURCES:
        shutil.copy2(root / "src" / name, core_dir / name)
        print("copy hw/display/radeon_legacy/" + name)
    for hdr in sorted((root / "include").glob("*.h")):
        shutil.copy2(hdr, core_dir / hdr.name)
        print("copy hw/display/radeon_legacy/" + hdr.name)

    kp = dst_dir / "Kconfig"
    k = kp.read_text(encoding="utf-8")
    if "config RADEON_LEGACY_VGA" not in k:
        block = ("config RADEON_LEGACY_VGA\n"
                 "    bool\n"
                 "    default y if PCI_DEVICES\n"
                 "    depends on PCI\n"
                 "    select VGA\n\n")
        idx = k.find("config ATI_VGA\n")
        k = (k + "\n" + block) if idx < 0 else (k[:idx] + block + k[idx:])
        kp.write_text(k, encoding="utf-8")
        print("patch hw/display/Kconfig")

    mp = dst_dir / "meson.build"
    m = mp.read_text(encoding="utf-8")
    files = ["radeon-legacy-vga.c"] + ["radeon_legacy/" + s for s in CORE_SOURCES]
    block = (BEGIN +
             "system_ss.add(when: 'CONFIG_RADEON_LEGACY_VGA', if_true: [files(\n" +
             "".join("  '%s',\n" % f for f in files) +
             "), pixman])\n" + END)
    # Drop any previous copy (marked, or the unmarked block written by v3).
    m = re.sub(re.escape(BEGIN) + r".*?" + re.escape(END), "", m, flags=re.S)
    m = re.sub(r"system_ss\.add\(when: 'CONFIG_RADEON_LEGACY_VGA'.*?\), pixman\]\)\n\n?",
               "", m, flags=re.S)
    anchor = "system_ss.add(when: 'CONFIG_ATI_VGA'"
    idx = m.find(anchor)
    m = (m + "\n" + block) if idx < 0 else (m[:idx] + block + "\n" + m[idx:])
    mp.write_text(m, encoding="utf-8")
    print("patch hw/display/meson.build")

    print("Applied. Device: radeon-legacy-vga")
    return 0


if __name__ == "__main__":
    sys.exit(main())
