#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026 ajiekc905
# Build the Linux 6.6 kernel for the HiBy R1 on a Linux host.
#
# $CLONE is a sparse scratch checkout owned by this script: every run resets
# it to $BASE, copies tree/ over kernel/kernel-6.6 and applies patches/. The
# config is the SDK's X1600 SFC NAND recovery defconfig plus r1.config.
#
#   ./build.sh            dev-loop kernel (initramfs/init), for kexec
#   PROD=1 ./build.sh     flash kernel (initramfs/init-prod + boot/)
#
# Do not run two builds on the same $CLONE at once: each resets it. Set
# WORK (or CLONE) to another directory for a parallel build.
#
# Output in $OUT: vmlinux, vmlinux.strip, hiby_r1.dtb and xImage (XZ, for the
# 5 MB kernel partition).
#
# Needs: git, a mipsel-linux-gnu- GCC (14 tested), xz, a static mipsel busybox
# ($BUSYBOX, config in initramfs/busybox.config) and, for PROD=1, a static
# mipsel C compiler for r1screen ($MUSL_CC, a musl cross GCC).
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=${REPO:-https://github.com/llixuma/ingenic-linux-kernel6.6-x2000-v1.0-20250221.git}
BASE=${BASE:-a98c2e1f22e4263ddd4153a4eca4db4dcfd2777b}
WORK=${WORK:-$HOME/r1-kernel-6.6-work}
CLONE=${CLONE:-$WORK/sdk66}
SRC=$CLONE/kernel/kernel-6.6
PROD=${PROD:-}
if [ -n "$PROD" ]; then OUT=${OUT:-$WORK/build-prod}; INIT=init-prod; else OUT=${OUT:-$WORK/build-dev}; INIT=init; fi
BUSYBOX=${BUSYBOX:-$WORK/busybox/busybox}
MUSL_CC=${MUSL_CC:-/opt/mipsel-musl/bin/mipsel-linux-musl-gcc}
DEFCONFIG=x1600_halley6_v20_linux_sfc_nand_recovery_defconfig
J=${J:-$(nproc)}
X=${CROSS_COMPILE:-mipsel-linux-gnu-}
M="make -C $SRC O=$OUT ARCH=mips CROSS_COMPILE=$X -j$J"

if [ ! -d "$CLONE/.git" ]; then
  git clone --filter=blob:none --no-checkout "$REPO" "$CLONE"
  git -C "$CLONE" sparse-checkout set kernel/kernel-6.6
fi
git -C "$CLONE" checkout -q -f --detach "$BASE"
git -C "$CLONE" clean -q -fd -- kernel/kernel-6.6
cp -r "$HERE/tree/." "$SRC/"
for p in "$HERE"/patches/*.patch; do git -C "$CLONE" apply "$p"; done

mkdir -p "$OUT"
[ -x "$BUSYBOX" ] || { echo "no static busybox at $BUSYBOX" >&2; exit 1; }
I=$HERE/initramfs
sed -e "s#@BUSYBOX@#$BUSYBOX#" -e "s#@INIT@#$I/$INIT#" -e "s#@RADIOS@#$I/radios.sh#" -e "s#@USBNET@#$I/usbnet.sh#" "$I/list.in" > "$OUT/initramfs.list"
if [ -n "$PROD" ]; then
  B=$HERE/boot
  [ -x "$MUSL_CC" ] || { echo "no static mipsel compiler at $MUSL_CC" >&2; exit 1; }
  "$MUSL_CC" -static -O2 -Wall -Wextra -o "$OUT/r1screen" "$B/r1screen/r1screen.c"
  "${MUSL_CC%gcc}strip" "$OUT/r1screen"
  printf 'file /r1boot.sh %s 0755 0 0\nfile /r1screen %s 0755 0 0\nfile /logo.jpeg %s 0644 0 0\n' \
    "$B/r1boot.sh" "$OUT/r1screen" "$B/logo.jpeg" >> "$OUT/initramfs.list"
  [ -n "${TESTNET:-}" ] && echo "file /testnet /dev/null 0644 0 0" >> "$OUT/initramfs.list"
fi
echo "CONFIG_INITRAMFS_SOURCE=\"$OUT/initramfs.list\"" > "$OUT/initramfs.config"
$M $DEFCONFIG
(cd "$OUT" && "$SRC/scripts/kconfig/merge_config.sh" -m -O "$OUT" "$OUT/.config" "$HERE/r1.config" "$OUT/initramfs.config" > /dev/null)
$M olddefconfig
while read -r l; do
  case "$l" in
    CONFIG_*=*|"# CONFIG_"*) grep -qx "$l" "$OUT/.config" || echo "config drift: $l";;
  esac
done < "$HERE/r1.config"
$M vmlinux
${X}strip -o "$OUT/vmlinux.strip" "$OUT/vmlinux"
cp "$OUT/module_drivers/dts/x1600/hiby_r1.dtb" "$OUT/hiby_r1.dtb"
$M xImage
cp "$OUT/arch/mips/boot/compressed/xImage" "$OUT/xImage"
ls -la "$OUT/vmlinux" "$OUT/vmlinux.strip" "$OUT/hiby_r1.dtb" "$OUT/xImage"
sz=$(stat -c %s "$OUT/xImage")
[ "$sz" -le 5242880 ] || { echo "xImage $sz bytes > kernel partition 5242880" >&2; exit 1; }
echo "xImage $sz bytes, $((5242880 - sz)) free in the kernel partition"
