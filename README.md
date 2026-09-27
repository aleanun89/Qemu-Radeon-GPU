# Radeon Legacy vGPU

Emulador experimental de GPUs ATI clásicas con un adaptador PCI/PCIe/VGA para
QEMU:

- integradas: Radeon Xpress 200 y X1250;
- dedicadas R3xx/R4xx: Radeon 9550, X300, X550, X600, X700 y X850, en AGP y
  PCIe.

El objetivo es reproducir la interfaz que ve el driver (PCI, MMIO, MC, GART,
ring, IRQ y display) y ejecutar el trabajo gráfico en el host, a futuro
mediante Vulkan.

## Perfiles

### Integradas

| `model=` | Chip | Plataforma | PCI ID | Familia de registros | VRAM/UMA |
|---|---|---|---:|---|---:|
| `rs690` (por defecto; alias `x1250`, `xpress1250`) | Radeon X1250 (RS690 / AMD 690G) | AMD K8 (Athlon 64 / X2) | `1002:791E` | AVIVO + MC indirecto @0x78 + GART RS400 | 256 MiB |
| `rs690m` | Radeon X1250M (RS690M / M690) | AMD K8 portátil | `1002:791F` | igual que RS690 | 256 MiB |
| `rs480` | Radeon Xpress 200 (RS480) | AMD K8 | `1002:5954` | CRTC clásico + GART RS400 | 128 MiB |
| `rs480m` | Radeon Xpress 200M (RS480M) | AMD K8 portátil | `1002:5955` | igual que RS480 | 128 MiB |
| `rs600` | Radeon Xpress 1250 (RS600) | **Intel** | `1002:7941` | AVIVO + MC indirecto @0x70 + tabla plana | 256 MiB |
| `rs600m` | Radeon Xpress 1250M (RS600M) | **Intel** | `1002:7942` | igual que RS600 | 256 MiB |

### Dedicadas (familia R300: CRTC clásico + PCIe GART o GART PCI)

| `model=` | Chip | Bus real | PCI ID | Núcleo | VRAM |
|---|---|---|---:|---:|---:|
| `r9550` (alias `rv350`) | Radeon 9550 (RV350) | AGP | `1002:4153` | 250 MHz | 128 MiB |
| `x300` (alias `rv370`) | Radeon X300 (RV370) | PCIe | `1002:5B60` | 325 MHz | 128 MiB |
| `x300se` (alias `x600se`) | Radeon X300 SE / X600 SE (RV370) | PCIe | `1002:5B62` | 325 MHz | 128 MiB |
| `x300m` | Mobility Radeon X300 (RV370/M22) | PCIe | `1002:5460` | 350 MHz | 64 MiB |
| `x600xt` (alias `rv380`) | Radeon X600 XT (RV380) | PCIe | `1002:3E50` | 500 MHz | 256 MiB |
| `x550xtx` | Radeon X550 XTX (RV410) | PCIe | `1002:5657` | 400 MHz | 128 MiB |
| `x700` (alias `rv410`) | Radeon X700 (RV410) | PCIe | `1002:5E4D` | 400 MHz | 128 MiB |
| `x700pro` | Radeon X700 PRO (RV410) | PCIe | `1002:5E4B` | 425 MHz | 128 MiB |
| `x700xt` | Radeon X700 XT (RV410) | PCIe | `1002:5E4A` | 475 MHz | 128 MiB |
| `x700m` | Mobility Radeon X700 (RV410/M26) | PCIe | `1002:5652` | 350 MHz | 128 MiB |
| `x850xt` (alias `r480`) | Radeon X850 XT (R480) | PCIe | `1002:5D52` | 520 MHz | 256 MiB |
| `x850xt-agp` (alias `r481`) | Radeon X850 XT AGP (R481) | AGP | `1002:4B49` | 520 MHz | 256 MiB |

- **Origen de los datos:** los PCI ID coinciden con `include/drm/drm_pciids.h`
  de Linux, y los nombres con `pci.ids`. Los relojes vienen de la VBIOS cuando
  la había; si no, son nominales.
- **VRAM:** ajusta `vgamem_mb=` a la memoria de tu tarjeta.
- **"Xpress 1250"** se usó para dos northbridges distintos: el RS600 (Intel) y
  el RS690 (AMD 690G). Para un equipo Athlon 64 X2, el correcto es el **RS690**.

Diferencias entre integradas y dedicadas:

- **TCL (vertex shaders por hardware):**
  - Las integradas (RS480/RS600/RS690) **no tienen TCL**: el driver ejecuta los
    vertex shaders en la CPU y el backend 3D solo recibe vértices transformados.
  - Las dedicadas **sí tienen TCL**: su ring llevará programas de vertex shader
    (PVS), que el futuro backend 3D tendrá que traducir.
- **Bus:** QEMU no tiene AGP.
  - Las tarjetas **AGP** se presentan como PCI, y el driver `radeon` usa
    entonces el GART PCI de la R100, que está emulado.
  - Las tarjetas **PCIe** se presentan como PCI Express en un bus PCIe
    (`-machine q35`) y usan el PCIe GART.
  - En la máquina `pc`, las PCIe también funcionan como PCI con el GART PCI.

## ¿Mi VBIOS está soportada?

```bash
python3 tools/romid.py bios/*.rom
```

Para cada ROM indica el PCI ID, el tipo (AtomBIOS o COMBIOS), el part number y
los relojes, y el `model=` exacto que hay que usar. Solo lee los ficheros.

Las ROM van en `bios/`, que está en `.gitignore`: son firmware propietario y no
se suben al repositorio.

## Qué implementa

### Núcleo portable (`src/`, `include/`)

- MMIO de 64 KiB con accesos de 8, 16 y 32 bits:
  - registros write-1-to-clear respetados en escrituras parciales;
  - `MM_INDEX`/`MM_DATA` a registros o VRAM, con protección contra
    auto-referencia.
- MC indirecto por familia:
  - RS480 `0x168/0x16C`, RS600 `0x70/0x74` y RS690 `0x78/0x7C`, con bit
    `WR_EN`;
  - dedicadas: espacio PCIe indirecto en `PCIE_INDEX/PCIE_DATA` (`0x30/0x34`).
- Espacio de direcciones GPU:
  - apertura de VRAM según `MC_FB_LOCATION` / `MCCFG_FB_LOCATION`;
  - GART RS400/RS690: PTE de 32 bits con flags R/W y bits 39:32;
  - GART RS600: tabla plana de PTE de 64 bits en VRAM;
  - PCIe GART de las dedicadas (`PCIE_TX_GART_*`): tabla en VRAM, PTE
    `addr >> 8 | addr[39:32] << 24 | R/W`;
  - GART PCI de la R100 (`AIC_*`): tabla en memoria del sistema, entrada =
    dirección de bus. Lo usan las tarjetas AGP y las PCIe en bus PCI; se elige
    automáticamente según cuál active el driver.
- R4xx: `GB_PIPE_SELECT` informa del número de quads (lo lee
  `r420_pipes_init`).
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
  - Híbrido PCI/PCIe: las tarjetas PCIe se presentan como endpoint PCI Express
    en un bus PCIe (`q35`); las integradas y las AGP son PCI convencional.
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
  Las dedicadas necesitan además traducir los vertex shaders (PVS).
- Texturas, render targets y la caché de superficies VRAM ↔ Vulkan.
- POST con VBIOS real: el código de la BIOS inicializa memoria y PLL y sondea
  registros de estado que todavía no se modelan (ver
  `docs/QEMU_INTEGRATION.md`). No se incluye ninguna VBIOS.
- AGP real: QEMU no emula un bus AGP; las tarjetas AGP funcionan como PCI.
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
