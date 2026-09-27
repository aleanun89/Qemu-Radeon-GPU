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

## X300 / X700

- **Máquina:** usa `-machine q35`. Sin bus PCIe, `radeon` no selecciona el
  PCIe GART.
- **BIOS:** estas tarjetas suelen llevar VBIOS clásica (COMBIOS) en lugar de
  AtomBIOS. El driver la necesita igualmente para conectores y relojes.
- **Hito propio:** después del `ib test`, el siguiente paso de r300g en estas
  tarjetas es subir programas de vertex shader (TCL por hardware). Esos
  programas llegan al CP como escrituras a los registros `VAP_PVS_*`, que hoy
  solo se almacenan.

## BIOS

El overlay usa por defecto `vgabios-ati.bin`, que no contiene tablas ATOM. No se
distribuye firmware de ATI/AMD. QEMU permite probar una ROM propia con
`romfile=`.
