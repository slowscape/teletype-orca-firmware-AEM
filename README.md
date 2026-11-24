# ORCA for Teletype (AE Modular / Telex (Teletype))

This was originally a port of [Orca-c](https://github.com/hundredrabbits/Orca-c) to the monome Teletype hardware (AT32UC3B0512) by [Zsa Zsa Roboto (Original Release Video)](https://www.youtube.com/watch?v=OAY_YaDVTds). 

In addition to patching a few bugs and adding some features (like CV In operator &, MPC style Swing, and Scales), I have tuned it for the AE Modular Telex clone of Teletype. See [manual.md](manual.md) for operating the module.

## Layout

| Path | Contents |
|---|---|
| `module/` | `main.c` (UI, flash scenes, DAC/trigger I/O) and the ASF `Makefile` / `config.mk` |
| `src/` | The ORCA VM: `sim.c`, `gbuffer.c`, `vmio.c`, `base.h` |
| `libavr32/` | monome libavr32 submodule, pinned to `eb834b9` (the version teletype pinned before June 2026) |
| `release/orca-0.1.hex` | The original 0.1 firmware image, kept as the reference for the 1:1 rebuild |
| `build.sh` | Builds `module/orca.hex` and compares it with the release image |
| `scripts/get-toolchain.sh` | Fetches the AVR32 toolchain |
| `scripts/scenes.py` | Lists a module backup's scenes and merges scenes + calibration into an image before flashing |

## Building (Linux or WSL)

```bash
git submodule update --init
scripts/get-toolchain.sh   # once; installs to ~/avr32-tools-docker
./build.sh
```

From Windows: `wsl -d Ubuntu --cd <repo path> ./build.sh`.

The original 0.1 sources, built with the pins below, reproduce `release/orca-0.1.hex` byte for byte (same SHA-256).
The current sources add features, so `build.sh` reports that the new image differs. The pins are:

- **Toolchain:** gcc 4.4.7, binutils 2.23.1 and newlib 1.19.0, built from source. This is the `/avr32-tools` layer
  of the `dewb/monome-build` Docker image, which `get-toolchain.sh` pulls by digest without needing Docker.
  Atmel's prebuilt 3.4.3 package compiles the same app code but links a different newlib, so the image comes out
  different. Its static binaries also segfault on WSL2 unless the kernel boots with `vsyscall=emulate`.
- **libavr32 `eb834b9`:** current `main` (June 2026, "keyboard fix") changes the HID code.

## Flashing (Windows)

One-time setup: USB A-A cable from the PC to the module's USB-A port, module in bootloader mode (hold the front
button while powering on), then use [Zadig](https://zadig.akeo.ie) (Options → List All Devices → "AT32UC3B DFU",
USB ID 03EB:2FF6) to install the **WinUSB** driver. Put dfu-programmer 1.1.0 (x64) in `tools/dfu-programmer/`
(git-ignored).

Each flash (module in bootloader mode), from Git Bash in the repo:

```bash
D=tools/dfu-programmer/dfu-programmer.exe
B=backup/module-flash-$(date +%F-%H%M).hex
$D at32uc3b0512 read > $B                                    # backup: firmware + scenes + calibration
python scripts/scenes.py merge module/orca.hex $B backup/flash-me.hex
$D at32uc3b0512 erase
$D at32uc3b0512 flash backup/flash-me.hex --suppress-bootloader-mem
$D at32uc3b0512 start
```

The bootloader (first 8 KB) is never erased or written. `erase` wipes saved scenes and calibration, which live in
`.flash_nvram` at 0x8004E000. `scripts/scenes.py merge` copies them from the backup into the image being
flashed, converting older 32×32 scenes into the current 64×64 layout (top-left corner). `scripts/scenes.py inspect
<backup>` lists what a backup holds. Flashing `module/orca.hex` directly starts with blank scenes and uncalibrated
outputs. To go back to the original firmware, flash `release/orca-0.1.hex` directly. It uses the older 32×32 scene
layout, which `scenes.py merge` doesn't write, so it starts with blank scenes.

The toolchain archive is cached in `~/.cache/orca-toolchain/`. Keep a copy of it, because the registry
may eventually drop that layer.
