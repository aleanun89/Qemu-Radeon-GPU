# Radeon Legacy vGPU v4

Emulador experimental de GPUs ATI clásicas con un adaptador PCI/PCIe/VGA para
QEMU:

- integradas: Radeon Xpress 200 y X1250;
- dedicadas PCIe: X300 y X700. El objetivo es reproducir la interfaz que ve el
driver (PCI, MMIO, MC, GART, ring, IRQ y display) y ejecutar el trabajo gráfico
en el host, a futuro mediante Vulkan.

## Perfiles

| `model=` | Chip | Plataforma | PCI ID | Familia de registros | VRAM/UMA por defecto |
|---|---|---|---:|---|---:|
| `rs690` (por defecto; alias `x1250`, `xpress1250`) | Radeon X1250 (RS690 / AMD 690G) | AMD K8 (Athlon 64 / X2) | `1002:791E` | AVIVO + MC indirecto @0x78 + GART RS400 | 256 MiB |
| `rs690m` | Radeon X1250M (RS690M / M690) | AMD K8 portátil | `1002:791F` | igual que RS690 | 256 MiB |
| `rs480` | Radeon Xpress 200 (RS480) | AMD K8 | `1002:5954` | CRTC clásico + GART RS400 | 128 MiB |
| `rs480m` | Radeon Xpress 200M (RS480M) | AMD K8 portátil | `1002:5955` | igual que RS480 | 128 MiB |
| `rs600` | Radeon Xpress 1250 (RS600) | **Intel** | `1002:7941` | AVIVO + MC indirecto @0x70 + tabla plana | 256 MiB |
| `rs600m` | Radeon Xpress 1250M (RS600M) | **Intel** | `1002:7942` | igual que RS600 | 256 MiB |
| `x300` (alias `rv370`) | Radeon X300 (RV370) | tarjeta PCIe | `1002:5B60` | CRTC clásico + PCIe indirecto @0x30 + PCIe GART | 128 MiB |
| `x300m` | Mobility Radeon X300 (RV370/M22) | portátil PCIe | `1002:5460` | igual que X300 | 64 MiB |
| `x700` (alias `rv410`) | Radeon X700 (RV410) | tarjeta PCIe | `1002:5E4D` | igual que X300 | 128 MiB |
| `x700pro` | Radeon X700 PRO (RV410) | tarjeta PCIe | `1002:5E4B` | igual que X300 | 128 MiB |
| `x700xt` | Radeon X700 XT (RV410) | tarjeta PCIe | `1002:5E4A` | igual que X300 | 128 MiB |
| `x700m` | Mobility Radeon X700 (RV410/M26) | portátil PCIe | `1002:5652` | igual que X300 | 128 MiB |

Los PCI ID coinciden con `include/drm/drm_pciids.h` de Linux, y los nombres con
`pci.ids`. "Xpress 1250" se usó comercialmente para dos northbridges distintos:
el RS600 (Intel) y el RS690 (AMD 690G). Para un equipo Athlon 64 X2, el correcto
es el **RS690**.

Diferencias entre integradas y dedicadas:

- **TCL (vertex shaders por hardware):**
  - RS480, RS600 y RS690 **no tienen TCL**: el driver ejecuta los vertex
    shaders en la CPU, así que el backend 3D solo recibe vértices transformados.
  - X300 y X700 **sí tienen TCL**: su ring llevará programas de vertex shader
    (PVS), que el futuro backend 3D tendrá que traducir.
- **Bus PCIe:** X300 y X700 son dispositivos PCIe. El driver `radeon` solo usa
  el PCIe GART si el dispositivo tiene capability PCI Express, así que hay que
  usar **`-machine q35`**. En la máquina `pc` (i440FX) el dispositivo es PCI
  convencional y QEMU muestra un aviso.

## Qué implementa

### Núcleo portable (`src/`, `include/`)

- MMIO de 64 KiB con accesos de 8, 16 y 32 bits:
  - registros write-1-to-clear respetados en escrituras parciales;
  - `MM_INDEX`/`MM_DATA` a registros o VRAM, con protección contra
    auto-referencia.
- MC indirecto por familia:
  - RS480 `0x168/0x16C`, RS600 `0x70/0x74` y RS690 `0x78/0x7C`, con bit
    `WR_EN`;
  - X300/X700: espacio PCIe indirecto en `PCIE_INDEX/PCIE_DATA` (`0x30/0x34`).
- Espacio de direcciones GPU:
  - apertura de VRAM según `MC_FB_LOCATION` / `MCCFG_FB_LOCATION`;
  - GART RS400/RS690: PTE de 32 bits con flags R/W y bits 39:32;
  - GART RS600: tabla plana de PTE de 64 bits en VRAM;
  - PCIe GART X300/X700 (`PCIE_TX_GART_*`): tabla en VRAM, PTE
    `addr >> 8 | addr[39:32] << 24 | R/W`.
- Command Processor:
  - tamaño de ring `2 << RB_BUFSZ`;
  - `RB_RPTR_WR`, `CSQ_CNTL` (el CP no ejecuta con `PRIDIS`);
  - PACKET0 (incluido `ONE_REG_WR`), PACKET1, PACKET2 y PACKET3;
  - paquetes incompletos: se espera al resto;
  - **indirect buffers** (`CP_IB_BASE`/`CP_IB_BUFSZ`);
  - writeback de `RPTR` (respeta `RB_NO_UPDATE`) y de scratch
    (`SCRATCH_UMSK`/`SCRATCH_ADDR`).
- Interrupciones:
  - fences por `SW_INT_FIRE` → `SW_INT`;
  - VBlank del CRTC clásico;
  - VBlank AVIVO (`DxMODE_INT_MASK`, `DISP_INTERRUPT_STATUS`, ACK en
    `D1MODE_VBLANK_STATUS`).
- Display:
  - CRTC clásico (`CRTC_PITCH` en unidades de 8 píxeles, `CRTC_DISPLAY_DIS`);
  - AVIVO D1 (`D1GRPH_*`, `D1CRTC_CONTROL`; 8, 15, 16 y 32 bpp).
- Motor 2D software: `SRCCOPY`, `PATCOPY`, `BLACKNESS` y `WHITENESS`.
  - Superficies por `DST/SRC/DEFAULT_PITCH_OFFSET` en direcciones MC, con
    `GMC_*_PITCH_OFFSET_CNTL`.
  - `DP_GUI_MASTER_CNTL` fija el datatype y el ROP.
  - Clipping, write mask y disparo por `DST_HEIGHT`, `DST_HEIGHT_WIDTH` y
    `DST_WIDTH_HEIGHT`.
- Backend Vulkan opcional (solo inicialización: instancia, dispositivo, cola y
  command pool).

### Adaptador QEMU (`qemu/overlay/`)

- Dispositivo `radeon-legacy-vga`.
  - `model=` inválido da error al arrancar (antes caía en silencio al RS480).
  - Híbrido PCI/PCIe: X300/X700 se presentan como endpoint PCI Express en un
    bus PCIe (`q35`); las integradas siguen siendo PCI convencional.
- BARs:
  - BAR0: aperture lineal;
  - BAR1: alias de I/O;
  - BAR2: MMIO.
- VGA legado + scanout: el modo del CRTC clásico o del AVIVO D1 se refleja en
  VBE. Cuando el driver apaga el CRTC, se devuelve la pantalla al VGA.
- DMA con `pci_dma_*`, INTx, VBlank a 60 Hz y log de lo no implementado con
  `-d unimp`.

## Lo que falta (no esperes Catalyst 3D todavía)

- Traducción de estado R300/R400 y del microcódigo de fragment shader a SPIR-V.
  X300/X700 necesitan además traducir los vertex shaders (PVS).
- GART PCI de la R100 (lo que usaría el driver con X300/X700 en un bus PCI
  convencional).
- Texturas, render targets y la caché de superficies VRAM ↔ Vulkan.
- AtomBIOS: tanto `radeon` KMS como Catalyst necesitan una VBIOS RS690 con
  tablas ATOM. No se incluye ninguna.
- PLL/clocks, CRTC2, encoders (TMDS/LVDS/DAC), I2C/DDC y power management.
- Microcódigo del CP: se aceptan las escrituras a `CP_ME_RAM_*` pero no se
  ejecutan.
- El CP se procesa de forma síncrona en el hilo de la vCPU.

## Compilar y probar el núcleo

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/radeon-legacy-demo rs690
```

Sin CMake ni compilador de C instalados (por ejemplo, en Windows), sirve
`zig cc` desde pip:

```bash
python -m pip install --user ziglang
python -m ziglang cc -std=c11 -Wall -Wextra -Iinclude src/chip.c src/device.c src/memory.c src/irq.c src/display.c src/engine2d.c src/cp.c src/mmio.c src/vulkan.c tests/test_core.c -o test-core.exe
```

## Integración QEMU

Ver `docs/QEMU_INTEGRATION.md`. Resumen:

```bash
python3 qemu/apply_to_qemu_11_1.py /ruta/qemu-11.1.x
```

```bash
qemu-system-x86_64 -vga none -device radeon-legacy-vga,model=rs690 -m 2048 -drive file=winxp.qcow2,if=ide
```

```bash
qemu-system-x86_64 -machine q35 -vga none -device radeon-legacy-vga,model=x700,bus=pcie.0 -m 2048 -drive file=linux.qcow2
```

Cambios respecto a v3: ver `CHANGELOG.md`.
