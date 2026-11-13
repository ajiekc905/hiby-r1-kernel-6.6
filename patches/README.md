# Patches

Applied in order by `../build.sh` with `git apply`, on top of the SDK commit
named in `../NOTICE.md`. Paths are relative to the SDK repository root
(`kernel/kernel-6.6/...`). Details and the device runs behind each one are
in `../docs/bringup-log.md`.

| Patch | Kind | What it does |
|-------|------|--------------|
| 0001 | glue | Adds the R1 board option and the boot log ring (`r1ring.c`) in place of the UART early printk |
| 0002 | build | Drops `-Werror` from `module_drivers` so GCC 14 builds it |
| 0003 | fix | MMC: drives the card VDD GPIO, honours the active-low card-detect flag, parks the bus pins low while the card is unpowered |
| 0004 | safety | SPI NAND: program and erase are refused outside the `userdata` partition. The Kconfig symbol is still called `R1_SFC_READONLY` |
| 0005 | fix | SFC: initialises the lock and completion and masks interrupts before `request_irq` (a pending interrupt panicked the probe) |
| 0006 | glue | Kconfig/Makefile for `panel-hiby-r1` |
| 0007 | glue | Kconfig/Makefile for `x1600-sadc` and `hyn-cst8xx` |
| 0008 | fix | I2C: masks controller interrupts after a timeout |
| 0009 | fix | Writes back and invalidates the caches in `prom_init`; needed when entered by kexec from 4.4 |
| 0010 | glue | Kconfig/Makefile for `axp2101-r1`; enables `cw2015_battery` |
| 0011 | fix | CS43130 codec: the interrupt line is optional (not wired on the R1) |
| 0012 | fix | halley6 machine driver: gives the codec its 24.576 MHz sysclk, tolerates a missing 3.3 V enable GPIO |
| 0013 | compat | CS43130: adds `Left`/`Right Playback Volume` controls with the stock names and scale |
| 0014 | glue | Kconfig/Makefile for `f_uac_sa` |
| 0015 | fix | MMC: the `present` sysfs store used `platform_data`, which is NULL with a device tree |
| 0016 | fix | dwc2: no gadget calls before the gadget is initialised (a pending interrupt panicked the probe) |
| 0017 | size | `xImage` is XZ-compressed instead of gzip, to fit the 5 MB kernel partition |
| 0018 | fix | MMC: card detect runs from a work item, not a timer (it takes a mutex) |
| 0019 | feature | FunctionFS accepts class-specific descriptors; composite no longer stalls SET_INTERFACE with a non-zero alternate setting for functions without `get_alt` |
| 0020 | glue | Kconfig/Makefile for `tcs1421-r1` |
| 0021 | feature | USB PHY: exports the ID override so the TCS1421 driver can switch dwc2 to host |
| 0022 | glue | Kconfig/Makefile for `r1-headset` |
| 0023 | fix | Suspend: removes the UART debug output from the sleep path (there is no UART) |
| 0024 | debug | Suspend: progress markers written to RAM and to RTC scratch registers. Diagnostic only; it is in the flashed build but should not be carried further |
| 0025 | fix | Suspend: the resume code copied to TCSM made function calls into kernel text |

0009 and 0016 only matter for entry by kexec or after a warm reset, but are
harmless on a cold boot.
