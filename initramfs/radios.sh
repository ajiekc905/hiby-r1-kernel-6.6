# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026 ajiekc905
rfk() {
    for r in /sys/class/rfkill/rfkill*; do
        [ "$(cat $r/name 2>/dev/null)" = "$1" ] && { echo $r; return; }
    done
}
bt() {
    R=/rootfs
    [ -x $R/usr/bin/brcm_patchram_plus ] || { k "bt: no fw rootfs"; return; }
    rf=$(rfk bluetooth)
    [ -n "$rf" ] || { k "bt: no bluetooth rfkill"; return; }
    i=0
    while [ ! -c /dev/ttyS0 ] && [ $i -lt 100 ]; do sleep 0.2; i=$((i + 1)); done
    [ -c /dev/ttyS0 ] || { k "bt: no /dev/ttyS0"; return; }
    echo "RF=$rf" > $R/tmp/bt_up.sh
    cat >> $R/tmp/bt_up.sh <<'EOF'
F=/lib/firmware/bt_bcm/BCM4343A1_001.002.009.1010.1030.hcd
A=$(cat /usr/data/bt_macaddr.txt 2>/dev/null)
[ -n "$A" ] || A=02:52:31:00:00:03
echo "addr $A"
echo 1 > $RF/state
sleep 0.2
brcm_patchram_plus --enable_hci --baudrate 3000000 --no2bytes --patchram $F /dev/ttyS0 --tosleep=50000 --use_baudrate_for_download --enable_lpm --bd_addr $A > /tmp/patchram.log 2>&1 &
i=0
while ! hciconfig hci0 > /dev/null 2>&1; do
    i=$((i + 1)); [ $i -gt 150 ] && { echo "no hci0"; cat /tmp/patchram.log; exit 1; }
    sleep 0.2
done
hciconfig hci0 up
i=0
while ! hciconfig hci0 2>/dev/null | grep -q UP; do
    i=$((i + 1)); [ $i -gt 25 ] && { echo "hci0 not up"; break; }
    sleep 0.2
done
mkdir -p /tmp/dbus /var/lib/dbus /run/dbus
rm -f /run/messagebus.pid
dbus-uuidgen > /var/lib/dbus/machine-id
dbus-daemon --config-file=/usr/share/dbus-1/system.conf
/usr/libexec/bluetooth/bluetoothd -E -C > /tmp/bluetoothd.log 2>&1 &
if command -v dbus-send > /dev/null 2>&1; then
    i=0
    while [ $i -lt 50 ]; do
        dbus-send --system --print-reply --dest=org.freedesktop.DBus \
            / org.freedesktop.DBus.NameHasOwner string:org.bluez 2>/dev/null \
            | grep -q "boolean true" && break
        sleep 0.2
        i=$((i + 1))
    done
else
    sleep 1
fi
bt-agent -c NoInputNoOutput > /tmp/bt-agent.log 2>&1 &
i=0
while ! bt-adapter --set Powered 1 > /dev/null 2>&1; do
    i=$((i + 1)); [ $i -gt 25 ] && { echo "adapter not powered"; break; }
    sleep 0.2
done
hciconfig hci0 | grep -E 'BD|UP'
EOF
    dump bt chroot $R /bin/sh /tmp/bt_up.sh
}
wifi() {
    R=/rootfs
    W=$R/lib/firmware/wifi_bcm
    [ -f $W/fw_bcm43438a1.bin ] || { k "wifi: no fw rootfs"; return; }
    rf=$(rfk wlan)
    [ -n "$rf" ] || { k "wifi: no wlan rfkill"; return; }
    set -- /sys/bus/platform/devices/13450000.*/present
    [ -f "$1" ] || { k "wifi: no msc0 present node"; return; }
    mkdir -p /lib/firmware/brcm
    ln -sf $W/fw_bcm43438a1.bin /lib/firmware/brcm/brcmfmac43430-sdio.bin
    ln -sf $W/nvram_ap6212a.txt /lib/firmware/brcm/brcmfmac43430-sdio.txt
    echo 1 > $rf/state
    sleep 1
    echo INSERT > "$1"
    i=0
    while [ ! -d /sys/class/net/wlan0 ]; do
        i=$((i + 1)); [ $i -gt 20 ] && break
        sleep 1
    done
    dump wifi-dmesg sh -c "dmesg | grep -iE 'mmc0|sdio|brcm|cfg80211'"
    [ -d /sys/class/net/wlan0 ] || { k "wifi: no wlan0 after ${i} s"; return; }
    k "wifi: wlan0 after ${i} s"
    cat > $R/tmp/wifi_up.sh <<'EOF'
ifconfig wlan0 up
wpa_supplicant -Dnl80211 -iwlan0 -c/usr/data/wpa_supplicant.conf -B > /tmp/wpa.log 2>&1
udhcpc -b -i wlan0 -x hostname:HiBy_Music -t 20 > /tmp/udhcpc.log 2>&1 &
i=0
while ! ifconfig wlan0 | grep -q 'inet addr'; do
    i=$((i + 1)); [ $i -gt 30 ] && { echo "no address"; cat /tmp/wpa.log /tmp/udhcpc.log; wpa_cli -iwlan0 status; exit 1; }
    sleep 1
done
echo "address after $i s"
ifconfig wlan0 | grep 'inet addr'
wpa_cli -iwlan0 status | grep -E 'ssid|freq|wpa_state'
EOF
    if [ -f $R/usr/data/r1boot_wifi_off ] || ! grep -q 'network=' $R/usr/data/wpa_supplicant.conf 2>/dev/null; then
        k "wifi: no saved network or r1boot_wifi_off, interface left down"
        return
    fi
    dump wifi chroot $R /bin/sh /tmp/wifi_up.sh
}
