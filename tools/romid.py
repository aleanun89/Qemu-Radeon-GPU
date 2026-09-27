#!/usr/bin/env python3
"""Identify ATI/AMD VBIOS images and tell which radeon-legacy-vga model= fits.

Usage: python3 tools/romid.py bios/*.rom

Only reads the files. The model table is parsed from include/radeon_legacy.h
and src/chip.c, so it always matches the code.
"""
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parent.parent


def load_models():
    """PCI device ID -> (model, name, bus) from the C sources."""
    hdr = (ROOT / "include/radeon_legacy.h").read_text(encoding="utf-8")
    chip = (ROOT / "src/chip.c").read_text(encoding="utf-8")
    ids = {m.group(1): int(m.group(2), 16)
           for m in re.finditer(r"#define RLG_DEVICE_(\w+)\s+0x([0-9a-fA-F]+)u", hdr)}
    alias = {}
    for name, prof in re.findall(r'\{\s*"([\w-]+)",\s*(RLG_PROFILE_\w+)\s*\}', chip):
        alias.setdefault(prof, name)                 # first alias = canonical model=
    models = {}
    for m in re.finditer(r"CHIP\((RLG_PROFILE_\w+),\s*\w+,\s*\"([^\"]+)\",\s*RLG_DEVICE_(\w+),"
                         r"[^;]*?(RLG_BUS_\w+)", chip, re.S):
        prof, name, dev, bus = m.groups()
        models[ids[dev]] = (alias.get(prof, "?"), name, bus.replace("RLG_BUS_", ""))
    return models


def u16(b, o): return struct.unpack_from("<H", b, o)[0]
def u32(b, o): return struct.unpack_from("<I", b, o)[0]


def identify(path, models):
    b = Path(path).read_bytes()
    print(f"{path}  ({len(b)} bytes)")
    if b[:2] != b"\x55\xaa":
        print("  not a PCI option ROM (no 55AA)\n")
        return
    pcir = u16(b, 0x18)
    if b[pcir:pcir + 4] != b"PCIR":
        print("  no PCIR structure\n")
        return
    ven, dev = u16(b, pcir + 4), u16(b, pcir + 6)
    img = b[2] * 512
    ok = img <= len(b) and sum(b[:img]) & 0xff == 0
    print(f"  PCI {ven:04x}:{dev:04x}   image {img} bytes, checksum {'ok' if ok else 'BAD'}")

    hdr = u16(b, 0x48)
    if hdr + 8 <= len(b) and b[hdr + 4:hdr + 8] == b"ATOM":
        msg = u16(b, hdr + 0x10)
        boot = b[msg:msg + 80].split(b"\0")[0].decode("latin1", "replace").strip()
        fw = u16(b, u16(b, hdr + 0x20) + 4 + 2 * 4)             # master data table[4] = FirmwareInfo
        clocks = ""
        if fw and fw + 16 <= len(b):
            clocks = f", default sclk {u32(b, fw + 8) // 100} MHz / mclk {u32(b, fw + 12) // 100} MHz"
        print(f"  AtomBIOS: '{boot}', subsystem {u16(b, hdr + 0x18):04x}:{u16(b, hdr + 0x1a):04x}{clocks}")
    else:
        print("  legacy COMBIOS (x86 code; no ATOM tables)")
    pn = re.search(rb"113-[A-Z0-9]+-\d+", b)
    if pn:
        print(f"  part number {pn.group().decode()}")

    if ven != 0x1002:
        print("  -> not an ATI/AMD ROM\n")
    elif dev in models:
        model, name, bus = models[dev]
        print(f"  -> SUPPORTED: {name}  (bus {bus})")
        print(f"     -device radeon-legacy-vga,model={model},romfile={path}\n")
    else:
        print("  -> no profile for this PCI ID yet\n")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    models = load_models()
    for p in sys.argv[1:]:
        identify(p, models)
    return 0


if __name__ == "__main__":
    sys.exit(main())
