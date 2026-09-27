# Arquitectura

```text
Windows / Linux invitado
        |
        | Catalyst / radeon DRM
        v
PCI 1002:791E (RS690) / 5954 (RS480) / 7941 (RS600)
PCIe 1002:5B60 (X300) / 5E4D (X700)
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
| R300 PCIe (X300 RV370, X700 RV410) | PCIe `0x30/0x34`, sin WR_EN | MMIO `0x148` | PCIe GART, tabla 32 bits en VRAM | CRTC clásico |

Todo lo demás (CP, 2D, parser PM4, futuro backend 3D) es común. Todas las
familias comparten el núcleo 3D de clase R300/R400. La diferencia está en el
TCL:

- **Integradas:** sin TCL por hardware, así que el backend recibirá vértices
  ya transformados.
- **X300/X700:** con TCL, así que el backend tendrá que traducir también los
  vertex shaders (PVS).

## Siguiente fase: 3D

```text
PM4 3D_DRAW_* / registros R300 (0x2000-0x4fff)
  -> snapshot de estado (GA/SU/SC/RS/US/TX/RB3D/ZB)
  -> microcódigo US (fragment) -> IR -> SPIR-V (caché por hash)
  -> VkPipeline (dynamic state) + caché de superficies VRAM <-> VkImage
  -> vkCmdDraw
```
