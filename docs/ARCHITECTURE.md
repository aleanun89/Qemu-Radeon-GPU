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
|       ├─ registros 3D ─> PVS/US program memory     |
|       └─ CP: ring ─> IB ─> PACKET0/1/3             |
|                 |          |                       |
|                 |          +──> motor 3D r3d_* ──> VRAM / GART
|                 +──> GART ──> DMA del invitado     |
+----------------------------------------------------+
        |
        +--> 2D y 3D por software (referencia, funcional)
        +--> backend Vulkan (siguiente fase)
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

Todo lo demás (CP, 2D, parser PM4, motor 3D) es común. Todas las familias
comparten el núcleo 3D de clase R300/R400. La diferencia está en el TCL:

- **Integradas:** sin TCL por hardware. El driver pone `VAP_CNTL_STATUS.TCL_BYPASS`
  y envía vértices ya transformados, que el VAP coloca en slots fijos (0 =
  posición, 2-5 = colores, 6-13 = coordenadas de textura).
- **Dedicadas:** con TCL, así que el PVS ejecuta el vertex shader y sus salidas
  se empaquetan en orden (posición, tamaño de punto, colores, coordenadas de
  textura), según `VAP_OUTPUT_VTX_FMT_0/1`.

## Motor 3D (`src/r3d_*.c`, `include/radeon_r3d.h`)

```text
CP PACKET3 3D_DRAW_*_2 / LOAD_VBPNTR / INDX_BUFFER        r3d_draw.c
  └─ por vértice: fetch (PROG_STREAM_CNTL) ─> PVS o bypass  r3d_vertex.c
       └─ ensamblado de primitivas, clipping homogéneo,
          culling, puntos (stuffing), líneas                 r3d_raster.c
            └─ VTE (divide + viewport) ─> quads 2x2 con regla top-left
                 └─ RS: interpoladores ─> temporales del FS
                      └─ US: programa de fragmentos (quad)   r3d_fs.c
                           └─ TX: sampling con LOD por quad  r3d_tex.c
                 └─ FG alpha test ─> ZB depth/stencil ─> RB3D blend ─> VRAM
```

- **Registros:**
  - el estado 3D vive en el almacén de registros normal;
  - solo tienen efectos laterales la memoria del PVS (`VAP_PVS_VECTOR_INDX_REG`
    / `UPLOAD_DATA`), la memoria del US con banking R400 (`US_ALU_*`,
    `US_TEX_INST`, `R400_US_CODE_BANK`) y el puerto de índices
    `VAP_PORT_IDX0` (`r3d_state.c`).
- **Programa de fragmentos:** se decodifica una vez por draw (`r3d_fs_build`) y
  se ejecuta en quads 2×2, de modo que las instrucciones TEX calculan el LOD
  con derivadas reales, igual que el hardware.
## Backend Vulkan

```text
r3d_draw_prims ──> triángulos recogidos (R3DTri, ya recortados y en pantalla)
                     │
                     ├─ r3d_vk_draw() == 0 ──> hecho en la GPU del host
                     └─ < 0 (estado no soportado) ──> scan_triangle() por software

r3d_vk_draw (vk_backend.c)
  1. comprobar y mapear estado: formato de CB + US_OUT_FMT, blend, Z/stencil,
     texturas (formato, swizzle, wrap, filtros), scissor/cliprect
  2. vk_fs_translate (vk_shader.c) ──> SPIR-V ──> VkPipeline (caché por clave)
  3. subir: filas afectadas de CB/ZB y niveles de textura desde la VRAM
  4. vértices: posición en NDC·w + salidas RS como varyings
  5. dynamic rendering, draw, copiar de vuelta a la VRAM (+ dirty)
```

- **Loader:** Vulkan se carga en tiempo de ejecución; basta con las
  cabeceras.
- **Fase 1:** sincroniza en cada draw.
- **Fase 2:** superficies residentes en la GPU, escritas de vuelta a la VRAM
  solo cuando se leen, lotes de draws sin esperar fence, un hilo worker y el
  PVS traducido a SPIR-V.
