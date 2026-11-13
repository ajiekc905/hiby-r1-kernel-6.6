# 6.x migration plan (started 2026-11-07)

> Working notes copied from the hiby-r1-modder repository. Paths such as
> `fresh-kernel/…`, `player/…`, `tools/…`, `tests/…` and `out/…` refer to that
> repository; `sdk-6.6/tree` and `sdk-6.6/patches` are `tree/` and `patches/` here.
> The telnet shells and `pushnc.py` these notes mention were part of the
> private setup and are not in this tree; the dev-loop shell here is the USB
> serial port.

Goal: the R1 runs a current Linux (target **6.18 LTS**) with the card player
(`r1boot.sh` + our LVGL player) as userland. The stock `hiby_player` is not a
target: it needs the `sa_*` / `soc_*` 4.4 ABI. 4.4 (`rebuild-4.4/`) stays the
flashed fallback until 6.x covers everything below.

## Sources compared (2026-11-07)

| Source | X1600 coverage |
|--------|----------------|
| mainline 6.18.62 (`~/k6/linux-6.18`) | pinctrl/GPIO and dwc2 params only (plus dwmac, unused on the R1). X2000 rows exist only in pinctrl, `spi-ingenic`, `phy-ingenic-usb`, dwc2 and dwmac: no X2000 CGU, INTC, TCU, UART, I2C, MMC, DMA or SFC in mainline |
| letux `letux/x1600-v9` (6.13-rc7, `~/k6/letux`) | about 65 commits: `x1600.dtsi`, `x1600-cgu.c` (+ a second PLL divider in `cgu.c`), X1600 rows in TCU clk/timer/irq, INTC, 8250, USB PHY, SPI. SPI is X1000 IP (`x1000_soc_info`); the USB PHY has X1600-only registers (pre-emphasis, VBUS detect, eye tune). INTC carries the X2000 per-CPU rework, which the single-core X1600 does not need. No SFC, PDMA, DPU or MSC descriptor DMA |
| **Ingenic 6.6.18 SDK** (`llixuma/ingenic-linux-kernel6.6-x2000-v1.0-20250221`, `~/k6/sdk66`, blobless) | **Full X1600 support**: `arch/mips/xburst/soc-x1600`, defconfigs `x1600_halley6_v20_linux{,_sfc_nand_recovery,_mmc_recovery}_defconfig`, `module_drivers/dts/x1600/{x1600.dtsi,halley6_v20.dts}`, and `module_drivers`: clk-v2 `clk-x1600.c` (has `ext_div_512`), INTC, TCU, sysost, pinctrl, PDMA `ingenic_dma`, `ingenic_mmc` + `ingenic_sdio`, `ingenic_sfc_v2` incl. `winbond_nand.c`, `i2c-ingenic`, ADC v13, eFuse v2, `pwm-ingenic-v3`, `phy-ingenic-x1600`, DPU fbdev, `hyn_touch`, bcmdhd 101.10.591, ASoC halley6. GPL-2.0 headers. AXP driver is AXP216 only; no `f_uac_sa` |

Where X1600 and X2000 share IP (vendor 4.4 tree: SFC v2, PDMA, I2C, SPI, RTC,
SADC, DPU, clk-v2, MCU), the shared driver is the SDK's, not mainline's.

## Base: SDK 6.6.18 first (decided 2026-11-07)

Boot the Ingenic SDK kernel first (its X1600 drivers are the vendor family our
4.4 replacements target), then move to mainline 6.18 + the letux X1600 core
with a working 6.x as the reference for every driver. 6.6 LTS ends Dec 2026.

| Dir | What |
|-----|------|
| `sdk-6.6/` | **active.** `build.sh` (sparse checkout `~/k6/sdk66`, out `~/k6/build-sdk`), `r1.config` over `x1600_halley6_v20_linux_sfc_nand_recovery_defconfig`, `tree/` (`module_drivers/dts/x1600/hiby_r1.dts`, `soc-x1600/r1ring.c`), `patches/` (Kconfig/Makefile glue, drop module_drivers `-Werror`, MMC card power pins, SFC NAND read-only guard, SFC IRQ ordering) |
| `letux/` | parked: letux 6.13 tree, own `r1_defconfig`, earlycon ring, TCU-clock watchdog guard |
| `initramfs/`, `devloop/kx.sh` | shared |

SDK facts that shaped the build:
- `prom_init` takes the built-in DTB when a0/a1 are not a DTB handoff; 4.4
  kexec passes a0..a3 = 0 (`relocate_kernel.S` `kexec_args`, never set).
- halley6 reserves `0x03d00000+0x300000` on 64 MB parts (`reg-64M`, picked
  by the SDK's patched `drivers/of/fdt.c`); `hiby_r1.dts` is our own and
  has no reserved-memory, so the ring window stays free.
- The ring replaces `soc-x1600/early_printk.c` (`CONFIG_R1_LOG_RING`), so
  it starts right after `prom_init`, before earlycon would. `pm.c` needs that
  file's `uart_base`; it gets a NULL one, and its sleep code prints to a UART
  (`pm.h` `PRINT_DEBUG`): no suspend until that goes. `SUSPEND=n` does not
  compile (`pm_wakeup_irq`).
- TCU, watchdog and RTC nodes are disabled, nothing else touches the WDT;
  the timer is the core sys OST (`0x12000000`). No UART nodes (no console
  pins on the R1).
- GCC 14 builds it with two warnings once module_drivers' `-Werror` is gone.
- 4.4's `ingenic_wdt` has no `.shutdown`, so the watchdog armed before
  `kexec -e` survives `device_shutdown()`.

## Dev loop: kexec from 4.4, no reflash per attempt

The 4.4 bring-up took 10 blind flashes. This loop avoids that.

1. **One 4.4 flash** with `CONFIG_KEXEC=y`, plus the stock SPL command line
   set by `CMDLINE_OVERRIDE`. Its only change is `mem=63M@0x0`, so 4.4 never
   touches the top 1 MiB (0x03f00000), which becomes the log ring.
2. **The 6.x kernel** carries a built-in DTB and command line. 4.4's kexec
   passes a0..a3 = 0 and has no UHI handoff, which makes Ingenic `prom_init`
   take `__dtb_start`. Its DT memory is 63 MiB.
3. **Persistent boot log**: an early console (`prom_putchar`) writes printk
   into the uncached ring at KSEG1 0xa3f00000 from `setup_arch` on.
   Proven under `qemu-system-mipsel -M malta` (monitor `pmemsave`): banner,
   `machine is HiBy R1`, 63 MiB memory map, then malta-specific death.
4. **One attempt**: copy `vmlinux` to the card. Over telnet, arm the TCU
   watchdog (stock-proven sequence, `patches-diag/0101`), stop USB, then
   `kexec -l vmlinux && kexec -e`. A hang leads to a watchdog reset, which
   boots 4.4 from flash, and telnet then dumps the ring through an mmap of
   `/dev/mem`.
5. **initramfs first**: static busybox plus a script that saves `dmesg` to
   the card and reboots. Nothing depends on SFC/UBI until phase 3.

Tools: static `kexec` (kexec-tools 2.0.32), static busybox (initramfs),
`tools/r1ring.c` (`dump|clear|fill|check`), `devloop/kx.sh` (load, clear
ring, arm the watchdog with firmware `devmem`, `kexec -e`).

### Device run (phase 0 + first phase 1 attempt)

- Flash `out/r1-devloop.upt` as `r1.upt`: the `r1-kernel-socmsc-test` preset
  with the dev-loop 4.4 xImage (KEXEC, `mem=63M`, otherwise the field kernel;
  the staged replacement modules are code-identical to its build).
- Copy `out/devloop/card/k6/` to the card root (`kexec r1ring kx.sh vmlinux
  hiby_r1.dtb`). 6.x writes `k6/6x-boot.log` (dmesg) there once its read
  check of `k6/check.md5` (written by `kx.sh`) passes.
- Over telnet: `cd /data/mnt/sd_0/k6`, then
  1. `./r1ring fill`, `reboot`, `./r1ring check`: the ring survives a reboot;
  2. `sh kx.sh /data/mnt/sd_0/k6/vmlinux hiby_r1.dtb 60`: 6.6 boots, runs
     the initramfs dump, the watchdog resets after 60 s;
  3. back in 4.4: `./r1ring dump > ring.txt`.

First run (2026-11-07): no oops, nothing deferred; probed clock-controller,
pinctrl (4×32 GPIOs), core OST (1.5 MHz sched_clock). CPU 1104 MHz (APLL),
MPLL 1400, DDR 350, AHB 280, APB 140; L1 16+16 kB, L2 128 kB; DDRC reports
64 MB. Memory 50.5 of 63 MB available (13.7 MB kernel+init incl. the
initramfs), MemFree 44.8 MB in userspace. 9.3 of the 9.9 s went to the
built-in bcmdhd retrying SDIO with no MMC host: drop it until phase 6.
The watchdog came back ~40 s after the jump, not 60: it ticks at 45.8/s,
not 32 (see "Watchdog" under phase 3). Pulling a file over WebDAV right
after a reboot missed it (MTP session restarted); telnet `cat` worked.

### Phase 2: microSD (2026-11-07, 9 device runs)

DT `&msc1`: `msc1_4bit` (PD0–PD5), 4-bit, 50 MHz, 3.3 V, `cap-sd-highspeed`,
`ingenic,removal-removable`, CD PB22 active low, VDD switch PC25 active high
(stock `soc_msc.sh` values). `patches/0003` fixes three things in the SDK
driver, each seen on the device:
- the VDD GPIO was never driven: `power_on()` only used it when
  `IS_ERR(host->power)`, and the regulator lookup that sets `host->power` is
  commented out (NULL); now `ERR_PTR(-ENODEV)`;
- CD/WP read raw, so the DT active-low flag was ignored; now `gpiod_get_value`;
- an "off" pin state (`msc1_off`: PD0–PD5 GPIO output low, `PINCTL_FUNCLOLVL`)
  selected before VDD goes down and left right after it comes up. Without
  it the card answered CMD8 and ACMD41 (`0x40300000`, HCS set) but stayed
  busy for >5 s on every attempt (OCR `0x00ff8000`), across 400/300/200 kHz,
  with PC25 measured high: it was fed through the bus and never got a clean
  power-on reset.

Ruled out on the way: CMD8/HCS (trace), card clock (`div_msc1` 23.3 MHz,
CLKRT 6, LPM as on 4.4: `LPM=0x80000001`), slow card recovery (5 s
ACMD41 limit), post-power-on delay (100 ms changed nothing). Two dead ends:
a `gpio-hog` for PC25 defers (SDK pinctrl adds GPIO chips before pin control
is up), GPC fails to register and reading debugfs `gpio` oopses; leaving PC25
undescribed does not work because 4.4's `soc_msc` shutdown hook
(`mmc_remove_host`) powers the card off on every kexec and reboot.

Open: in 2 of 3 runs the first init attempt after kexec still stays busy and
the retry (after a pins-low power cycle) succeeds, ~1 s lost; the final run
succeeded first time. Possibly specific to kexec (4.4 cuts VDD with the bus
live); check on a cold boot before working on it.

Image size and kexec: `kexec` holds the image while the kernel copies it,
so it needs about twice the image free. With the card player running only
~17–23 MB is available: a 12.2 MB image was OOM-killed. `kx.sh` now stops
the card player (r1boot first) and drops caches (34 MB available), and the
image shrank to 8.4 MB (initramfs XZ, `-Os`, no cfg80211/mac80211 until
phase 6). Uploads go through `r1dav` on Wi-Fi port 6065: `mtpdav` needs the
USB link, which was `not attached` after the reboots.

### Phase 2: USB shell (2026-11-07, first attempt)

DT: `&otg_phy` (SDK `ingenic,usbphy-x1600`) with no `drvvbus` (stock's PC24
drives VBUS out in host mode) and no VBUS-detect GPIO (stock has none);
`&otg` `dr_mode = "peripheral"`. Config: `USB_DWC2_PERIPHERAL`,
`INGENIC_USB_PHY`, configfs ACM + ECM. The init:
- sets the TCS1421 straps to Sink as the player does on USB power (PA09,
  PB24 low via `PxPAT0C`; 4.4 leaves them as outputs, NormalDRP = 11);
- builds a configfs gadget `1d6b:0104` "R1 6.x" (not the player's
  `32bb:0004`, which R1Tray would try to mount over MTP): `acm.GS0` +
  `ecm.usb0`, fixed MACs `02:52:31:00:00:01/02`;
- `usb0` = `10.66.0.1/24`, udhcpd hands the host `10.66.0.2`, telnetd;
  a respawning `sh` on `/dev/ttyGS0`;
- meant to pet the TCU watchdog by writing TCNT = 0; that write does
  nothing on the X1600 (phase 3), so these sessions only lasted as long as
  the timeout `kx.sh` was given. Fixed in phase 3: the deadline (TDR) moves.

Result: enumerated on the Mac in seconds, `configured`/`high-speed`;
`/dev/cu.usbmodemr1_6x1` (`tools/r1serial.py` works unchanged),
`en13` got `10.66.0.2`, ping 1.6 ms, telnet root shell, host→R1 23.7 MB/s.
6.6's PHY init brings the controller up after the warm reset that leaves
4.4's dwc2 wedged (`dwc2_hsotg_corereset: reset failed` under 4.4 after a
dev-loop reset; whether a replug or a cold boot clears it is untested).

### Phase 3, step 1: SFC NAND read-only (2026-11-07, 5 device runs)

The SDK already has the W25N01GV (`winbond_nand.c`, id `0xef 0xaa21`), so
no driver port was needed. DT `&sfc` uses stock's values (init 200 MHz,
max 400 MHz, burner partition table at flash `0x5800`, `use_ofpart_info 0`).
Config: `MTD_INGENIC_SFC_NANDFLASH` only (no NOR probe path), `flashtype=nand`,
`MTD_BLOCK_BBT_RO` + squashfs (LZO/XZ), and no UBI/UBIFS or rw `mtdblock`.
Probe writes only the volatile protect/feature registers, the same as 4.4.

- **Read-only guard** (patch 0004, `CONFIG_R1_SFC_READONLY`): clears
  `MTD_WRITEABLE` and makes the driver's page program and block erase
  return `-EROFS`. The disassembly confirms neither reaches the controller.
  (Superseded 2026-11-09 by flash item 3: the guard now allows writes
  only inside `userdata`; see "Flash image" item 3.)
- **SDK bug** (patch 0005): `sfc_res_init` requested the IRQ before it
  initialised the lock and completion the handler uses. An SFC interrupt
  that 4.4 left pending fired at once, giving a spinlock bad magic, an oops
  in `swake_up_locked` and a panic. Now the lock and completion are
  initialised and the SFC interrupts masked and cleared before
  `request_irq`.
- **Result:** 7 partitions from the burner table, and the SFC runs at
  350 MHz. The initramfs reads every `/dev/mtdN` (`dd | md5sum`) at about
  11 MB/s (the 45 MB `rootfs` takes 4.0 s). `uboot`, `kernel`, `rootfs`,
  `kernel2`, `rootfs2` and `ota` hash identical to 4.4 (`out/devloop/mtd-md5-4.4.txt`),
  and `uboot`/`kernel2`/`rootfs2`/`ota` also match the 2026-11-01 dump. The
  stock `rootfs` squashfs mounts from `mtdblock_bbt_ro2`.
- **`userdata` size:** 6.6 first sized `userdata` at `0x3000000` (to the end
  of the chip), while 4.4 has `0x2d00000`. 4.4 is built with
  `INGENIC_SFCNAND_FMW` (SN/MAC/license, 1 MB each, off the end of the
  chip), so 6.6 now uses the same option (step 2) and its table matches.
- **Open:** one build (#13) died at 0.0 s with two `Micro-assembler field
  overflow` warnings in `build_clear_page` on two runs in a row. The
  arguments are the constants of `uasm_i_nop`, so the uasm table or code was
  wrong in memory. The same layout (`insn_table` at `0x804cdd30`) booted
  cleanly two builds later, so the cause is unknown. Suspects are the cache
  state after 4.4's `relocate_kernel`, which has no writeback before the
  jump, and DMA still running in 4.4. Fix `r1_kexec_cache_sync` (patch
  0009) is in #26/#27, device proof pending — see phase 4, step 2
  (#24 died the same way, but predates the fix).

#### Watchdog (X1600 TCU)

- It counts EXT/512 = 46875 Hz with a /1024 prescale = **45.8 ticks/s**
  (SDK `wdt_config`: `24000000 / 512`), not the 32 `kx.sh` assumed.
- **TCNT cannot be written**: tested 16/32-bit, running, with TCER = 0 and
  via TSSR/TSCR, under 4.4 and 6.6. The SDK's and 4.4's ping
  (`writel(0, TCNT)`) is a no-op. TCER = 0 freezes the count but does not
  clear it; only a reset does.
- So `kx.sh` arms at TDR = TCNT + S·46875/1024, and the initramfs moves TDR
  to TCNT + 20 s every 5 s. TDR is 16-bit, so a session ends after
  ~23.8 min at the latest; `touch /tmp/nopet` returns to 4.4 in 20 s
  (measured). A run with a 60 s arm stayed up past 150 s.
- Do not poke the watchdog by hand on running 4.4. A raise-TDR test there
  left the R1 with a black screen, no Wi-Fi and no USB until it was forced
  off.

### Phase 3, step 2: firmware userland from flash (2026-11-07, 2 device runs)

The initramfs (`fwroot`) sets up the stock root read-only, keeps it mounted
for the USB shell, and runs checks in a chroot:
- **rootfs:** the stock squashfs from `mtdblock_bbt_ro2`;
- **`userdata`:** `ubiattach -m 6 -d 1 /dev/ubi_ctrl` (busybox needs the
  control device; same `ubi1` as 4.4), UBIFS mounted ro at `/usr/data`;
- **system mounts:** proc, sys, bind `/dev`, and tmpfs `/tmp` and `/run`;
- **card:** ro at `/usr/data/mnt/sd_0`.

Config: `MTD_UBI`, `UBIFS_FS` (LZO, ZLIB), `INGENIC_SFCNAND_FMW` 1/1/1. UBI
sees the MTD isn't writeable: "MTD device 6 is write-protected, attach in
read-only mode", 360 good PEBs, 0 bad, 0 corrupted. UBIFS mounts `ro` and
shows the real `/usr/data`.

Results:
- the firmware's glibc 2.22 (`/lib/libc.so.6`), busybox and
  `dbus-daemon 1.12.16` run unchanged on 6.6;
- the card player (`player 480 360 /tmp/r1.ppm data - 24`, headless,
  `PLAYER_SINK=none`) loads its fonts (the user's Nunito), theme and locale
  and renders the main menu in 0.7 s with a 495 KB heap; the frame is in
  `out/devloop/r1-6.6-player.png`. Gaps: no backlight sysfs (phase 4), no
  audio engine (none requested);
- `library.db is not a library database` also appears 978 times in 4.4's
  `player.log`, and a writable copy fails the same way under 6.6: a
  player-side issue, not a kernel one.

Not comparable: 6.6's md5 of `userdata` differs from 4.4's, but 4.4 mounts
it rw and keeps writing until the jump (the card player stops in `kx.sh`).

The stock `rcS` was not run: most of its scripts insmod 4.4 modules or wait
for hardware 6.6 doesn't drive yet (fb0, Wi-Fi, PMIC, BT).

### Phase 4, step 1: display and backlight (2026-11-07, 2 device runs)

Facts from the stock module scripts (`/module_driver/*.sh`) and our 4.4
`lcd_lg35583` / `soc_fb` replacements:
- **Panel:** 480×800 TFT, 62 Hz, porches L/R 24, U 8, D 14, hsync 24,
  vsync 5; colour order BGR on even and odd lines; 24-bit parallel out on
  the RGB888 pin positions without the two low bits per colour (PA2–7,
  PA10–15, PA18–27; PA8 = WL_HOST_WAKE (Wi-Fi OOB IRQ, phase 6), PA9 = TCS1421 strap, PA16/17 = touch).
- **Init:** bit-banged 3-wire SPI (SCK PA0, MOSI PA1, CS PA30, reset PA31)
  with one of two tables: LG (16-bit `0x2000|hi`, `0x4000|data` words) or
  ST7701S (9-bit). 4.4 picks the table from ADC channel 3 (> 999 mV =
  ST7701S); this unit reads 1 mV = LG (DT `hiby,lcd-id = <0>`).
- **Rails:** AXP2101 `bldo2` (vccio) and `bldo1` (vcc); touch `aldo2`.
  There's no PMIC driver yet; in the dev loop 4.4 leaves them on.
- **Backlight:** PWM0 on PC00, 30 kHz, 0..101, active high. LEDs: red
  PWM1 PC01, blue PWM2 PC02, 10 kHz, 0..100.

6.6 side:
- `panel-hiby-r1.c` (tree/, patch 0006 adds the Kconfig/Makefile glue) is
  an SDK `fb_stage` panel driver with both tables.
- `&dpu` runs the SDK `ingenicfb` at 2 frames, 32 bpp.
- `&pwm` (`pwm-ingenic-v3`) feeds `pwm-backlight` named `backlight_pwm0`
  (the sysfs name the player and S11 use) and `pwm-leds` `red`/`blue`.
  The driver needs no PDMA for duty-cycle output.

Result:
- `fb0` comes up at 480×800p-62, the DPU interrupts at 62.5/s and the
  pixel clock is 28.57 MHz.
- The user saw black, then the noise, then the card player's home menu
  (device mode, `player 0 0 fb`), then the brightness change. The `fb0`
  readback (`out/devloop/r1-6.6-fb0.png`) matches.
- The player leaves its status bar empty: there's no clock or battery
  source yet.

One 6.6 session before this returned to 4.4 after a few minutes with
nothing in the ring (no oops). The pet loop now logs `alive <uptime>`
every minute to find out when.

### Phase 4, step 2: inputs — keys, ADC buttons, touch (built #27, device run pending)

- DT + drivers: `gpio-keys` (PC31 power active-low, PC28 next active-high;
  stock `keyboard_gpio_add.sh` values, codes 116/163), `adc-keys` (ch0,
  0/1000/1650 mV + 2800 mV keyup; stock `keyboard_adc_multifunc.sh`
  values, codes 164/114/115) over the new `x1600-sadc.c` IIO driver
  (register sequence mirrors the field-proven 4.4 `soc-adc.c`: same
  offsets, clkdiv 119/1/99, 3300 mV / 12-bit scale), and `hyn-cst8xx.c`
  (bus 1, 0x15, rst PA17, irq PA16 per stock `cst8xx_touch.sh`, rail
  AXP2101 aldo2 left on by 4.4 in the dev loop; reset timing and
  touch-data decode mirror the 4.4 replacement, single-touch slot API).
  Kconfig glue is patch 0007. #23 registered both key devices
  (`adc-keys` event0, `gpio-keys` event1); keypresses, ADC voltages and
  touch events were never observed — see next.
- **#23 post-mortem (2026-11-07):** `Kernel panic - not syncing: Attempted
  to kill init! exitcode=0x00000200` 0.1 s after the last input-dump line,
  no oops, no display lines. Exit code 2 from pid 1 with no kernel fault
  = the shell hit a syntax error in that run's init draft (since
  superseded). The current init is gated before every build: `busybox sh
  -n` with the exact static busybox that runs as pid 1, plus an
  applet-presence check (`hexdump timeout chroot ubiattach telnetd
  udhcpd devmem`).
- **#24 post-mortem:** died at 0.0 s on the stale-cache `uasm field
  overflow` again (ring20) — that build predates the fix. Patch 0009 +
  `r1_kexec_cache_sync()` (`tree/.../r1ring.c`): `prom_init` first does
  Index_Writeback_Inv over the full L1 D + L2 and invalidates L1 I,
  covering 4.4's `relocate_kernel` jump with no cache writeback. In
  #26/#27 (`System.map`: `r1_kexec_cache_sync`).
- #26 (23:37, no device run) already had the cache sync, both drivers and
  the input nodes; #27 adds the touch node enabled plus the rewritten
  init (`inputs()` logs ADC voltages + a per-event hexdump logger;
  `display()` unblanks fb0 after the headless player run). Verified in
  the image: DTB touch node `okay`, `cst_irq` + `x1600_sadc_read_raw` in
  `System.map`, the XZ initramfs inside vmlinux decompresses to the
  current init.
- Still to verify on the device: ADC voltages at rest, keypress `ev`
  lines for power/next/play/volup/voldn, touch probe + touch `ev` lines,
  and a full run to `done` with no panic — then AXP2101 (i2c0), cw2015,
  TCS1421.
- **Device runs 2026-11-08 (#27 ring21, #28 live over USB serial/ECM):**
  full runs to `done` with no panic (closes the #23 init-panic question);
  0 `Micro-assembler` warnings (cache-sync fix verified in two captures);
  ADC live (ch0 3287 mV rest, ch2/ch3 0); gpio-keys + adc-keys registered
  (event0/1) but silent. Touch on #27 failed 5× (`--I2C pio write wait
  timeout`, `--I2C irq read timeout` masked by patch 0008, then `no answer
  at 0x15`, boot and manual bind alike) with pins proven correct
  (i2c1_pb_f0 = PB19/20 = stock); `clk-always-enable` on i2c1 (the SDK's
  own x1600.dtsi pins i2c0's clock on) fixed it in #28: `hyn_ts` on
  event2, bound, 266 touch `ev` lines from a live swipe (ABS_MT + BTN_TOUCH).
  Sessions run ~24 min (TCNT saturation) then watchdog-reset to 4.4.
- **Power done 2026-11-08 (#34–#40):** new `axp2101-r1.c` (SDK axp20x
  has no AXP2101 variant) on i2c0@0x34: chip-id check (0x4a on this unit),
  the stock boot + charger register writes from the 4.4 replacement
  (4400 mV limit ladder, 500 mA input, 780 mA charge), `ac`/`usb`
  supplies with VBUS from status0 bit 5, `pm_power_off`, IRQ on PB23.
  Regulators are deliberately not registered (rails stay as 4.4 set
  them; describing them is for the flash-boot phase). cw2015 uses the
  in-tree `cw2015_battery` with the stock 64-byte profile in DT;
  charger attachment comes from the AXP supplies via
  `cfg.supplied_to`/`num_supplicants` (the DT `power-supplies` link
  panics this kernel's supply core in `__power_supply_is_supplied_by`
  — NULL `supplied_from` entry, builds #37/#38, do not re-add).
  TCS1421 straps: gpio-hog does not take on the pinctrl-integrated
  gpiochip (verified in debug/gpio); in the dev loop 4.4 sets
  NormalDRP before kexec, and the role driver belongs with USB-DAC.
  Three traps found: (1) enabling all 24 AXP IRQ sources storms
  (level INT, charge-state bits re-latch) — only plug in/out sources
  on PB23 falling edge; a storm session that watchdog-resets
  mid-transfer wedges the PMIC on SDA until a hardware power cycle
  (even stock 4.4 then fails i2c0 with -145). (2) The IRQ *count* is
  still 0 after boot — no plug edge has been observed yet (USB must
  stay in for the serial console), so the IRQ path is registered but
  unexercised. (3) Build #40 verified end state: `ac`/`usb`
  online=1, `cw2015-battery` Full 100 % 4370 mV, 0 supply warnings,
  no panic, full run to `done`.
- **Buttons CLOSED 2026-11-08 (#32 diagnosis, #33 clean):** the drivers
  were never broken. Temporary trace patches (0010–0013, since removed)
  proved the full chain on #32: gpio_keys ISR → input core → evdev →
  `__pass_event` buffer writes for codes 116/163, and the evdev client
  head/tail advanced by 2 per packet between presses, i.e. a reader was
  consuming every packet. The reader was the init logger, whose
  `hexdump | while read` pipe buffers ~4 KB in busybox stdio: a touch
  swipe flushes it, a few key presses never do, so keys looked silent
  while touch "worked". Unbuffered `dd bs=16` captures on #32 delivered
  everything: gpio-keys 116/163 down+up, adc-keys 115 (Vol+), 114 (Vol−),
  164 (Play) down+up. The init logger now captures with `dd` to
  `/tmp/eventN.raw` and batch-parses every 2 s (each hexdump run exits,
  so it flushes). Clean build #33 (no trace patches) re-verified through
  the fixed logger itself: `r1init: ev` lines for 116, 163, 115 and a
  touch tap (ABS 238,314 + BTN_TOUCH), full run, no panic. Step 2 is
  done; next is AXP2101 (i2c0, IRQ PB23), cw2015, TCS1421 straps.

## Phases

| # | Milestone | Pieces |
|---|-----------|--------|
| 0 | Dev loop proven | **done 2026-11-07**: `out/r1-devloop.upt` flashed (4.4 #4, `mem=63M`); the 1 MiB ring pattern survives a reboot intact |
| 1 | 6.x reaches the initramfs | **done 2026-11-07, first attempt**: SDK 6.6 kexec'd from 4.4, userspace at 9.9 s, full dump in the ring (30 KB, `out/devloop/ring1.txt`), watchdog back to 4.4 |
| 2 | Interactive 6.x | **done 2026-11-07**: SD card (SDK `ingenic_mmc`, MSC1 50 MHz 4-bit) + USB gadget (dwc2 peripheral, ACM shell on `ttyGS0`, ECM `10.66.0.1` with udhcpd + telnetd, high-speed, ~24 MB/s) |
| 3 | Boots from flash | **steps 1–2 done 2026-11-07**: SDK `ingenic_sfc_v2` reads the NAND read-only, partitions hash identical to 4.4, the stock squashfs and `userdata` (UBI ro) mount, firmware glibc and the card player run in a chroot. Next: a 6.6 kernel in `kernel` (≤ 5 MB: XZ, modules) via a T2 image, which needs UBI write (lift the read-only guard for `userdata` only) |
| 4 | Daily-use hardware | **step 1 done 2026-11-07** (panel/DPU/backlight/LEDs, player on `fb0`). **Step 2 done live 2026-11-08 (#27→#28)**: gpio-keys + adc-keys registered, SADC live (ch0 3287 mV rest), touch probing + swipe events on #28 (`hyn_ts` event2, 266 ev lines), full runs to `done` with no panic. **Buttons closed 2026-11-08 (#33)**: all keys verified to userspace (gpio 116/163, ADC 115/114/164) — the "silent keys" were the init logger's buffered hexdump pipe, not the drivers; logger fixed. **Power done 2026-11-08 (#40)**: AXP2101 (i2c0@0x34, IRQ PB23 registered, plug edge unexercised) + cw2015 gauge (Full/100 %/4370 mV, charger state via supplied_to); TCS1421 straps inherited from 4.4 in the dev loop, role driver deferred to USB-DAC work. Next: phase 3 flash boot or phase 5 audio |
| 5 | Audio | **step 1 done live 2026-11-08 (#41): AIC/I2S + CS43131 audible.** SDK as-v1 AIC (`ingenic,x1600-aic`, PDMA on) + mainline `cs43130` (covers CS43131) + SDK halley6 machine driver. Codec sits on the stock bit-bang bus 3 (PB30/PB31, in-tree `i2c-gpio`), power PB02 taken as the VD regulator enable, reset PB21, no INT wired (patch 0011 makes the codec IRQ optional; patch 0012 gives the codec its 24.576 MHz sysclk in the halley6 hw_params and guards the absent en3v3 gpio). Card `hiby-r1` registers, `/dev/snd/pcmC0D0p` streams, and a 1 kHz test tone (fw-glibc `fwlink` ALSA tool in the `fwroot` chroot) was heard in headphones. **Noise fixed 2026-11-09 (#48):** builds #41–#47 played dirty noise and the codec NACKed I2C during every stream (the `-6` errors) because PB02, the codec power enable, was never driven: the dts used `enable-gpios` on a `regulator-fixed`, which reads only `gpio`, so the pin stayed low as 4.4 left it. With `gpio = <&gpb 2 …>` the 1 kHz tone is clean at 48 and 44.1 kHz and the card player plays real music clean (`alsa:hw:0,0`, S24_LE, mmap). Details and the corrected register tables: `AUDIO-REFERENCE.md`. **Player volume fixed (#49):** patch 0013 adds the stock-named `Left`/`Right Playback Volume` controls (raw attenuation on PCM_VOL_A/B) next to mainline's inverted `Master`, so the player drives the codec unchanged; volume keys verified on the device. Open: tloop capture untested since the fix. **USB DAC `uac_sa` ported (build only, 2026-11-09):** the 4.4 `f_uac_sa.c` (patches 0008 + 0009 applied) is in `sdk-6.6/tree/`, with Kconfig/Makefile glue in `patches/0014`, and `CONFIG_USB_CONFIGFS_F_UAC_SA=y`. Changes from 4.4: `class_create(name)` (one argument), `usb_assign_descriptors` takes ss/ssp, and the configfs opts are a local `struct uac_sa_opts` (the SDK's `f_uac2_opts` has `p_srates[]` arrays, not the scalar `p_srate`). A full `build.sh` links clean with no config drift. **Device runs 2026-11-09:** enumerates on macOS, and the card player streams the host into the codec in a DAC-mode boot (`k6/dacmode` on the card: the initramfs skips ECM and runs the player with `PLAYER_SINK=alsa:hw:0,0`, logging `k6/dac.log`, `dac-stats.txt` and `dac-dmesg.txt` to the card). With one isochronous OUT request in flight, as on 4.4, the 6.6 dwc2 missed most intervals: 100 good, 1995 empty, 667 `-ENODATA` completions, 4 KB delivered, so the ring never got half full. Eight queued OUT requests fixed it: 10484 good, 0 empty, 0 errors, and the player got the whole 4 s tone (190512 frames). `/sys/class/uac_sa/uac_sa/stats` keeps the counters. A 1 kHz tone from the Mac was heard on the R1 headphones (2026-11-09 17:13) |
| 6 | Radios | **BT receive done live 2026-11-09 (#55): phone → R1-6x → headphones plays.** uart0 (PB07..PB10, RTS/CTS) enabled; `rfkill-gpio` "bluetooth" drives BT_REG_ON PB04 and muxes the 32 kHz clock out on PC26 (`rtc32k_enable`); BT_WAKE PB05 held high by an always-on `regulator-fixed` (gpio-hog does not take on this gpiochip); `BT`, `BT_HCIUART_H4`, RFCOMM, HIDP, `RFKILL_GPIO`, `UHID` built in. Firmware load stays in userspace as stock: `brcm_patchram_plus --enable_hci --baudrate 3000000 --no2bytes --use_baudrate_for_download --enable_lpm` with `BCM4343A1_001.002.009.1010.1030.hcd` on `/dev/ttyS0` (chroot into the fw rootfs) → `hci0` Broadcom HCI 5.2, 0 errors; LE scan sees nearby devices; fw bluetoothd 5.54 + bluealsa + `bluealsa-aplay -D hw:0,0` play A2DP from a phone. **BT send heard the same day:** setting the controller to the 4.4 adapter address (`hcitool cmd 0x3f 0x0001` with the bytes of `/usr/data/bt_macaddr.txt` reversed, then hci0 down/up, then bluetoothd) makes the 4.4 link keys in `/usr/data/bluetooth/<addr>/` valid, so the OpenRun by Shokz reconnected with no re-pair and `aplay -D bluealsa:DEV=…,PROFILE=a2dp` played a 1 kHz tone (SBC). Two dropouts in the first seconds, then steady; uart0 shows no overrun/frame errors and bluealsa no underruns, so it looks like stream start; **the same tone from 4.4 gives the same "short, short, long" start** (a hiss "fiii", a drop, then steady), so it is not 6.6. btmon traces on 4.4: ~0.3 s into every stream the host hands the controller ~1 packet in 100 ms then a 10-packet burst (aplay's default buffer; `-B 1000000` evens it out, a ~62 ms gap stays on the bluealsa side), and the link gets no baseband acks for ~180 ms at ~0.2 s; RSSI/LQ fine, sniff exit irrelevant. Matrix: Capsule3 clean on LDAC and on plain SBC; OpenRun stutters on SBC dual bitpool 70 (our player's tuning), on plain SBC joint (default tuning), and on 6.6 with stock bluealsa. A silent pre-roll fixes it: 0.7 s leaves one drop, **2 s is clean**. So: an OpenRun start-up quirk exposed by the R1's uneven first ~0.4 s; fix belongs in the player (pre-roll silence after the A2DP stream starts, before the music). **BT at boot (#58):** initramfs `bt()` runs the stock `bt_init` steps in the fw chroot (rfkill, patchram with `--bd_addr` from `/usr/data/bt_macaddr.txt`, wait for hci0, dbus, bluetoothd, bluealsa source+sink, bt-agent, Powered on): hci0 UP at 26 s with the 4.4 address, the Shokz connect with the 4.4 keys. `CONFIG_HID_SUPPORT`/`HID` added so `BT_HIDP`/`UHID` stick. **Wi-Fi done live 2026-11-09 (#61):** mainline `brcmfmac` on the SDK `ingenic_mmc` MSC0 (PB12..PB17 function 0, as 4.4 muxes them; 4 bit, 50 MHz, high-speed SDIO), stock firmware `fw_bcm43438a1.bin` 7.45.96.123 + `nvram_ap6212a.txt` linked by init as `brcm/brcmfmac43430-sdio.{bin,txt}` (chip a9a6 rev 1 = BCM43430A1, as 4.4's cywdhd reports). WL_REG_ON PB03 is a second `rfkill-gpio` ("wlan"); MSC0 is `ingenic,removal-nonremovable`, so the host comes up with no card and init writes `INSERT` to its `present` node once the firmware is reachable in the fw rootfs (patch 0015: that sysfs store read `dev->platform_data`, NULL under DT, so it would have oopsed). init `wifi()` then runs the fw `wpa_supplicant -Dnl80211` with `/usr/data/wpa_supplicant.conf` and `udhcpc`: joined the access point (same MAC as 4.4) 5 s after wlan0, telnet over Wi-Fi, ~9-10 Mbit/s R1→Mac through telnet+base64 (CPU-bound), 0/30 ping loss, no driver errors after ~35 MB, BT up beside it. **In-band SDIO interrupts lose events** (#60: joined, then every dcmd timed out with -145 = ETIMEDOUT from ~28 s on, scans looping, link dead within a minute); the OOB WL_HOST_WAKE on PA08 (`wifi@1` `brcm,bcm4329-fmac` child, level high), which stock cywdhd uses too, fixed it (#61). bt/wifi find their rfkill by name now (a second rfkill renumbers them). Also: the kernel uses its **built-in DTB** — `kx.sh`'s dtb argument is ignored, so a dts change needs the new vmlinux. And #59 panicked at 0.59 s in `dwc2_driver_probe`: a session-request interrupt left pending by 4.4 fired before `dwc2_gadget_init`, and `usb_gadget_vbus_draw` dereferenced the NULL `gadget.ops`; patch 0016 skips gadget calls until ops are set. No `clm_blob` / regulatory db on purpose (the fw's built-in CLM; regional rules are the router's job, decided 2026-11-09). wpa_supplicant config was read-only until flash item 3 (userdata rw, done 2026-11-09). Open: HFP/SCO mic |
| 7 | Switch-over | `card-player-boot` on 6.x, a T2 preset, field validation, 4.4 retired |

## Flash image (started 2026-11-09)

Plan: the `.upt` carries a 6.6 `xImage` in `kernel` and the untouched stock
rootfs; our initramfs init does all of boot and runs the card player in a
chroot of the stock squashfs (no stock rcS: its S-scripts load 4.4 modules).
Recovery (`kernel2`/`rootfs2`, Power + Vol Up) is never written.

1. **Kernel image — done.** The SPL in `uboot` loads `kernel` straight (uImage
   header, load = entry). The SDK's Ingenic `xImage` (gzip, 5.69 MB) did not
   fit the 5 MB partition; patch 0017 makes it XZ (`lib/decompress_unxz.c` in
   preboot mode, Ingenic `head.S` + its L1 cache flush kept; mainline zboot
   has no flush). Load/entry `0x81000000` (SDK `XIMAGE_LDADDR`), kernel ends
   at ~`0x80ae8000`. Production image 4,534,340 bytes, 708 KB free;
   `build.sh` fails past 5,242,880. Tested by kexec of the payload wrapped as
   an ELF at `0x81000000` (`ld -b binary --section-start=.data=0x81000000`).
2. **Production init — done (2026-11-09, build #8 via kexec).** `PROD=1
   build.sh` → `~/k6/build-prod`: `initramfs/init-prod` + shared
   `radios.sh` (bt/wifi) and `usbnet.sh` (debug ECM, only with `TESTNET=1`) +
   card-player-boot's `r1boot.sh`, `r1screen`, splash. Boot: TCS1421 Sink,
   lo, stock squashfs + userdata (rw since item 3), binds + configfs, splash
   via stock `cmd_jpeg_display`, bt + wifi in the background, `r1boot.sh` in
   the chroot (card is `mmcblk1`: MSC0 = mmc0 by alias, and ingenic_mmc picks
   its clocks by alias, so no swap; passed as `R1BOOT_DEVS`). PID 1 traps
   busybox `poweroff`/`reboot`/`halt` (USR2/TERM/USR1): kill all, unmount,
   then `-f`. `reboot` from the player returns to 4.4 clean (card mounts
   with no dirty warning). Timeline: AXP 0.15 s, touch 0.96 s, userspace
   1.8 s, card + player 2.4 s, Wi-Fi address 9.1 s, hci0 13.2 s; home screen
   and Wi-Fi confirmed on the device. Fixed on the way:
   - `/rootfs` missing from the initramfs (mounts failed for lack of a
     mountpoint; the debug init's `flash()` had made it).
   - uart0 probed at 19 s: `clocks = <&extclk 0>`, and `extclk`
     (`ingenic,fixed-clock`) never binds a driver, so fw_devlink waited for
     the deferred-probe timeout; the driver takes `gate_uart0` by name, the
     dts deletes the property. bt also waits for `/dev/ttyS0`.
   - Touch "no answer at 0x15" after 5 × 1.2 s i2c timeouts, and panel rails:
     4.4's shutdown on `kexec -e` turns off AXP2101 `aldo2` (touch, 3.3 V) and
     `bldo1`/`bldo2` (panel vcc/vccio, 2.8 V) — 0x90 = 0x3e on 4.4, 0x0c
     after the jump — and a cold boot cannot rely on them either. The
     axp2101-r1 driver now sets those voltages and enables them (0x90 |=
     0x32) and registers at `subsys_initcall`, so it probes on i2c0 at
     0.15 s, before touch and panel. It also already sets `pm_power_off`
     (AXP `REG_CONFIG` bit 0), which replaces `jz_hibernate`.
   - Patch 0018: `ingenic_mmc_detect` ran in a timer (softirq) and called
     `clk_prepare_enable` (a mutex): "scheduling while atomic", a corrupted
     spinlock, then a jump to 0 — a race that hit one boot at 1.41 s. Card
     detect now runs from a work item.
3. **Writable userdata — done (2026-11-09, build #9 via kexec).** Patch
   0004 (renamed `0004-sfc-nand-userdata-writable`) no longer blocks all
   program/erase: `r1_userdata_range()` finds the burner partition named
   `userdata` at probe (end clamped to the MTD size: burner says
   `0x5000000+0x3000000`, the device truncates to `0x2d00000`), and
   `sfcnand_do_write`/`sfcnand_do_erase` return `-EROFS` only outside
   it — so FMW-area and bad-block writes outside userdata stay blocked
   even for callers that bypass the MTD core. Every non-userdata
   partition also loses `MTD_WRITEABLE` via `mask_flags`, so UBI
   attaches mtd6 rw while `/dev/mtd0..5` refuse at open. The master
   keeps `MTD_WRITEABLE` (children inherit it) and the driver log says
   "writes only in userdata". `init-prod` mounts UBIFS rw (the debug
   init still mounts ro); `init-prod`'s `sync` + unmount in
   `halt_with` covers clean shutdown. Verified on #9: UBI attached
   with no "write-protected" line, a token written to
   `/usr/data/r1-item3-test.txt` in the 6.6 session read back after a
   reboot under 4.4, `/usr/data/r1boot.log` (47 KB) now lands on
   userdata, `dd` to `/dev/mtd5` and `/dev/mtd2` got "Permission
   denied", and mtd0–mtd5 md5s are byte-identical to
   `out/devloop/mtd-md5-4.4.txt`. Settings, saved Wi-Fi networks
   (`wpa_supplicant.conf`) and new BT pairings can now persist. The
   FMW sysfs stores stay inert (upstream `#if 0`).
4. **Power off, reboot, low battery — done (2026-11-10, build #10 via
   kexec).** No kernel change was needed: `axp2101-r1` already sets
   `pm_power_off` (AXP `REG_CONFIG` bit 0, the stock bit). The chain is
   player `g_poweroff_req` → exit 5 → `run.sh` `poweroff` → busybox
   signals PID 1 (USR2) → `init-prod` `halt_with poweroff` (kill, sync,
   unmount, `poweroff -f`) → `axp_power_off`. It fired for real on
   #10: the player's no-output auto-off logged "no headphones, BT or
   DAC for 600 s: power off" → "power: exit 5" → "=== player exit 5",
   and the device stayed off (no watchdog reboot to 4.4) until it was
   powered on by hand, then booted normally. Reboot verified twice
   more on #10 (6.6 → flashed 4.4, up in ~13 s, card clean). Low
   battery was the broken link: the player read the stock supply
   names (`battery`, `axp_battery`), which 6.6 does not have, so
   `g_bat_cap` stayed −1 and every battery policy was inert.
   `player/src/usbdac.c` now falls back to `cw2015-battery`
   (capacity/voltage/status/current_now; its `current_now` is a
   magnitude estimate, given the same sign treatment as
   `axp_battery`), and the `init-prod` splash reads either name for
   its ≤15 % backlight. On #10: sysfs 99 %, 4341 mV, Charging; the
   player's status icon showed full, and `qa.bat 27` over
   `/tmp/player_action` dropped it to a low fill (real reading
   restored with `qa.bat -1`). The ≤3 %/≤5 % shutdown branch itself
   is unchanged player code (field-proven on 4.4) fed by the same
   `g_bat_cap` and the exit-5 path proven above; it was not drained
   to 3 % for this item. Updated player deployed to the card with
   `deploy.py` before the #10 kexec.
5. **USB peripheral roles / DAC switching at runtime — done
   (2026-11-10, build #12 via kexec; DAC-out host role split out
   below).** Build #12 is a final-shape `PROD=1` build (no
   `TESTNET=1`): the debug ECM gadget is gone and the UDC belongs to
   the player's `usbd`, which picks the SDK dwc2 (`13500000.otg`) up
   by fallback. What was missing was kernel surface, in three parts:
   `CONFIG_USB_CONFIGFS_F_FS`/`MASS_STORAGE`/`F_HID` + 
   `CONFIG_POSIX_MQUEUE` (umtprd died with `mq_open` ENOSYS), and
   patch 0019, the source port of the 4.4 `ffs-class-descriptors`
   bundle: f_fs skipped-unknown-class-descriptors (`ffs_acm` and
   `ffs_mic` both carry 0x24/0x25 descriptors and got `-EINVAL`;
   `ffs_hid` already passed through the HID special case), and
   composite's `w_value && !f->get_alt` SET_INTERFACE stall removed
   (6.6 f_fs still has no `get_alt`; a UAC mic only starts on
   alt 1). The xImage grew to 4,558,916 bytes (684 KB spare).
   Verified live against the Mac, all by runtime switching with no
   reboot: the boot composition (uac_sa DAC-in + FFS keys + MTP with
   the Windows OS descriptor + FFS serial console) bound and the
   host configured it; a 4 s Mac tone streamed with 9,773 good OUT
   packets and 0 errors in `/sys/class/uac_sa` stats; `mode audio`
   ⇄ `mode mtp` rebuilt cleanly (uac released in ~100–230 ms, host
   back in <1 s); `opt mic on` brought the 16 kHz 1-channel mic up
   (macOS lists R1 as an input) and off again; mass storage through
   the player's own path (Storage action → exit 4 → `usbd ms on`)
   exported the card — after one player fix, below — the Mac
   mounted "MusicDAP", read it, ejected, and the card remounted
   with the player back in the mtp composition. One bug found on
   the way: inside the prod chroot `/proc/mounts` names the card
   `/usr/data/mnt/sd_0` (`/data` is a symlink), but usbd matched
   `/data/mnt/sd_0` literally and refused with "the card is not
   mounted". `player/src/usbd.c` now matches the canonical path
   too (`mp_eq`), gated by the new `test_ms_symlink_card` case in
   `tests/test_usbd.py` (fails without the fix, full gate green);
   the updated player was deployed with `deploy.py`.
   **USB DAC-out (host role) — done (2026-11-10, builds #13–#14 via
   kexec).** Three kernel pieces: (1) dwc2 is now dual-role
   (`USB_DWC2_DUAL_ROLE` replaces `PERIPHERAL`; DT `dr_mode =
   "otg"` + `usb-role-switch` + default peripheral, so the gadget
   owns the port until told otherwise) and `SND_USB_AUDIO` is on;
   the xImage grew to 4,735,044 bytes (508 KB spare). (2) A TCS1421
   strap driver (`tree/drivers/misc/tcs1421-r1.c`, glue patch 0020)
   at the stock `/sys/devices/platform/tcs1421/tcs1421_cfg` path:
   a role write drives the two strap GPIOs, the dwc2 role switch
   (Source/NormalDRP/StrongDRP = host, Sink = device), the PHY ID
   override and the VBUS-out GPIO (PC24 — the piece pure userspace
   poking could not reach: mainline dwc2 never calls the PHY's
   `set_vbus`, so nothing sourced 5 V and the first host attempts
   sat at "port powered, no connect" with the test DAC's LED dark).
   (3) Patch 0021 exports `ingenic_usb_phy_id_host()` from the SDK
   PHY driver (its `sw_switch_hsotg` IDDIG override, shared lock);
   without it dwc2 refuses the mode flip ("Couldn't set host
   mode"). Verified end to end with the player untouched: plugged
   into a DAC from boot, the player set NormalDRP, the driver went
   host and powered the port, an AKLIAM PD5 (and separately a
   Zorloo Ztella) enumerated at high speed as ALSA card 1, and the
   player logged "a device was already attached, hosted it" →
   "route hw:1,0 (PD5)" and played the local library through it;
   a FIIO KA11 was not re-tried (its 4.4-era pickiness is
   documented; the two UAC DACs above stand for the class).
   Return path: one `Sink` write from a stable host state gave
   `udc=configured` with the full item-5 composition back and Mac
   audio streaming again (an earlier stuck return traced to
   interleaving usbd session rebuilds with the flip, not the flip
   itself). One limitation, by hardware: TCS1421 gives the SoC no
   attach interrupt, so while hosting, plugging into a PC does not
   auto-return to Sink (on 4.4 the stock dwc2 resolved this from
   VBUS/ID; both are overridden here). The player's Settings →
   USB-C mode ("DAC for phone-PC") forces Sink, which is the
   supported way back until/unless a non-flapping trigger appears.
6. **Player audit (sysfs paths) on 6.6 — done (2026-11-10, builds
   #15–#16 via kexec).** Every `/sys`, `/proc` and `/dev` path the
   player touches was checked against a live 6.6 dump. Already right:
   backlight (`backlight_pwm0`, max 101), LEDs `red`/`blue`, supplies
   `ac`/`usb`/`cw2015-battery`, `fb0` (virtual_size 480,1600), UDC
   `13500000.otg`, TCS1421 role file, `/sys/class/sound`, `wlan0`
   statistics, rfkill names. Four mismatches found and fixed:
   - **Headset jack + EarPods remote had no driver at all** (the
     SmartAction quartet was never ported): no
     `/sys/class/switch/headset/state`, so `headset_state()` read 0
     forever — the status icon never lit and the no-output auto-off
     would kill wired playback after its timeout. New
     `tree/drivers/misc/r1-headset.c` (glue patch 0022,
     `CONFIG_R1_HEADSET=y`, DT `r1-headset` on SADC ch1/ch2):
     switch-class `headset` with stock semantics (200 ms poll,
     3-sample debounce, 2800–3300 mV window) plus the `earpods_adc`
     input device and its `earpods_adc_sw` switch at the stock path,
     click grammar ported from our 4.4 replacement (1/2/3 taps =
     play/next/prev, holds = FF/RW, vol ladder 50–250/350–550 mV),
     mic bias via a new exported `axp2101_mic_bias()` (aldo4 3.3 V,
     0x90 bit 3). Verified live: state 1 with headphones in (ch1
     ≈3290 mV), 0 with the jack empty (ch1 ≈4 mV), back to 1 on
     replug; player log shows `remote switch … on (was 0)` and the
     `earpods_adc` device classified. Remote button presses
     exercised 2026-11-10 on the flashed item-8 image: the player
     log shows `key: 164 (earpods_adc) -> player.toggle` and
     `key: 163 (earpods_adc) -> player.next`, and the track change
     was confirmed on the headphones.
   - **Charge control was read-only**: the AXP `usb` supply had no
     `set_property`, so the player's charge writes got EACCES and
     the DAC-screen charge settings silently did nothing. The
     driver now implements both setters (µA in sysfs, tracked
     icc/iin; limit 0 = 2000 mA unlimited, matching stock's
     readback) with `property_is_writeable`, and
     `usbdac_charge_set` detects the µA scale (4.4 reports mA) from
     either node — the off state reads current 0, so the limit node
     is the tell. Writes verified on #16 (1000000 lands, limit 0 →
     2000000, restored 780000/500000). Gated by new µA cases in
     `tests/test_player_usbdac.py`.
   - **LEDs dead**: 6.6 has no LED triggers (no `trigger` file at
     all), and `usbdac_led` returned −1 on the failed trigger write
     before ever writing brightness. It now treats the trigger as
     best-effort and always writes brightness (breathing falls back
     to full on). Same change wires the red LED as asked: solid on
     a hardware fault (`g_hw_fault` — no audio engine, no codec
     mixer — or the battery gauge not answering), solid below
     20 %, blinking at ≤10 %, on battery only. Verified on #16 with
     `qa.bat`: 15 % → red 100, 5 % → blinking, 25 %/real 95 % → 0;
     blue sat at 100 through a library rescan. The fault branch
     itself was not fault-injected on device.
   - **Input names**: 6.6 calls the ADC keypad `adc-keys`, not
     `jz adc keyboard`, so the play key lost its `player.hwpp`
     double-press mapping; `input.json` now carries both names.
   Left as-is, documented: the efuse serial (`/proc/jz/efuse/…`
   does not exist; the gadget serial falls back to `123456` —
   cosmetic, MTP/UAC unaffected), and the ALSA `hiby-r1 Headphone`
   jack input (SW events; the player polls the switch file, which
   now exists).
7. **First cold boot from flash — done (2026-11-10, build #17).**
   The final-shape image (`PROD=1`, no `TESTNET=1`; xImage 4,739,140
   bytes, md5 `7ffd558b…`, 503,740 bytes spare — same size as #16)
   was packed by ISO-level surgery on the pristine stock `.upt`:
   only the `xImage` chunks and their `ota_md5_xImage` chain were
   swapped, the rootfs chunks are byte-identical to stock, and
   `ota_update.in` carries the new kernel size/md5. The result
   (`out/r1-6.6-flash7.upt`) was verified before flashing by
   unpacking it again: both md5 chains valid (10 kernel chunks, 72
   rootfs chunks), reconstructed rootfs md5 = stock
   `9c8b3a94…`. Flashed via the bootloader SD update (Power + Vol Up,
   `r1.upt` in the card root; the previous 4.4 image stays on the
   card as `r1-4.4-cardplayer-backup.upt`). The device cold-booted
   straight into the card player with no kexec involved. Verified
   from the flashed system itself: `uname` = 6.6.18-rt23 #17,
   `rdinit=/init`, userspace up at 2.06 s, splash at 2.5 s, Wi-Fi
   address at 9.3 s, `hci0` present, player + `usbd` + scan running,
   userdata UBIFS rw. Partition audit against the pre-flash baseline
   (`out/devloop/mtd-md5-4.4.txt`): `uboot`, `kernel2`, `rootfs2`
   and `ota` are byte-identical (recovery untouched, as designed),
   the `kernel` partition's first 4,739,140 bytes md5-match the
   #17 xImage, and the `rootfs` partition md5-matches the stock
   squashfs. A userdata token written before a `reboot` read back
   after it — the second boot also came from flash. What remains
   different from the kexec runs is only the entry path (SPL loads
   the uImage from `kernel`); every driver and init step is the
   code already proven in items 1–6.
8. **T2 bundle swapping `xImage` + preset + gates — done
   (2026-11-10).** The hand-packed ISO surgery of item 7 is now a
   gated, reproducible build. New bundle `kernel-6.6`
   (`bundles/t2-reflash/kernel-6.6/`) declares a whole-`xImage`
   replacement (`kernel_replace`: name prefix `Linux-6.6`, load/entry
   `0x81000000`) and carries no rootfs ops at all — the proven flash
   configuration is the pristine stock rootfs, which the 6.6 initramfs
   only chroots into as a library root. It conflicts with
   `card-player-boot` (the 4.4 boot path; the 6.6 initramfs carries
   its own r1boot/splash and never runs the stock init scripts) and
   with `ffs-class-descriptors` (signature-patches the stock kernel
   this bundle replaces). `tiers/reflash/build.py` gates it: a
   kernel-replace bundle must be the only bundle enabled, `--kernel`
   is required, and the image is validated as a uImage (header + both
   CRCs, Linux/MIPS kernel type, ≤ 5 MB kernel partition, name and
   load/entry from the manifest) before anything is unpacked; the
   swap also still refuses to combine with kernel-patch bundles, and
   the kernel name/size/md5 are recorded in `t2.build.manifest.json`.
   Preset `config/presets/r1-6.6.ini` enables exactly that bundle.
   Gate `tests/test_kernel_6_6.py`: manifest/preset/G3 shape,
   synthetic-uImage validator cases (bad CRC, stock 4.4 name,
   oversize), build fail-fast without `--kernel`, and — with
   `HIBY_K6_XIMAGE` set — a full T2 build whose `.upt` unpacks to
   the supplied xImage byte-for-byte and a rootfs tree identical to
   stock. **Field-verified the same day**: `out/r1-6.6.upt`
   (md5 `b7ad89b4…`, built from the #17 `PROD=1` xImage) was put on
   the card as `r1.upt` (item-7 image kept as
   `r1-6.6-flash7-backup.upt`), flashed via Power + Vol Up, and
   cold-booted into the player. Partition audit from the flashed
   system: `kernel`'s first 4,739,140 bytes md5 = `7ffd558b…` (the
   supplied xImage), `rootfs`'s first 37,507,072 bytes md5 =
   `372a8639…` (the repacked squashfs inside `out/r1-6.6.upt` — this
   distinguishes the item-8 flash from item 7, whose rootfs was the
   stock `9c8b3a94…`), and `kernel2`/`rootfs2` are byte-identical to
   the pre-flash baseline (recovery untouched). The first boot after
   the update felt longer with no splash to the user; the logs do
   not reproduce it — a controlled reboot shows userspace at 2.06 s,
   splash at 2.52 s, Wi-Fi at 9.3 s (item 7: 2.06 / 2.5 / 9.3) and
   the splash was seen. Packaging explains no slowdown either: both
   `.upt`s are 42,661,888 bytes with 10 kernel + 72 rootfs chunks,
   and the rootfs images have the same 37,507,072-byte size; only
   the squashfs bytes differ (recompression of an identical tree).

9. **Charger never charges (found + fixed + flashed 2026-11-10).** On the flashed 6.6 the battery drained for hours
   with a known-good wall charger attached while every sysfs signal
   said charging. Root cause is in `axp2101-r1.c`: its charger init
   cleared `REG_TS_CFG` (0x50) bits 1/3 and never restored them. The
   stock 4.4 driver does the same clear in its charger probe, but its
   `axp_reg_update` (run from `axp_init_chip`) then writes 0x0e back;
   our port dropped that second write, leaving 0x04, and the AXP2101
   sat in charge state 5 (`REG 0x01` = 0x15: stage 5 = NotCharging,
   direction bits 6:5 = 0) with VBUS present. The cw2015 `status`
   derives "Charging" from VBUS alone (its dts node has no monitored
   supply — "No monitored battery" in dmesg), so the player's home
   screen and Battery graph showed charging while the cell drained
   (47 % → 29 % in ~2 h, voltage pinned at 3.76 V). Field proof on
   the device: `i2cset 0x34 0x50 0x0e` flipped `REG 0x01` 0x15 →
   0x32 (direction 01, constant-current) within seconds and the
   gauge voltage rose 3.76 V → 4.03 V, capacity 29 % → 32 % and
   climbing. Fix: `axp_charger_init` now writes `REG_TS_CFG` = 0x0e
   outright (gate `tests/test_axp2101_charger.py`); the kernel
   rebuilds clean (`xImage` 4,661,316 bytes, 581,564 free). The
   on-device registers were left at the fixed values, so the running
   system charges until its next reboot; the fix reaches the device
   with the next kernel flash. Note the AXP charge stage is the only
   truthful "charging" signal — cw2015 status cannot be trusted for
   it on this board.
   **Flashed + verified the same day**: new `PROD=1` xImage (md5
   `59b60735…`, 4,739,140 bytes) in `out/r1-6.6.upt` (md5
   `d0b4a66c…`) staged on the card as `r1.upt` (previous image kept
   as `r1-6.6-prechargefix-backup.upt`), flashed via Power + Vol Up.
   Cold boot with the charger attached and no manual register
   writes: `REG_TS_CFG` reads 0x0e (the driver wrote it), charge
   state 0x32 (constant-current), charge limit 1000 mA applied,
   voltage 4.06 V → 4.13 V and capacity 51 % → 54 % over ~8 min.
   The flashed `kernel` partition's first 4,739,140 bytes md5 =
   `59b60735…`, byte-identical to the supplied xImage.

Dev-loop transfers: `devloop/pushnc.py <host> <file> <dest> [nc]` — the R1 listens with `nc -l`
(initramfs busybox on 6.6, `k6/busybox-nc` on 4.4), the Mac sends the raw
file: 4.6 MB in 1.2 s over Wi-Fi to 4.4, 0.5 s over ECM to 6.6, vs ~10 min of
base64 through telnet.

## TODO once 6.6 is stable

- **Suspend-to-RAM for a quick wake (player idle in pause).** Phase 0 DONE
  2026-11-10: STR (`mem` and `standby`) suspends and resumes on hardware,
  Power and Next wake, the ADC ladder keys and BT-in do not, and the player
  side (entry policy, wake-press handling, Wi-Fi/BT/USB wake-time recovery,
  a trustworthy current number) is what remains. Full findings, the two
  kernel blockers and their fixes (patches 0023 + 0025, debug markers in
  0024): [`firmware/suspend-to-ram.md`](suspend-to-ram.md).
  Then the player suspends after its idle timeout when paused and nothing
  streams, wakes on the power key, and falls back to a real power-off after
  a longer idle. Not `jz_hibernate` (`soc-x1600/reset.c`, the kernel's
  `pm_power_off`): that writes `RTC_HCR` = 1, keeps only the RTC, loses RAM,
  and its "wake" (WKUP_N low 1 s) is a full cold boot through the SPL; it
  also leaves the AXP2101 rails up, so it drains more than a real power-off.
  Auto power-off goes through the AXP2101 (item 4 of the flash plan).

## Stage 2: 6.18 LTS (planned 2026-11-13, not started)

Target: **6.18 LTS (6.18.63 on 2026-11-13), built from `letux-6.18.y`**
(`goldelico/letux-kernel`). The flashed SDK 6.6 stays the fallback until 6.18
covers everything, as 4.4 did for 6.6.

Checked 2026-11-13:
- kernel.org: stable is 7.3.4, longterm 6.18.63 / 6.12.117 / 6.6.162; 6.19 is
  no longer listed (EOL), so 6.18 is the newest maintained 6.x.
- `letux-6.18.y` is at 6.18.62 and has `arch/mips/boot/dts/ingenic/x1600.dtsi`
  and `drivers/clk/ingenic/x1600-cgu.c`, so the X1600 core does not have to be
  rebased from the parked `letux/x1600-v9` (6.13-rc7). `letux/x1600-v15` is
  7.3-rc7, `letux-7.3.y` 7.3.3.
- Mainline has no X1600 CGU or dtsi in v6.18 or in master.
- Mainline 6.18.62 (`~/k6/linux-6.18`): `winbond.c` has W25N01GV; `cs43130`,
  `cw2015_battery`, `hynitron-cst816x` exist; no AXP2101 in `axp20x`; no
  Ingenic SFC controller; `jz4740_mmc`, `i2c-jz4780`, `dma-jz4780`,
  `pwm-jz4740`, `jz4740-i2s`, `ingenic-sysost` stop at X1000/X1830 rows.
- The flashed 6.6 `PROD=1` config has `CONFIG_KEXEC` off.

### What carries over

| Class | Pieces | Work |
|-------|--------|------|
| Mainline, DT only | CS43131, cw2015, brcmfmac, BT hci_uart, gpio-keys, adc-keys, pwm-backlight/leds, rfkill-gpio, i2c-gpio, dwc2, UBI/UBIFS, squashfs, snd-usb-audio | rewrite `hiby_r1.dts` against the letux dtsi and mainline pinctrl bindings |
| Mainline Ingenic driver, no X1600 row | MMC, I2C, PDMA, AIC/I2S, watchdog, RTC | compare each IP with the SDK driver; add a row or transplant the SDK one |
| Not in mainline | SFC v2 NAND controller, DPU fbdev + `panel-hiby-r1`, `pwm-ingenic-v3`, `pm.c` (STR), XZ `xImage` wrapper | port from `sdk-6.6` onto mainline clocks and pinctrl |
| Ours (`sdk-6.6/tree`, 9 files, ~3,650 lines; 25 patches) | `axp2101-r1`, `hyn-cst8xx`, `x1600-sadc`, `tcs1421-r1`, `r1-headset`, `f_uac_sa`, `r1ring`, panel | 6.6 → 6.18 API fix-ups |

SDK drivers take clocks by SDK clk-v2 names and use the SDK pinctrl bindings;
each transplant needs that glue redone for the letux CGU and mainline
`pinctrl-ingenic`.

### Phases

| # | Milestone | Pieces |
|---|-----------|--------|
| A | Dev loop | `letux-6.18.y` + log ring + TCU watchdog guard (`letux/patches`), kexec, initramfs reached. Needs a launcher (decision 1) |
| B | Interactive | SD card, USB gadget shell (ACM + ECM). Compare MemAvailable with 6.6 here |
| C | Flash storage | SFC controller + W25N01GV read-only, partition md5s against the 6.6 baseline, then the userdata-only write guard |
| D | Display, inputs, power | DPU + panel, backlight, LEDs, keys, SADC, touch, AXP2101 (rails, charger `REG_TS_CFG` = 0x0e), cw2015 |
| E | Audio | AIC + CS43131 with the stock volume controls, `uac_sa` (8 queued OUT requests) |
| F | Radios | BT on uart0, brcmfmac on MSC0 with the OOB interrupt on PA08 |
| G | USB roles | f_fs class descriptors + SET_INTERFACE, TCS1421, PHY ID override, DAC-out host |
| H | Suspend-to-RAM | port patches 0023–0025 |
| I | Flash | XZ `xImage` ≤ 5 MB, `init-prod` reused, `kernel-6.18` bundle + preset + gate cloned from `kernel-6.6`, player sysfs audit, cold-boot field test |

Each phase ends with a device run compared with 6.6 on the same unit.

### Open decisions

1. **Launcher.** Proposed: a 6.6 rebuild with `CONFIG_KEXEC=y`, flashed once;
   it leaves pins and rails in the state 6.18 must reproduce and keeps the
   6.6 player for daily use. Untested: kexec from the SDK xburst arch code,
   and the image has ~500 KB spare. Fallback: reflash the proven 4.4
   `out/r1-devloop.upt`.
2. **Display.** Proposed: port the SDK fbdev DPU as is (the player needs `fb0`
   with a pan per frame). Mainline `ingenic-drm` would be a new driver for
   this IP.
3. **Middle-class drivers.** Proposed: transplant the SDK driver whenever the
   mainline one does not match the IP on first inspection; upstream-quality
   X1600 rows later.

### Risks

- Size: 6.6 is 4.74 MB of the 5 MB partition and 6.18 is larger; expect
  Wi-Fi, BT and USB audio as modules loaded from the initramfs or userdata.
- RAM: 64 MB.
- kexec hides undriven pins: check `pinmux-pins` before diffing registers.
- Not verified: that the letux X1600 timer, INTC and USB PHY run on the R1
  (the parked 6.13 build has no recorded device boot), and which
  X1000-family drivers match the X1600 IP.

## Constraints

- Kernel partition: stock 6.6 demo images were 5.5 % too big (MatthewBriggs
  notes). XZ, `-Os`, and everything non-boot as modules.
- RAM: 64 MB total.
- Recovery is unchanged: Power + Vol Up runs the stock recovery OS
  (`kernel2`), which the `.upt` never writes.
