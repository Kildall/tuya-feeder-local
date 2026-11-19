#!/bin/bash
# Builds libvhook.so for the feeder (ARMv5, uClibc-ng 1.0.28) and, if the device's root filesystem is
# available, checks that the library will load there:
#  - it depends on nothing but libc.so.0
#  - every symbol it imports is exported by the device's own libc
#  - the device's dynamic loader honours LD_PRELOAD
#
# Usage:  TOOLCHAIN=~/toolchains/armv5-uclibc ROOTFS=/path/to/extracted/rootfs scripts/build.sh
#   TOOLCHAIN defaults to ~/toolchains/armv5-uclibc (see scripts/get_toolchain.sh)
#   ROOTFS    is the squashfs from your own flash backup (mtd5), unpacked; checks are skipped without it
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
T="${TOOLCHAIN:-$HOME/toolchains/armv5-uclibc}/bin"
OUT="${OUT:-$HERE/build}"
mkdir -p "$OUT"

"$T/arm-linux-gcc" -shared -fPIC -O2 -Wall -Wextra -Wno-unused-parameter \
  -mcpu=arm926ej-s -marm -fvisibility=default \
  -o "$OUT/libvhook.so" "$HERE/src/vhook.c"
"$T/arm-linux-strip" --strip-unneeded "$OUT/libvhook.so"

fail=0
needed=$("$T/arm-linux-readelf" -d "$OUT/libvhook.so" | awk '/NEEDED/{gsub(/[\[\]]/,"",$5); print $5}')
echo "NEEDED: $needed"
[ "$needed" = "libc.so.0" ] || { echo "FAIL: unexpected dependencies"; fail=1; }

exports=$("$T/arm-linux-readelf" --dyn-syms -W "$OUT/libvhook.so" | awk '$7!="UND" && $4=="FUNC"{print $8}' | grep '^ak_' || true)
echo "hooks exported: $exports"
[ "$exports" = "ak_venc_encode_frame" ] || { echo "FAIL: hook symbol not exported"; fail=1; }

if [ -n "${ROOTFS:-}" ] && [ -f "$ROOTFS/lib/libuClibc-1.0.28.so" ]; then
  syms=$(mktemp)
  "$T/arm-linux-readelf" --dyn-syms -W "$ROOTFS/lib/libuClibc-1.0.28.so" \
    | awk '$7!="UND" && $8!=""{sub(/@.*/,"",$8); print $8}' | sort -u > "$syms"
  missing=0
  for s in $("$T/arm-linux-readelf" --dyn-syms -W "$OUT/libvhook.so" \
             | awk '$7=="UND" && $8!="" && $5!="WEAK"{sub(/@.*/,"",$8); print $8}' | sort -u); do
    grep -qx "$s" "$syms" || { echo "FAIL: device libc lacks $s"; missing=1; }
  done
  rm -f "$syms"
  [ $missing -eq 0 ] && echo "imports: all present in the device's libc" || fail=1
  if strings "$ROOTFS/lib/ld-uClibc-1.0.28.so" | grep -qx 'LD_PRELOAD'; then
    echo "device loader: LD_PRELOAD supported"
  else
    echo "FAIL: device loader has no LD_PRELOAD"; fail=1
  fi
else
  echo "note: ROOTFS not set (or not this firmware); skipped the device-libc checks"
fi

ls -l "$OUT/libvhook.so" | awk '{print "size: "$5" bytes"}'
md5sum "$OUT/libvhook.so" | awk '{print "md5: "$1}'
[ $fail -eq 0 ] && echo "BUILD OK" || { echo "BUILD CHECKS FAILED"; exit 1; }
