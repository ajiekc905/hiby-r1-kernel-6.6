# Linux 6.6 for the HiBy R1

Board support for the HiBy R1 music player (Ingenic X1600E, 64 MB RAM,
SPI NAND) on the Ingenic 6.6.18 SDK kernel: a device tree, drivers for the
R1's own hardware, fixes to SDK drivers, a kernel config, an initramfs and
the tools used to bring it up without reflashing.

It is published as a starting point: for changing how the R1 boots and what
it runs, and for bringing up similar Ingenic X1600 players. It is not a
finished product, and the debug facilities are left in on purpose.

This is an independent project. It is not affiliated with or endorsed by
HiBy or Ingenic; their names are used only to say what hardware and SDK it
is for. It comes with no warranty (`COPYING`, sections 11 and 12).

This is an overlay, not a kernel tree. `build.sh` fetches the SDK kernel and
applies `tree/` and `patches/` on top. Licence and provenance: `NOTICE.md`.

The flash kernel does not start the stock player (`hiby_player` needs the
vendor's 4.4 module ABI). What it starts instead is described under
"Boot chain" below and is meant to be changed.

## Status

Flashed and in daily use on one R1 since 2026-11-10.

| Hardware | Driver | State |
|----------|--------|-------|
| SPI NAND (W25N01GV) | SDK `ingenic_sfc_v2`, writes limited to `userdata` (patch 0004) | works |
| microSD (MSC1) | SDK `ingenic_mmc` (patches 0003, 0015, 0018) | works |
| Display, backlight, LEDs | SDK DPU fbdev + `panel-hiby-r1`, `pwm-ingenic-v3` | works |
| Touch (CST8xx) | `hyn-cst8xx` | works |
| Keys | `gpio-keys`, `adc-keys` on `x1600-sadc` | works |
| PMIC, charger, power off | `axp2101-r1` | works |
| Fuel gauge | mainline `cw2015_battery` | works; its `status` is not a reliable charging signal |
| Audio (CS43131 on AIC/I2S) | SDK as-v1 AIC + mainline `cs43130` (patches 0011–0013) | works |
| Headphone jack, inline remote | `r1-headset` | works |
| USB device (DAC in, MTP, serial, mic, mass storage) | dwc2 + `f_uac_sa` + FunctionFS (patches 0014, 0016, 0019) | works |
| USB host (external DAC) | dwc2 dual role + `tcs1421-r1` (patches 0020, 0021) | works; no automatic return to device role |
| Bluetooth (AP6212A, uart0) | mainline `hci_uart`, firmware loaded from user space | A2DP in and out work; HFP/SCO untested |
| Wi-Fi (AP6212A, MSC0) | mainline `brcmfmac`, out-of-band interrupt | works |
| Suspend to RAM | SDK `pm.c` (patches 0023–0025) | suspends and resumes; player side unfinished |
| Watchdog, RTC, TCU | — | disabled in the device tree |

History, measurements and the reasoning behind each patch: `docs/`.

## Layout

| Path | What |
|------|------|
| `tree/` | Files added to the SDK kernel: `hiby_r1.dts`, R1 drivers, `f_uac_sa.c`, the boot log ring |
| `patches/` | Changes to SDK and kernel files, applied in order |
| `r1.config` | Config fragment merged over `x1600_halley6_v20_linux_sfc_nand_recovery_defconfig` |
| `initramfs/` | `init` (dev loop), `init-prod` (flash boot), radio and USB network helpers, BusyBox config |
| `boot/` | Files `PROD=1` adds to the initramfs: `r1boot.sh`, splash, `r1screen` source |
| `devloop/` | kexec launcher `kx.sh`, log-ring reader `r1ring.c` |
| `docs/` | Bring-up log, audio reference, suspend-to-RAM notes |

## Build

On a Linux host with `git`, `xz`, a `mipsel-linux-gnu-` GCC (14.2 tested) and
a static mipsel BusyBox 1.37.0 built with `initramfs/busybox.config`:

```bash
BUSYBOX=/path/to/busybox ./build.sh
```

```bash
BUSYBOX=/path/to/busybox MUSL_CC=/path/to/mipsel-linux-musl-gcc PROD=1 ./build.sh
```

The first is the dev-loop kernel, the second the flash kernel. Work files go
to `$WORK` (default `~/r1-kernel-6.6-work`); nothing is written into this
tree. The script fails if `xImage` exceeds the 5 MB kernel partition.

`$CLONE` (the SDK checkout under `$WORK`) is scratch space that every run
resets and patches in place. Runs one after another are fine, including
runs of different trees that share it; two builds at the same moment on
the same `$CLONE` would collide. Give a parallel build its own `WORK`.

The SDK repository is a third party's upload and may disappear; keep your
own copy of the commit named in `NOTICE.md`. The dev loop also needs a
static mipsel `kexec` (kexec-tools 2.0.32 was used), which is not built
here.

## Running it

- **Dev loop (no flash per attempt):** kexec `vmlinux` from a running kernel
  built with `CONFIG_KEXEC` and `mem=63M`, using `devloop/kx.sh`. The top
  1 MiB of RAM holds a boot log ring that survives a watchdog reset;
  `devloop/r1ring.c` reads it. See `docs/bringup-log.md`, "Dev loop".
- **Flash:** `xImage` replaces the `kernel` partition image inside the
  owner's own stock `.upt`; the root filesystem stays stock. The repacking
  is done by the `hiby-r1-modder` project (`build.py reflash --kernel`,
  bundle `kernel-6.6`), not by this tree.
- **Recovery:** the update never writes the bootloader or the recovery
  system. Power + Volume Up at power-on flashes `r1.upt` from the SD card
  root.

## Boot chain

What happens after the kernel starts, and where to change it:

1. **`initramfs/init-prod`** is PID 1. It sets the USB-C role straps, mounts
   the stock root filesystem read-only at `/rootfs` (`mtdblock_bbt_ro2`,
   squashfs) and `userdata` read-write at `/rootfs/usr/data` (UBI on mtd6),
   then binds `/proc`, `/sys`, `/dev` and tmpfs into it. It never runs the
   stock init scripts.
2. It shows `boot/logo.jpeg` with the stock `cmd_jpeg_display`, sets the
   backlight, and starts `bt` and `wifi` from `initramfs/radios.sh` in the
   background. Both use the stock user-space tools and firmware files inside
   the chroot.
3. It runs **`boot/r1boot.sh`** in a chroot of `/rootfs`. That script mounts
   the SD card at `/data/mnt/sd_0` and runs **`<card>/player/run.sh`** in a
   loop. This is the hand-over point: whatever `run.sh` starts is the user
   interface. The author's player lives there and is not part of this tree.
4. With no card or no `player/run.sh`, or after three starts that each end
   within 45 s, `r1boot.sh` shows `r1screen` (`boot/r1screen/`), a static
   framebuffer message screen.
5. `poweroff` and `reboot` called from the chroot signal PID 1, which
   unmounts everything and powers off through the PMIC.

To run something else, change step 3: point `r1boot.sh` at another program
(its paths and timeouts are `R1BOOT_*` variables at the top), or replace the
call in `init-prod`. To drop the dependency on the stock root filesystem,
replace steps 1 and 2 with your own root and radio bring-up.

What a program started this way can rely on: `/dev/fb0` (480×800, 32 bpp,
double height for panning), `/dev/input/event*` (keys, touch, headset
remote), ALSA card `hiby-r1` on `hw:0,0`, backlight
`/sys/class/backlight/backlight_pwm0`, LEDs `red` and `blue`, power supplies
`ac`, `usb` and `cw2015-battery`, the headset switch at
`/sys/class/switch/headset/state`, the USB-C role file
`/sys/devices/platform/tcs1421/tcs1421_cfg`, and configfs for USB gadgets
on UDC `13500000.otg`.

## Porting to a similar device

The parts that are specific to the R1, roughly in the order to revisit them:

- `tree/module_drivers/dts/x1600/hiby_r1.dts`: every pin, rail and bus.
  Start here; most of the R1 knowledge is in this file.
- `panel-hiby-r1.c`: panel timings and init sequences.
- `axp2101-r1.c`: PMIC rails and charger limits for the R1's battery.
- `hyn-cst8xx.c`, `r1-headset.c`, `tcs1421-r1.c`: touch, jack and USB-C
  role hardware that another device may not have.
- `r1.config`: partition assumptions (`INGENIC_SFCNAND_FMW`, the 5 MB
  kernel limit in `build.sh`) and the built-in command line (`mem=63M`
  keeps the top 1 MiB for the log ring).
- Patch 0004 finds the writable area by the partition name `userdata`.

The dev loop (`devloop/`, `docs/bringup-log.md`) is the part most worth
reusing: it brought this kernel up with one flash instead of one per try.

## Security

- The flash kernel (`PROD=1`) opens no shell or network service of its own.
  The telnet shells that `docs/` mentions were removed from this tree.
- **Root shell over USB in debug builds.** The dev-loop kernel, and a
  `PROD=1` build with `TESTNET=1`, expose a root shell with no password on
  a USB serial port and bring up a USB network interface (`10.66.0.1`).
  Do not flash those for daily use.
- `r1.config` is a debugging configuration: debugfs, SysRq, `/dev/mem`
  access from the init scripts, verbose console log level.

## Known limitations

- `axp2101-r1` hard-codes the R1's battery and charger settings (4.4 V
  charge voltage, 780 mA charge current, taken from the stock firmware).
  Do not bind it on other hardware.
- Wi-Fi runs on the regulatory data built into the stock firmware blob; the
  kernel carries no regulatory database or CLM blob.
- If `/usr/data/bt_macaddr.txt` is missing, Bluetooth comes up with the
  fixed address `02:52:31:00:00:03`.
- The config and command line are tuned for bring-up, not for size or boot
  time. Patch 0024 is diagnostic code (see `patches/README.md`).
- The drivers in `tree/` are written against the SDK kernel's clock and
  pinctrl bindings and are not in a form that could be sent upstream.

## Warnings

- Flashing a kernel can leave the device unbootable until it is recovered
  as above. Use it at your own risk.
- The watchdog counter on the X1600 cannot be reset by writing it; do not
  arm or poke it by hand on a running system. `kx.sh` handles it.
- The init scripts expect the stock root filesystem and stock radio
  firmware on the device. Nothing here supplies them.
