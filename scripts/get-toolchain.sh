#!/bin/bash
# Fetch the AVR32 toolchain that reproduces release/orca-0.1.hex byte for byte.
#
# This is the /avr32-tools layer of the dewb/monome-build Docker image (amd64):
# gcc 4.4.7 + binutils 2.23.1 + newlib 1.19.0, built from source. Docker is not
# needed; the layer is pulled straight from the registry by its digest.
#
# Atmel's prebuilt 3.4.3 package (monome/avr32-toolchain-linux) does NOT
# reproduce the image: it ships a different newlib, so printf/stdio code differs.
set -euo pipefail

DEST=${1:-$HOME/avr32-tools-docker}
CACHE=${XDG_CACHE_HOME:-$HOME/.cache}/orca-toolchain
REPO=dewb/monome-build
LAYER=352e95abece19f88f0100c2fda624d05fd5f696aa5237ed90d8c0393c2d5e673

mkdir -p "$CACHE"
TGZ=$CACHE/avr32-tools-$LAYER.tgz

if [ ! -f "$TGZ" ]; then
    TOKEN=$(curl -fsSL "https://auth.docker.io/token?service=registry.docker.io&scope=repository:$REPO:pull" |
        sed -E 's/.*"token":"([^"]+)".*/\1/')
    curl -fL --progress-bar -H "Authorization: Bearer $TOKEN" -o "$TGZ.part" \
        "https://registry-1.docker.io/v2/$REPO/blobs/sha256:$LAYER"
    mv "$TGZ.part" "$TGZ"
fi

echo "$LAYER  $TGZ" | sha256sum -c -

rm -rf "$DEST"
mkdir -p "$DEST"
tar xzf "$TGZ" -C "$DEST" --strip-components=1 avr32-tools
"$DEST/bin/avr32-gcc" --version | head -1
echo "installed to $DEST"
