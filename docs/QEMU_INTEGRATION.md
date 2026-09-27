# Integración con QEMU 11.1.x

El adaptador crea un dispositivo PCI llamado `radeon-legacy-vga`.

QEMU se encarga de lo que ya resuelve bien: VGA legado, consola gráfica, dirty
tracking y mapeo PCI. El núcleo Radeon se encarga de MMIO, MC, GART, CP, 2D,
display y los perfiles de chip.

El uso de la API está contrastado con `hw/display/ati.c` y `vga.c` de QEMU
master (11.1.50). **El adaptador no se ha compilado dentro de un árbol QEMU
completo**: ver `BUILD_VALIDATION.json`.

## 1. Dependencias

### Linux (Debian/Ubuntu)

```bash
sudo apt install build-essential ninja-build pkg-config python3-venv \
  libglib2.0-dev libpixman-1-dev libgtk-3-dev libsdl2-dev
```

### Windows (host)

QEMU se compila en Windows con MSYS2 (entorno UCRT64):

```bash
pacman -S --needed git base-devel mingw-w64-ucrt-x86_64-toolchain \
  mingw-w64-ucrt-x86_64-meson mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-glib2 mingw-w64-ucrt-x86_64-pixman \
  mingw-w64-ucrt-x86_64-SDL2 mingw-w64-ucrt-x86_64-gtk3 python
```

- Configura con `--target-list=x86_64-softmmu,i386-softmmu --enable-whpx`
  (Hyper-V/WHPX) en lugar de `--enable-kvm`.
- Alternativa: compilar y ejecutar dentro de WSL2 con KVM.

## 2. Aplicar el overlay

```bash
python3 qemu/apply_to_qemu_11_1.py /ruta/qemu-11.1.x
```

El script es idempotente:

- copia `hw/display/radeon-legacy-vga.c`;
- copia el núcleo desde `src/` e `include/` a `hw/display/radeon_legacy/`;
- añade `CONFIG_RADEON_LEGACY_VGA` a `hw/display/Kconfig`;
- añade (o reemplaza) el bloque de fuentes en `hw/display/meson.build`.

No toca `ati.c`: `ati-vga` sigue disponible.

## 3. Compilar

```bash
mkdir -p /ruta/qemu/build-radeon && cd /ruta/qemu/build-radeon
../configure --target-list=x86_64-softmmu,i386-softmmu --enable-kvm --enable-pixman
ninja
./qemu-system-x86_64 -device help | grep radeon-legacy
```

También está `qemu/build_qemu_example.sh /ruta/qemu`.

## 4. Ejemplo: Athlon 64 X2 3800+ con 690G / Radeon X1250

```bash
./qemu-system-x86_64 -machine pc -smp 2,sockets=1,cores=2 -m 2048 -cpu Opteron_G2,vendor=AuthenticAMD,family=15,model=75,stepping=2,model-id="AMD Athlon(tm) 64 X2 Dual Core Processor 3800+",-svm -vga none -device radeon-legacy-vga,model=rs690,vgamem_mb=256 -drive file=winxp.qcow2,if=ide
```

- Con KVM, añade `-accel kvm` (y quita `+3dnow` si lo usas); en un host Windows,
  `-accel whpx`.
- Variantes:
  - `model=rs690m`: portátil M690;
  - `model=rs480`: Xpress 200;
  - `model=rs600`: Xpress 1250 de plataforma Intel.

## 4b. Ejemplo: tarjeta dedicada X300 / X700 (PCIe)

```bash
./qemu-system-x86_64 -machine q35 -smp 2,sockets=1,cores=2 -m 2048 -cpu Opteron_G2,vendor=AuthenticAMD,family=15,model=75,stepping=2,model-id="AMD Athlon(tm) 64 X2 Dual Core Processor 3800+",-svm -vga none -device radeon-legacy-vga,model=x700,vgamem_mb=256,bus=pcie.0 -drive file=disk.qcow2
```

- **`q35` es obligatorio para el camino correcto.**
  - El driver `radeon` activa `RADEON_IS_PCIE` solo si `pci_is_pcie()` (ver
    `radeon_kms.c`). Con ese flag, el RV370/RV410 usa el PCIe GART emulado.
  - En `-machine pc` el dispositivo queda como PCI convencional: el driver
    intentaría el GART PCI de la R100, que no está emulado, y QEMU lo avisa.
- **Bus:** `bus=pcie.0` (el bus raíz) mantiene el rango VGA legado accesible.
  Detrás de un `pcie-root-port`, el enrutado VGA depende del bridge.
- **Windows XP en `q35`:** XP no trae driver AHCI; necesitas integrarlo en la
  instalación.
- **Modelos:** `x300`, `x300m`, `x700`, `x700pro`, `x700xt`, `x700m`.

## 5. BARs

| BAR | Tipo | Uso |
|---|---|---|
| 0 | memoria prefetchable | aperture lineal. La VRAM de `VGACommonState` está al inicio y se comparte con el núcleo sin copias |
| 1 | I/O | alias de los primeros 256 bytes del MMIO (`MM_INDEX`/`MM_DATA` para la VGA BIOS) |
| 2 | memoria | MMIO Radeon de 64 KiB |

## 6. Memoria vista por la GPU

- **VRAM**: en `[FB_START, FB_TOP]`, según la familia:
  - RS480: `MC_FB_LOCATION` (MMIO `0x148`);
  - RS690: `MCCFG_FB_LOCATION` (MC `0x100`);
  - RS600: `MC_FB_LOCATION` (MC `0x04`).

  X300/X700: `MC_FB_LOCATION` (MMIO `0x148`), como el RS480.

  Tras un reset vale `0 .. VRAM-1` hasta que el firmware o el driver la programen.
- **GART RS480/RS690** (Linux `rs400_gart_enable`):
  - tabla en memoria del sistema, en `GART_BASE` (MC `0x2C`);
  - activación y tamaño en `AGP_ADDRESS_SPACE_SIZE` (MC `0x38`);
  - aperture en `MC_AGP_LOCATION` (RS480) o `MCCFG_AGP_LOCATION` (RS690);
  - PTE de 32 bits: `addr[31:12] | addr[39:32] << 4 | READ(bit 3) | WRITE(bit 2)`.
- **GART RS600** (`rs600_gart_enable`):
  - tabla plana de PTE de 64 bits **en VRAM**, en `MC_PT0_CONTEXT0_FLAT_BASE_ADDR`;
  - activa con `MC_CNTL1` bit 26 + `MC_PT0_CNTL` bit 0.
- **PCIe GART X300/X700** (`rv370_pcie_gart_enable`):
  - registros indirectos por `PCIE_INDEX/PCIE_DATA` (`0x30/0x34`):
    `TX_GART_CNTL` (bit 0 = enable), `TX_GART_BASE` (tabla en VRAM),
    `TX_GART_START_LO` y `TX_GART_END_LO` (inicio de la última página);
  - PTE de 32 bits: `addr[31:12]` en bits 23:4, `addr[39:32]` en 31:24,
    READ = bit 3, WRITE = bit 2.
- Las lecturas y escrituras de la GPU a memoria del sistema pasan por
  `pci_dma_read`/`pci_dma_write`. Por eso respetan el bit de *bus master* del
  espacio de configuración PCI.

## 7. Display y VBlank

- **Scanout**: el adaptador llama a `rlg_is_display_reg()` tras cada escritura
  MMIO. Si el modo cambió, lo refleja en VBE.
  - RS480: CRTC clásico.
  - RS600/RS690: AVIVO D1; si D1 está apagado, se usa el CRTC clásico que
    programa la VGA BIOS.
  - Con el CRTC apagado, la pantalla vuelve al VGA.
- **VBlank**: temporizador virtual a 60 Hz.
  - RS480: `GEN_INT_STATUS` bit 0, habilitado en `GEN_INT_CNTL`.
  - RS600/RS690: `DISP_INTERRUPT_STATUS` y `D1MODE_VBLANK_STATUS`, habilitado
    en `DxMODE_INT_MASK`; ACK escribiendo `D1MODE_VBLANK_ACK`.
- **Fences**: `SW_INT_FIRE` escrito en `GEN_INT_STATUS`, habilitado con
  `GEN_INT_CNTL` bit 25.

## 8. BIOS

- La ROM por defecto es `vgabios-ati.bin`, la SeaVGABIOS de QEMU para `ati-vga`.
  Sirve para POST y VGA, pero **no contiene tablas AtomBIOS**.
- `radeon` KMS en RS600/RS690 exige AtomBIOS, y Catalyst también. Si tienes una
  ROM obtenida legalmente de tu propia placa 690G:

```text
-device radeon-legacy-vga,model=rs690,romfile=/ruta/rs690.rom
```

Espera que las tablas ATOM toquen PLL, encoders y registros MC que todavía no
están modelados (van al almacén genérico de registros).

## 9. Depuración

```bash
./qemu-system-x86_64 -vga none -device radeon-legacy-vga,model=rs690,verbose=on -d guest_errors,unimp -D qemu-radeon.log -m 2048 -drive file=disk.qcow2,if=ide
```

Con `verbose=on` y `-d unimp` se registra:

- escrituras MC a registros no modelados;
- PACKET3 no manejados;
- fallos de fetch del CP;
- IB rechazados;
- ROPs 2D no soportados;
- accesos inválidos por `MM_DATA`.

Invitado Linux:

```bash
lspci -nn
dmesg | grep -Ei 'radeon|drm|gart|ring'
```

En Windows XP: Administrador de dispositivos → Adaptadores de pantalla →
Propiedades → Detalles → Id. de hardware. Debe mostrar
`PCI\VEN_1002&DEV_791E` para `rs690`.

## 10. Limitaciones del adaptador

- El CP se ejecuta de forma síncrona en el hilo de la vCPU que escribe `WPTR`.
  Es simple y correcto, pero un ring grande bloquea esa vCPU mientras se procesa.
- No hay estado de migración (como `ati-vga`): no uses snapshots ni `savevm`
  con este dispositivo.
- Sin I2C/DDC: el driver no obtendrá EDID.
