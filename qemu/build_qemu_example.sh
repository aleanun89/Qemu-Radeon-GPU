#!/bin/sh
set -eu
QEMU_SRC="${1:?usage: build_qemu_example.sh /path/to/qemu-11.1.x [build-dir]}"
BUILD="${2:-$QEMU_SRC/build-radeon}"
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
python3 "$SCRIPT_DIR/apply_to_qemu_11_1.py" "$QEMU_SRC"
mkdir -p "$BUILD"
cd "$BUILD"
"$QEMU_SRC/configure" \
  --target-list=x86_64-softmmu,i386-softmmu \
  --enable-kvm \
  --enable-pixman \
  --disable-werror
ninja -j"$(nproc)"
