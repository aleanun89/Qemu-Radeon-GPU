# Cambios

## Tarjetas de las VBIOS aportadas (9550, X300 SE, X550, X600, X850)

A partir de las ROM de `bios/` (locales, no versionadas):

- **Perfiles nuevos:**

  | Modelo | Chip | PCI ID | Bus |
  |---|---|---|---|
  | `r9550` | RV350 | `1002:4153` | AGP |
  | `x300se` | RV370 | `1002:5B62` | PCIe |
  | `x600xt` | RV380 | `1002:3E50` | PCIe |
  | `x550xtx` | RV410 | `1002:5657` | PCIe |
  | `x850xt` | R480 | `1002:5D52` | PCIe |
  | `x850xt-agp` | R481 | `1002:4B49` | AGP |

  Relojes tomados del FirmwareInfo de las AtomBIOS cuando existían.
- **GART PCI de la R100** (`AIC_CNTL/AIC_PT_BASE/AIC_LO_ADDR/AIC_HI_ADDR`,
  según `r100_pci_gart_enable`):
  - es lo que usa `radeon` con tarjetas AGP (QEMU no tiene bus AGP) y con
    tarjetas PCIe en un bus PCI convencional;
  - el núcleo elige el GART que haya activado el driver;
  - resultado: las PCIe ya **no requieren `q35`**, y el adaptador deja de
    mostrar el aviso.
- **Cambios de modelo:**
  - la familia `RLG_FAMILY_R300_PCIE` pasa a llamarse `RLG_FAMILY_R300`;
  - cada chip declara su bus real (`RLGBus`: IGP/AGP/PCIe); el adaptador solo
    añade la capability PCI Express a las tarjetas PCIe;
  - el campo `integrated` se sustituye por `bus`;
  - el estado del GART incluye `kind`.
- **R4xx** (RV410, R480, R481): `GB_PIPE_SELECT` informa de los quads de pipes
  (R480 = 4, RV410 = 2). `r420_pipes_init` lo lee al arrancar.
- `rlg_profile_models()`: lista única de modelos para los mensajes de error de
  la demo y del adaptador.
- **`tools/romid.py`:**
  - identifica una VBIOS (PCI ID, AtomBIOS/COMBIOS, part number, relojes);
  - dice qué `model=` usar;
  - lee la tabla de perfiles de las fuentes C.
- **Tests nuevos:**
  - un perfil por cada ROM aportada;
  - GART PCI con 9550, X850 XT AGP y X700 en bus PCI (ring + IB por GART);
  - `GB_PIPE_SELECT`.

## Tarjetas dedicadas X300 y X700 (PCIe)

- **Perfiles nuevos:**

  | Modelo | Chip | PCI ID |
  |---|---|---|
  | `x300` | RV370 | `1002:5B60` |
  | `x300m` | RV370/M22 | `1002:5460` |
  | `x700` | RV410 | `1002:5E4D` |
  | `x700pro` | RV410 | `1002:5E4B` |
  | `x700xt` | RV410 | `1002:5E4A` |
  | `x700m` | RV410/M26 | `1002:5652` |

  IDs tomados de `drm_pciids.h` (Linux: `CHIP_RV380` / `CHIP_RV410`) y nombres
  de `pci.ids`.
- **Familia nueva `RLG_FAMILY_R300_PCIE`:**
  - CRTC clásico y `MC_FB_LOCATION` en MMIO;
  - espacio indirecto `PCIE_INDEX/PCIE_DATA` en `0x30/0x34`;
  - PCIe GART (`PCIE_TX_GART_*`) con tabla en VRAM y PTE
    `addr >> 8 | addr[39:32] << 24`, según `rv370_pcie_gart_enable` y
    `rv370_pcie_gart_get_page_entry`.
- `hw_tcl=true` para estos chips: tienen vertex shaders por hardware.
- **Adaptador QEMU híbrido PCI/PCIe:** con X300/X700 en un bus PCIe (`q35`) se
  añade la capability PCI Express. Es necesario porque `radeon` solo elige el
  PCIe GART si `pci_is_pcie()`. En un bus PCI convencional se avisa.
- `rlg_family_is_avivo()` sustituye las comprobaciones `family != RS400`.
- Test nuevo `test_x700_pcie`:
  - secuencia de `rv370_pcie_gart_enable` y PTE por encima de 4 GiB;
  - límite de la última página y acceso a `0x30/0x34`;
  - ring + IB en GART + fence;
  - scanout clásico, y AVIVO ignorado en el RV410.

## Revisión de v3

Referencia usada para verificar registros y formatos: Linux
`drivers/gpu/drm/radeon/` (`radeon_reg.h`, `r500_reg.h`, `r100.c`, `rs400.c`,
`rs600.c`, `rs690.c`) e `include/drm/drm_pciids.h`.

## Identidad del chip

- **Nuevo perfil RS690 (`1002:791E`) / RS690M (`791F`)**: la Radeon X1250 de los
  chipsets AMD 690G/M690, la que corresponde a un Athlon 64 X2. Ahora es el
  perfil por defecto.
- RS600 (`7941`/`7942`) se mantiene, pero identificado como chipset para Intel.
- Nuevos datos por chip: familia de registros, perfil de pixel shader
  (`ps_2_0`/`ps_2_b`) y `hw_tcl=false`.
- `model=` desconocido → error en lugar de caer en silencio al RS480.

## Fallos corregidos

| Área | Problema en v3 | Corrección |
|---|---|---|
| Seguridad | `MM_INDEX=4` + acceso a `MM_DATA` provocaba **recursión infinita** (el invitado podía tumbar QEMU) | Se rechaza cualquier índice ≤ `MM_DATA+3`, como en `ati.c` |
| IRQ | Una escritura parcial (8/16 bits) en `GEN_INT_STATUS` reconocía también los bits no escritos | Los registros W1C solo aplican los bytes escritos |
| IRQ | Se levantaba una IRQ "CP" (bit 7) tras cada proceso del ring; no existían fences | `SW_INT_FIRE` (bit 26) → `SW_INT` (bit 25), como emite `r100_fence_ring_emit` |
| CP | Tamaño del ring `1 << ((cntl>>1)&0x3f)` | `2 << RB_BUFSZ` (bits 5:0), como `r100_cp_init` |
| CP | Registro de PACKET0 con máscara `0x7fff` (incluía el bit `ONE_REG_WR`) | `(h & 0x1fff) << 2` y soporte de `ONE_REG_WR` |
| CP | Sin indirect buffers; `ib test` del driver fallaría | `CP_IB_BASE`/`CP_IB_BUFSZ` (un nivel) |
| CP | Writeback de `RPTR` con dirección física y sin respetar `RB_NO_UPDATE` | Dirección GPU (vía GART) y respeta `RB_NO_UPDATE` |
| CP | `RPTR` escribible directamente; paquetes truncados desincronizaban el ring | `RB_RPTR_WR` + `RB_RPTR_WR_ENA`; espera a que el paquete esté completo |
| CP | Se ejecutaba aunque el CP estuviera deshabilitado | Respeta `CSQ_CNTL` (`PRIDIS`) |
| GART | Bit 0 de la PTE tratado como "válido" (en RS400 es `UNSNOOPED`); tabla por `AGP_BASE`/`AGP_CNTL` | Mecanismo real: `GART_BASE` y `AGP_ADDRESS_SPACE_SIZE` en el MC indirecto, PTE con flags R/W y bits 39:32 |
| Memoria | Direcciones GPU 0..VRAM tratadas como VRAM, ignorando `MC_FB_LOCATION` | Traducción según la FB location de cada familia |
| Display | `CRTC_PITCH` interpretado en unidades de 8 bytes | Unidades de 8 **píxeles** |
| Display | `CRTC_DISPLAY_DIS` buscado en `CRTC_GEN_CNTL` bit 23 | Está en `CRTC_EXT_CNTL` bit 10 |
| Display | RS600 sin display AVIVO: con el driver real no habría imagen | Scanout AVIVO D1 + VBlank AVIVO |
| 2D | `*_Y_X`, `DST_HEIGHT_WIDTH`, `SC_TOP_LEFT` y `SC_BOTTOM_RIGHT` decodificados al revés | Orden de campos correcto |
| 2D | Offsets tratados como offset en VRAM | Son direcciones MC; se traducen |
| 2D | Se ignoraba el datatype y el ROP de `DP_GUI_MASTER_CNTL`; `DST_HEIGHT` no disparaba | Implementado, junto con `DEFAULT_PITCH_OFFSET` |
| QEMU | La comprobación de la aperture se hacía después de crear el core (fuga); VBE se reprogramaba en cada escritura de CRTC | Validación previa; VBE solo cambia si cambia el modo, y se devuelve el control al VGA al apagar el CRTC |

## Estructura

- Formato legible del código (las fuentes v3 estaban minificadas en una línea).
- `radeon_legacy_int.h` con los prototipos internos (antes eran `extern`
  sueltos, lo que da `-Wmissing-prototypes` en QEMU).
- El overlay ya no duplica el núcleo: `apply_to_qemu_11_1.py` copia `src/` e
  `include/` directamente. Reemplaza el bloque de meson de v3 si lo encuentra.
- Tests ampliados de 3 a 9 grupos: MC/GART RS690 y RS600, ring + IB + fence +
  writeback, AVIVO + VBlank, W1C, guarda de `MM_INDEX` y decodificación 2D.
