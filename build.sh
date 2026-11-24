#!/bin/bash
# Build module/orca.hex. Run under Linux / WSL:  ./build.sh [clean]
# AVR32_TOOLS overrides the toolchain location (see scripts/get-toolchain.sh).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
AVR32_TOOLS=${AVR32_TOOLS:-$HOME/avr32-tools-docker}
REFERENCE=$ROOT/release/orca-0.1.hex

if [ ! -x "$AVR32_TOOLS/bin/avr32-gcc" ]; then
    echo "no toolchain at $AVR32_TOOLS; run scripts/get-toolchain.sh" >&2
    exit 1
fi
if [ ! -f "$ROOT/libavr32/asf/avr32/utils/make/Makefile.avr32.in" ]; then
    echo "libavr32 submodule missing; run: git submodule update --init" >&2
    exit 1
fi

export PATH=$AVR32_TOOLS/bin:$PATH
cd "$ROOT/module"

if [ "${1:-}" = clean ]; then
    make clean
    exit 0
fi

make
avr32-size orca.elf | tail -1

if cmp -s orca.hex "$REFERENCE"; then
    echo "orca.hex is identical to release/orca-0.1.hex"
else
    echo "orca.hex differs from release/orca-0.1.hex"
fi
