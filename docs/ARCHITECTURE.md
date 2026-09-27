# Arquitectura

```text
Windows / Linux invitado
        |
        | Catalyst / radeon DRM
        v
PCI 1002:791E (RS690) / 5954 (RS480) / 7941 (RS600)
AGP→PCI 1002:4153 (9550) / 4B49 (X850 XT AGP)
PCIe 1002:5B60 (X300) / 3E50 (X600 XT) / 5E4D (X700) / 5D52 (X850 XT) ...
        |
        +-- BAR0  aperture / framebuffer
        +-- BAR1  I/O alias
        +-- BAR2  MMIO Radeon
        |
+----------------------------------------------------+
| radeon-legacy core                                 |
|                                                    |
| MMIO ─┬─ MC indirecto ── FB location / GART         |
|       ├─ display: CRTC clásico | AVIVO D1 ──> scanout
|       ├─ IRQ: GEN_INT + AVIVO VBlank + SW_INT      |
|       ├─ motor 2D ─────────────────────> VRAM      |
|       └─ CP: ring ─> IB ─> PACKET0/1/3             |
|                 |          |                       |
|                 |          +──> estado 3D (siguiente fase)
|                 +──> GART ──> DMA del invitado     |
+----------------------------------------------------+
        |
        +--> software 2D (funcional)
        +--> backend Vulkan (pendiente: R300/R400 FS -> SPIR-V)
```

## Criterio de emulación

No se reproduce el chip ciclo a ciclo. La prioridad es la interfaz observable por
el driver: PCI, MMIO, memoria, ring, comandos, IRQ, fences y BIOS. El trabajo
gráfico reconstruido se ejecuta en el host.

## Familias frente a perfiles

- Un **perfil** es lo que ve el invitado: PCI ID, nombre y reloj nominal.
- Una **familia** (`RLGFamily`) define la interfaz de registros:

| Familia | MC indirecto | FB location | GART | Display |
|---|---|---|---|---|
| RS400 (RS480) | `0x168/0x16C`, WR_EN bit 8 | MMIO `0x148` | tabla 32 bits en sysmem | CRTC clásico |
| RS600 | `0x70/0x74`, WR_EN bit 23 | MC `0x04` | tabla plana 64 bits en VRAM | AVIVO |
| RS690 | `0x78/0x7C`, WR_EN bit 9 | MC `0x100` | tabla 32 bits en sysmem (RS400) | AVIVO |
| R300 (dedicadas RV350/RV370/RV380/RV410/R480/R481) | PCIe `0x30/0x34`, sin WR_EN | MMIO `0x148` | PCIe GART (tabla en VRAM) **o** GART PCI R100 (`AIC_*`, tabla en sysmem), el que active el driver | CRTC clásico |

Dentro de la familia R300, cada chip indica además su bus real (AGP o PCIe, que
decide si el adaptador añade la capability PCI Express) y si es R4xx (valor de
`GB_PIPE_SELECT`).

Todo lo demás (CP, 2D, parser PM4, futuro backend 3D) es común. Todas las
familias comparten el núcleo 3D de clase R300/R400. La diferencia está en el
TCL:

- **Integradas:** sin TCL por hardware, así que el backend recibirá vértices
  ya transformados.
- **Dedicadas:** con TCL, así que el backend tendrá que traducir también los
  vertex shaders (PVS).

## Siguiente fase: 3D

```text
PM4 3D_DRAW_* / registros R300 (0x2000-0x4fff)
  -> snapshot de estado (GA/SU/SC/RS/US/TX/RB3D/ZB)
  -> microcódigo US (fragment) -> IR -> SPIR-V (caché por hash)
  -> VkPipeline (dynamic state) + caché de superficies VRAM <-> VkImage
  -> vkCmdDraw
```
