#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026 ajiekc905
# r1boot.sh — runs the player from the SD card. On this kernel it is started
# by initramfs/init-prod inside a chroot of the stock root filesystem; the
# stock player never starts. This brings Wi-Fi up from the saved
# networks, mounts the card, and runs the custom player from the card
# (<card>/player/run.sh) — forever. The card is ours to mount: sys_server
# mounts it only when the stock player sends MOUNT:MOUNT (field 2026-10-31:
# /dev/mmcblk0p1 present, nothing mounted), with the same plain
# 'mount -t vfat,exfat <dev> /data/mnt/sd_0' used here (iocharset=utf8 is the
# kernel default).
#
#   no card / no player on the card -> r1screen says so and waits for it
#   3 starts in a row that end within 45 s -> r1screen crash screen with the
#     tail of player.log; tap/Play = try again, Vol- or 10 min idle = off
#
# Off-switches (no reflash): none needed — pull the card and the screen says
# so; recovery is the bootloader's own Power + Vol Up flash of a stock r1.upt.
# Log (uptime-stamped, capped): /usr/data/r1boot.log.
CARD=${R1BOOT_CARD:-/data/mnt/sd_0}
P=$CARD/player
LOG=${R1BOOT_LOG:-/usr/data/r1boot.log}
SCREEN=${R1BOOT_SCREEN:-/usr/bin/r1screen}
WPA=${R1BOOT_WPA:-/data/wpa_supplicant.conf}
CARD_WAIT=${R1BOOT_CARD_WAIT:-20}
UP=${R1BOOT_UP:-45}
MAX_FAILS=${R1BOOT_MAX_FAILS:-3}
OFF=${R1BOOT_OFF:-poweroff}
WIFI_ON=${R1BOOT_WIFI_ON:-/usr/bin/wifi_on.sh}
UPTIME=${R1BOOT_UPTIME:-/proc/uptime}
MOUNTS=${R1BOOT_MOUNTS:-/proc/mounts}
DEVS=${R1BOOT_DEVS:-/dev/mmcblk0p1 /dev/mmcblk0}

cd /
exec </dev/null >/dev/null 2>&1

if [ -f "$LOG" ] && [ "$(wc -c <"$LOG")" -gt 65536 ]; then
    tail -c 32768 "$LOG" >"$LOG.new" && mv "$LOG.new" "$LOG"
fi
up() { cut -d' ' -f1 "$UPTIME"; }
say() { echo "$(up) $*" >>"$LOG"; }
say "=== r1boot"

if [ ! -f /usr/data/r1boot_wifi_off ] && [ -f "$WPA" ] && grep -q 'network=' "$WPA" &&
    [ -z "$(pidof wpa_supplicant)" ]; then
    say "wifi: saved networks, wifi_on.sh"
    (sh "$WIFI_ON" >/dev/null 2>&1 &)
fi

# /proc/mounts names the real path (/usr/data/mnt/sd_0: /data is a symlink)
mounted() { grep -q -e " $CARD " -e " $(readlink -f "$CARD" 2>/dev/null) " "$MOUNTS"; }
have_player() { [ -f "$P/run.sh" ] && [ -f "$P/player" ]; }
card_dev() {
    for d in $DEVS; do
        [ -e "$d" ] && { echo "$d"; return 0; }
    done
    return 1
}
# a pulled card leaves its mount behind: the device node is gone, or the
# filesystem hit I/O errors and went read-only (errors=remount-ro). Drop it so
# a reinserted card gets mounted again.
stale() {
    awk -v a=" $CARD " -v b=" $(readlink -f "$CARD" 2>/dev/null) " \
        'index(" " $2 " ", a) || index(" " $2 " ", b) { print $1, $4; exit }' "$MOUNTS" | {
        read -r src opts || exit 1
        [ ! -e "$src" ] && exit 0
        case ",$opts," in *,ro,*) exit 0 ;; esac
        exit 1
    }
}
# mount the card (partition first, then a card without a partition table)
mount_card() {
    if mounted && stale; then
        say "card: dropping stale mount"
        umount -l "$CARD" 2>/dev/null
    fi
    mounted && return 0
    for d in $DEVS; do
        [ -e "$d" ] || continue
        mkdir -p "$CARD"
        if mount -t vfat,exfat "$d" "$CARD" 2>/dev/null; then
            say "card: mounted $d"
            return 0
        fi
    done
    return 1
}

i=0
while ! mount_card && [ "$i" -lt $((CARD_WAIT * 5)) ]; do
    [ "$i" -ge 25 ] && ! card_dev >/dev/null && break    # the mmc probe is long done by S92
    usleep 200000
    i=$((i + 1))
done
say "card: $(mounted && echo mounted || echo absent), player: $(have_player && echo yes || echo no)"

off() {
    say "switching off"
    sync
    $OFF
    exit 0
}

# screen <title> <message> [log] [watch]: rc of r1screen; 2 when it cannot show
screen() {
    if [ -x "$SCREEN" ]; then
        "$SCREEN" -t "$1" -m "$2" ${3:+-l "$3"} ${4:+-w "$4"}
        return $?
    fi
    return 2
}

fails=0
while :; do
    mount_card
    if ! have_player; then
        if mounted; then
            say "no player on the card"
            screen "No player on the card" "Copy the folder named player to the root of the microSD card." "" "$P/player"
        elif card_dev >/dev/null; then
            say "card $(card_dev) cannot be mounted"
            screen "Cannot read the microSD card" "It needs a FAT32 or exFAT file system." "" "$P/player"
        else
            say "no card"
            screen "No microSD card" "Insert the card with the player folder on it." "" "${DEVS##* }"
        fi
        case $? in
            3) off ;;
            4) say "card or player appeared"; sleep 3 ;;
            2) sleep 10 ;;
        esac
        continue
    fi

    t0=$(up)
    say "starting $P/run.sh (fails $fails)"
    sh "$P/run.sh"
    rc=$?
    t1=$(up)
    ran=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%d", b - a }')
    say "run.sh ended rc $rc after ${ran}s"
    if [ "$ran" -ge "$UP" ]; then
        fails=0
    else
        fails=$((fails + 1))
    fi
    [ "$fails" -lt "$MAX_FAILS" ] && continue

    say "crash screen after $fails quick stops"
    screen "The player keeps stopping" "It stopped $fails times in a row, each within $UP s. Full log: player/player.log on the card." "$P/player.log"
    case $? in
        3) off ;;
        2) sleep 30 ;;
    esac
    fails=0
done
