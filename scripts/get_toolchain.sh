#!/bin/bash
# Downloads and unpacks Bootlin's armv5 uClibc toolchain (2018.02-2: uClibc-ng 1.0.28,
# the same C library version as the feeder's firmware) into ~/toolchains, verifying its SHA-256.
set -euo pipefail
f=armv5-eabi--uclibc--stable-2018.02-2
url=https://toolchains.bootlin.com/downloads/releases/toolchains/armv5-eabi/tarballs/$f.tar.bz2
sha=21f77d9727aa600851a59d5011d7f1f1c42ad49e85e8c78923dc179d8eb043e4
dl=$(mktemp -d)
mkdir -p ~/toolchains
if [ ! -d ~/toolchains/$f ]; then
  curl -fL --retry 3 -o "$dl/$f.tar.bz2" "$url"
  echo "$sha  $dl/$f.tar.bz2" | sha256sum -c -
  tar -xjf "$dl/$f.tar.bz2" -C ~/toolchains
  ~/toolchains/$f/relocate-sdk.sh >/dev/null 2>&1 || true
fi
rm -rf "$dl"
ln -sfn ~/toolchains/$f ~/toolchains/armv5-uclibc
~/toolchains/armv5-uclibc/bin/arm-linux-gcc --version | head -1
ls ~/toolchains/armv5-uclibc/bin | grep -E 'arm-linux-(gcc|objdump|readelf|strip)$'
