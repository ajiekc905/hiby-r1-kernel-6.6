#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026 ajiekc905
# One 6.x boot attempt from the 4.4 dev-loop kernel (../docs/bringup-log.md), run from a shell on the device:
#   sh kx.sh [vmlinux] [dtb] [watchdog-seconds]
# Stops the card player (kexec needs ~2x the image in RAM), loads the
# kernel, quiesces everything that DMAs into 4.4 memory behind the copy's back
# (USB gadget unbound, Wi-Fi down and cywdhd unloaded: 4.4's soc_msc has no
# .shutdown, so SDIO transfers ran across the jump), clears the log ring, writes check.md5 (the 6.x
# initramfs verifies it before it writes to the card), arms the TCU watchdog and
# jumps. A hang or a finished initramfs ends in a watchdog reset back into 4.4;
# then: ./r1ring dump. The watchdog counts EXT/512/1024 = 45.8 ticks/s and its
# TCNT cannot be written (only a reset clears it), so the deadline is set as
# TDR = TCNT + S seconds; the 6.x initramfs keeps moving TDR forward. TDR is
# 16-bit: one session ends after ~23.8 min at the latest.
D=$(cd "$(dirname "$0")" && pwd)
K=${1:-$D/vmlinux}
B=${2:-$D/hiby_r1.dtb}
S=${3:-60}
set -e
[ "$S" -ge 2 ] && [ "$S" -le 1400 ] || { echo "watchdog seconds must be 2..1400" >&2; exit 2; }
C=$(($(devmem 0x10002008 32) & 0xffff))
T=$((C + S * 46875 / 1024))
[ "$T" -le 65535 ] || { echo "watchdog counter at $C, reboot first" >&2; exit 2; }
stop() {
    for p in /proc/[0-9]*; do
        c=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
        case "$c" in *"$1"*) kill "${p#/proc/}" 2>/dev/null || true;; esac
    done
}
for p in r1boot.sh /data/mnt/sd_0/player /tmp/player-sh/ /usr/data/player/ wpa_supplicant bluetoothd bt-agent brcm_patchram_plus; do
    stop "$p"
done
sleep 1
sync
echo 3 > /proc/sys/vm/drop_caches
grep MemAvailable /proc/meminfo
"$D/kexec" -l "$K" --dtb="$B"
for u in /sys/kernel/config/usb_gadget/*/UDC; do
    [ -s "$u" ] && echo "" > "$u" 2>/dev/null || true
done
ifconfig wlan0 down 2>/dev/null || true
rmmod cywdhd 2>/dev/null || true
lsmod | grep -q "^cywdhd " && echo "cywdhd still loaded" || echo "cywdhd unloaded"
"$D/r1ring" clear
(cd "$D" && md5sum vmlinux kexec r1ring > check.md5)
sync
G=$(devmem 0x10000020 32)
devmem 0x10000020 32 $((G & ~((1 << 18) | (1 << 27))))
devmem 0x10002004 8 0
devmem 0x10002028 32 $((1 << 24))
devmem 0x1000203c 32 $((1 << 16))
devmem 0x10002000 32 $(((T << 16) | T))
devmem 0x1000200c 16 $(((5 << 3) | (1 << 1)))
devmem 0x10002004 8 1
echo "watchdog armed: TCNT $C, TDR $T (${S}s), jumping"
sync
exec "$D/kexec" -e
