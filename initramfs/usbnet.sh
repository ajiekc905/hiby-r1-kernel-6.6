# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2026 ajiekc905
usb() {
    devmem 0x10010048 32 $((1 << 9))
    devmem 0x10010148 32 $((1 << 24))
    k "usb: TCS1421 straps set to Sink"
    mount -t configfs none /sys/kernel/config || return
    G=/sys/kernel/config/usb_gadget/r1
    mkdir -p $G/strings/0x409 $G/functions/acm.GS0 $G/functions/ecm.usb0 $G/configs/c.1/strings/0x409
    echo 0x1d6b > $G/idVendor
    echo 0x0104 > $G/idProduct
    echo 0x0200 > $G/bcdUSB
    echo 0xef > $G/bDeviceClass
    echo 0x02 > $G/bDeviceSubClass
    echo 0x01 > $G/bDeviceProtocol
    echo HiBy > $G/strings/0x409/manufacturer
    echo "R1 6.x" > $G/strings/0x409/product
    echo r1-6x > $G/strings/0x409/serialnumber
    echo 02:52:31:00:00:01 > $G/functions/ecm.usb0/dev_addr
    echo 02:52:31:00:00:02 > $G/functions/ecm.usb0/host_addr
    echo "ACM+ECM" > $G/configs/c.1/strings/0x409/configuration
    echo 250 > $G/configs/c.1/MaxPower
    ln -s $G/functions/acm.GS0 $G/configs/c.1/
    ln -s $G/functions/ecm.usb0 $G/configs/c.1/
    set -- $(ls /sys/class/udc)
    [ -n "$1" ] || { k "usb: no UDC"; return; }
    echo "$1" > $G/UDC && k "usb: gadget bound to $1"
    ifconfig usb0 10.66.0.1 netmask 255.255.255.0 up
    printf "start 10.66.0.2\nend 10.66.0.2\ninterface usb0\noption subnet 255.255.255.0\nlease_file /tmp/udhcpd.leases\n" > /tmp/udhcpd.conf
    touch /tmp/udhcpd.leases
    udhcpd /tmp/udhcpd.conf
    while :; do
        [ -c /dev/ttyGS0 ] && setsid -c sh -c 'exec sh -i </dev/ttyGS0 >/dev/ttyGS0 2>&1'
        sleep 1
    done &
    k "usb: shell on ttyGS0, network 10.66.0.1"
}
