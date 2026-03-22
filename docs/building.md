# Building FRANK OS

## Prerequisites

1. **Pico SDK** (version 2.0+): [github.com/raspberrypi/pico-sdk](https://github.com/raspberrypi/pico-sdk)
2. **ARM GCC toolchain:** `arm-none-eabi-gcc`
3. **CMake** 3.13+
4. **Python 3** (for icon conversion tools)
5. **picotool** (for flashing)

Set the SDK path:
```bash
export PICO_SDK_PATH=/path/to/pico-sdk
```

## Building the Firmware

```bash
git clone --recursive https://github.com/rh1tech/frankos.git
cd frankos
./build.sh
```

The build script creates a clean `build/` directory, runs CMake, and compiles with `make -j4`. Output files appear in `build/`:
- `frankos.elf` — debug ELF
- `frankos.uf2` — flashable UF2 image

### CMake Options

Edit `CMakeLists.txt` to adjust:

| Option | Default | Description |
|--------|---------|-------------|
| `PSRAM_SPEED` | `166` | PSRAM max frequency in MHz (empty string to disable HW init) |
| `FLASH_SPEED` | `66` | Flash chip max frequency in MHz |
| `CPU_CLOCK_MHZ` | `252` | CPU clock (set via `OVERCLOCKING` define) |

## Building Applications

```bash
cd apps
./build_apps.sh
```

Each app in `apps/source/*/` is built independently using its own `CMakeLists.txt`. Compiled binaries and `.inf` metadata are placed directly in `sdcard/fos/`.

To build a single app:
```bash
cd apps/source/notepad
mkdir build && cd build
cmake ..
make -j4
```

## Flashing

```bash
# Flash the most recent build
./flash.sh

# Flash a specific file
./flash.sh build/frankos.elf
./flash.sh release/frankos_m2_1_00.uf2
```

The flash script uses `picotool` to load firmware and reboot the device. Hold BOOTSEL on the board to enter USB mass storage mode if picotool can't connect.

## Creating a Release

```bash
./release.sh
```

Prompts for a version number (MAJOR.MINOR format), updates `version.txt`, builds, and copies the UF2 to `release/frankos_m2_<version>.uf2`.

## SD Card Setup

1. Format an SD card as FAT32
2. Copy the contents of `sdcard/` to the root of the SD card:
   ```
   /fos/       App binaries, .inf files, .ico icons
   /mos2/      MOS2 compatibility utilities
   /uf2/       User firmware files (.uf2, .m2p2) — flashable from Start > Firmware
   ```
3. Insert the card — it is auto-mounted at boot

## Project Structure

```
frankos/
  CMakeLists.txt          Main firmware build
  build.sh                Clean build script
  flash.sh                Flashing script
  release.sh              Release build manager
  version.txt             Current version (MAJOR MINOR)
  memmap.ld.in            Linker script template
  src/                    Firmware source code
  drivers/                Hardware drivers
  lib/                    Third-party libraries (submodules)
  apps/
    api/frankos-app.h     App development header
    source/               App source trees
    build_apps.sh         Build all apps
  sdcard/                 SD card contents (deploy to card)
  assets/                 Source artwork (icons)
  tools/                  Build tools (Python scripts)
  images/                 Documentation screenshots
  docs/                   Documentation
```

## Regenerating Icon Metadata

If you modify app icons or add new apps:
```bash
python3 tools/regen_inf.py
```

This regenerates `.inf` files and deploys `.ico` icons to `sdcard/fos/`.
