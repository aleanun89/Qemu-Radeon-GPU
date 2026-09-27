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

### Motor 3D R300/R400 (`src/r3d_*.c`)

Es un renderizador de referencia por software que ejecuta el pipeline del
hardware sobre la VRAM emulada. Todas las codificaciones siguen el driver r300
de Mesa (`src/gallium/drivers/r300` y su `compiler/`).

| Bloque | Implementado |
|---|---|
| **CP** | `3D_LOAD_VBPNTR`, `3D_DRAW_VBUF_2`, `3D_DRAW_IMMD_2`, `3D_DRAW_INDX_2` (índices inline, por `INDX_BUFFER` o por `VAP_PORT_IDX0`) |
| **VAP** | `PROG_STREAM_CNTL/_EXT` (FLOAT_1-4, BYTE, D3DCOLOR, SHORT_2/4, FLT16_2/4, signed/normalized, swizzles, write mask); modo TCL y modo bypass (IGP/SW TCL) |
| **PVS** (vertex shaders) | Intérprete completo de VE_* y ME_* (MAD, DP4, DST, SGE/SLT, CMP, FRC, ARL, EXP/LOG, EX2/LG2, RCP/RSQ, POW, SIN/COS, macro MADD); direccionamiento relativo por A0; subida por `PVS_VECTOR_INDX`/`UPLOAD_DATA`; constantes con base offset |
| **VTE / GA / SU** | División de perspectiva, viewport `SE_VPORT_*`, clipping homogéneo (w, near, far; espacio GL o DX), culling, flat/Gouraud con provoking vertex; triángulos, strips, fans, quads, quad strips, polígonos, líneas y puntos |
| **Puntos** | `GA_POINT_SIZE` y stuffing de coordenadas (`GB_ENABLE`, `GA_POINT_S0..T1`). Es el camino que usa el blitter de Mesa para clears y copias |
| **SC / RS** | Rasterizado en quads 2×2 con regla top-left; scissor y cliprects con `SC_CLIP_RULE`; interpoladores `RS_IP`/`RS_INST` con corrección de perspectiva hacia los temporales del FS |
| **US** (fragment shaders) | Pares RGB/Alpha, 3 fuentes con constantes float24, swizzles nativos, presub, MAD/DP3/DP4/D2A/MIN/MAX/CND/CMP/FRC/REPL_ALPHA, EX2/LG2/RCP/RSQ, output modifiers y clamp, escritura de profundidad; nodos de indirección; TEX/TXP/TXB/KIL; banking R400 (modo r390, 512 ALU) |
| **TX** (texturas) | Formatos R300/R400: 8-32 bpp, 565/1555/4444/2101010, 16F/32F, DXT1/3/5, con signo y sRGB. Swizzle por canal, 7 wrap modes, nearest/bilinear, mip nearest/linear con LOD por derivadas del quad, LOD bias, 3D y cubemaps. Layout de mips idéntico a `r300_texture_desc.c` (incluida la alineación del RS690) |
| **FG / ZB / RB3D** | Alpha test; Z de 16 y 24 bits; stencil de 8 bits a doble cara; blending con los 15 factores y 8 funciones; blend color; máscara de canales; formatos ARGB8888/1555/4444, RGB565, I8, UV88, ARGB16161616 (unorm/fp16) y ARGB32323232; varios render targets; `US_OUT_FMT`; CBZB clear |

**Superficies:** se guardan en lineal aunque tengan activados los bits de
tiling. Es coherente porque la CPU solo ve las superficies tiled a través de
los surface registers (que las presentan en lineal) y todos los demás accesos
pasan por esta GPU emulada.

**Validación** (`tests/test_3d.c`): los tests generan comandos como r300g y
comprueban píxeles contra valores calculados a mano. Entre otros:
- ALU contra aritmética exacta;
- offsets de mips contra las reglas de Mesa aplicadas a mano;
- clear del blitter con cliprect;
- CBZB clear;
- depth, stencil y blending;
- VBO con PVS.

Además, mutaciones deliberadas en CMP, float24, la regla del RS690 y la
orientación del culling hacen fallar los tests.

### Backend Vulkan del 3D (`src/vk_backend.c`, `src/vk_shader.c`, `src/rlg_spirv.c`)

El trabajo se divide como en un driver con SW TCL:

- **CPU (código ya verificado):** fetch de vértices, PVS, clipping, culling,
  expansión de puntos y líneas, y el mapeo RS.
- **GPU del host:** todo lo que ocurre por píxel.

Piezas:

- **Traductor US → SPIR-V:** reproduce una a una las construcciones del
  intérprete por software (pares RGB/Alpha, presub, swizzles, omod y clamp,
  CND/CMP, TEX/TXP/TXB con LOD implícito, KIL y alpha test con
  `OpDemoteToHelperInvocation`, escritura de profundidad). El generador de
  SPIR-V es propio y no tiene dependencias; sus opcodes están comprobados contra
  SPIRV-Headers.
- **Carga dinámica del loader** (`vulkan-1.dll` / `libvulkan.so.1`): para
  compilar solo hacen falta las cabeceras de Vulkan.
- **Mapeo de estado:**
  - formatos de colorbuffer con el orden de canales de `US_OUT_FMT`;
  - blending, máscara de canales, Z16 y Z24S8 (sobre `D32_SFLOAT_S8_UINT`
    porque AMD no soporta D24S8), stencil a doble cara;
  - texturas R300 (incluido BC1-3 con el swizzle DXTC del R400, sRGB y float),
    con cada nivel mip leído desde su posición en el layout de Mesa;
  - samplers, scissor y cliprect.
- **Fallback por draw:** cualquier estado que no se pueda reproducir con
  exactitud (MRT, CBZB, texturas 3D o cubemaps, formatos con signo, reglas de
  cliprect complejas…) se dibuja por software. La corrección nunca empeora.
- **Coherencia (fase 1):** cada draw sube la región afectada de colorbuffer y
  depth desde la VRAM emulada y la devuelve al terminar.

**Verificación en una RX 9070 XT:** los 14 grupos de `tests/test_3d.c`
pasan igual con `test-3d vulkan` (24 draws en GPU y un fallback esperado, el
CBZB). Además, las mutaciones en el traductor o el backend (orden de CMP,
presub, swizzle BC, stencil ops, offsets de mips) hacen fallar los tests.

**Rendimiento** (`tests/bench_3d.c`, quad texturizado de 640×480 a pantalla
completa):

| Backend | ms/draw | Mpíxel/s |
|---|---:|---:|
| Software | 101,8 | 3,0 |
| Vulkan (fase 1, versión inicial) | 5,25 | 58,5 |
| Vulkan (fase 1 + staging cacheado y caché de imágenes/samplers) | 0,33 | 935 |
| Vulkan con lotes, 16 draws por kick del CP | 0,080 | 3.858 |

Con quads de 64×64 (más parecido a un juego: muchos draws pequeños), el número
de draws por segundo pasa de ~4.750 con un draw por kick a **~17.500** con 16
por kick (`bench-3d vulkan 4000 16 64`).

Desglose medido de la versión inicial: 3,5 ms eran la lectura desde la CPU del
buffer de staging, asignado en memoria sin caché, y 0,65 ms la creación y
destrucción de imágenes y samplers. Después, el coste estaba en esperar el fence
en cada draw (~0,22 ms). Ahora los draws de una misma ejecución del CP van en un
solo lote: el render target se queda en la GPU y se copia de vuelta a la VRAM
una vez (ver `docs/ARCHITECTURE.md`).

Como referencia, una X300 SE (RV370, 4 píxeles/ciclo a 325 MHz) tiene un
fill rate teórico de 1,3 Gpíxel/s. El límite práctico ya no es el píxel sino el
número de draws por frame.

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

## Lo que falta

- **Fase 2 del backend Vulkan, el paso que da el rendimiento:**
  - superficies residentes en la GPU (colorbuffer, depth, texturas), que se
    escriben de vuelta a la VRAM solo cuando la CPU o el scanout las leen;
  - caché de texturas;
  - lotes de draws sin esperar un fence en cada uno;
  - un hilo worker de comandos (patrón de `dxvk_cs`) para liberar la vCPU;
  - traducir también el PVS a SPIR-V, para no hacer el TCL en la CPU.
- **Rendimiento del 3D por software:** el rasterizado es por píxel y se ejecuta
  en el hilo de la vCPU. Es correcto, pero lento; queda como referencia y como
  fallback.
- **Aspectos del 3D sin implementar:**
  - control de flujo del PVS (loops/jumps de `PVS_FLOW_CNTL`, se ejecuta en
    lineal);
  - predicación;
  - HyperZ (ZMASK/HiZ; Mesa solo lo activa en R300-R400 con
    `RADEON_HYPERZ=1`);
  - MSAA, polygon offset y stipple;
  - filtrado anisotrópico (se hace bilineal);
  - formatos YUV, CxV8U8, ATI2N y W24_FP;
  - Shader Model 3 y R500 (X1300 y posteriores).
- **Pendiente de confirmar con hardware real:** la orientación de
  `SU_CULL_MODE` (se toma CCW como visto en pantalla con y hacia abajo) y la
  semántica exacta de D2A.
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

- El backend Vulkan se activa si CMake encuentra `vulkan/vulkan.h`, ya sea en
  el sistema, en `VULKAN_SDK` o en `-DRLG_VULKAN_INCLUDE=<dir>`. No hace falta
  enlazar contra Vulkan.
- El test `3d-vulkan` se salta (código 77) si no hay una GPU Vulkan 1.3.

Sin CMake ni compilador de C instalados (por ejemplo, en Windows), sirve
`zig cc` desde pip:

```bash
python -m pip install --user ziglang
python -m ziglang cc -std=c11 -O2 -Iinclude -I<vulkan-headers> -DRLG_HAVE_VULKAN=1 src/chip.c src/device.c src/memory.c src/irq.c src/display.c src/engine2d.c src/cp.c src/mmio.c src/r3d_state.c src/r3d_vertex.c src/r3d_fs.c src/r3d_tex.c src/r3d_raster.c src/r3d_draw.c src/rlg_spirv.c src/vk_shader.c src/vk_backend.c tests/test_3d.c -o test-3d.exe
```

```bash
test-3d.exe vulkan
```

Para medir el rendimiento, `tests/bench_3d.c` acepta `bench-3d software|vulkan [draws]`.

## Integración QEMU

Ver `docs/QEMU_INTEGRATION.md`. Resumen:

```bash
python3 qemu/apply_to_qemu_11_1.py /ruta/qemu-11.1.x
```

```bash
qemu-system-x86_64 -vga none -device radeon-legacy-vga,model=rs690 -m 2048 -drive file=winxp.qcow2,if=ide
```

```bash
qemu-system-x86_64 -machine q35 -vga none -device radeon-legacy-vga,model=x700,bus=pcie.0,backend=vulkan -m 2048 -drive file=linux.qcow2
```

`backend=vulkan` ejecuta el 3D en la GPU del host (por defecto es `software`).
Si Vulkan no está disponible, QEMU avisa y usa software.

Cambios respecto a v3: ver `CHANGELOG.md`.
