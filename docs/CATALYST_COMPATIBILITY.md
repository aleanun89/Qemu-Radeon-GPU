# Compatibilidad con drivers reales

Exponer `1002:791E` (RS690) es necesario, pero no suficiente, para que un driver
real inicialice la aceleración.

## Orden recomendado

Empieza con un invitado **Linux + `radeon` KMS + Mesa r300g**. Todo es código
abierto y puedes comparar cada acceso MMIO con lo que hace el driver. Catalyst
para XP es una caja negra: déjalo para cuando Linux funcione.

1. QEMU/BIOS enumera el dispositivo PCI con el ID correcto.
2. El invitado arranca en VGA genérico.
3. `radeon` encuentra la VBIOS (necesita AtomBIOS para RS600/RS690).
4. Programación MC: FB location y MC idle.
5. GART: `PCIE GART of 32M enabled`.
6. CP: pasan `ring test` (scratch) e `ib test` (indirect buffer + fence).
   *Implementado en v4; los tests del núcleo reproducen esas secuencias.*
7. KMS: modo AVIVO D1 estable y consola fbdev.
8. Mesa r300g con `LIBGL_ALWAYS_SOFTWARE=0`: los draws llegan al CP.
   *Aquí empieza el trabajo del backend 3D.*
9. Windows XP + Catalyst: 2D, DirectDraw, D3D9.

## Qué registrar cuando algo falle

Con `verbose=on -d unimp`:

- escrituras MC a registros no modelados;
- PACKET3 no manejados;
- fallos de fetch del CP;
- IB rechazados.

Añade trazas propias de:

- offset MMIO, tamaño y valor;
- `RPTR`/`WPTR` y `CSQ_CNTL`;
- `GEN_INT_STATUS`/`GEN_INT_CNTL`, `DxMODE_INT_MASK` y `DISP_INTERRUPT_STATUS`;
- `gart.faults` y `cp.faults`.

## Dedicadas (9550, X300, X550, X600, X700, X850)

- **Máquina:**
  - Tarjetas PCIe: `-machine q35` reproduce el camino real (PCIe GART). En
    `-machine pc` funcionan como PCI con el GART PCI de la R100.
  - Tarjetas AGP (9550, X850 XT AGP): QEMU no tiene AGP, así que siempre van
    como PCI. `radeon` lo soporta. Catalyst para XP también instala en tarjetas
    AGP sin puente AGP, pero sin AGP no hay transferencias rápidas, y alguna
    versión podría comprobar el bus.
- **BIOS:** según las ROM analizadas, las R3xx llevan **COMBIOS** (código x86)
  y las R4xx llevan **AtomBIOS**:

  | BIOS | Tarjetas |
  |---|---|
  | COMBIOS | 9550, X300, X300 SE, Mobility X300, X600 XT |
  | AtomBIOS | X550 XTX, X700 PRO, X850 XT, X850 XT AGP |

  El driver la necesita en ambos casos para conectores y relojes.
  `tools/romid.py` indica el tipo de cada ROM.
- **Hito propio:** después del `ib test`, el siguiente paso de r300g en estas
  tarjetas es subir programas de vertex shader (TCL por hardware). Esos
  programas llegan al CP como escrituras a los registros `VAP_PVS_*`, que hoy
  solo se almacenan.

## BIOS

El overlay usa por defecto `vgabios-ati.bin`, que no contiene tablas ATOM. No se
distribuye firmware de ATI/AMD. QEMU permite probar una ROM propia con
`romfile=`.
