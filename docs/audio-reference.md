# AUDIO-REFERENCE — known-good 4.4 audio state (for the 6.6 port)

> Working notes copied from the hiby-r1-modder repository. Paths such as
> `fresh-kernel/…`, `player/…`, `tools/…`, `tests/…` and `out/…` refer to that
> repository; `sdk-6.6/tree` and `sdk-6.6/patches` are `tree/` and `patches/` here.

Purpose: one authoritative place for the live-captured, known-good audio
register state on stock-derived 4.4, so the 6.6 AIC/I2S + CS43131 bring-up
can diff against truth instead of hypotheses. Captured 2026-11-08 from the
running device, during playback that a listener confirmed as a clean 1 kHz
tone. If you are another LLM picking this up: do not re-derive the wiring;
it is below, from the stock loader scripts and the replacement drivers.

## Status: 6.6 noise fixed (2026-11-09, build #48)

**Root cause: the codec power enable PB02 was never driven on 6.6.** The dts
gave the VD `regulator-fixed` an `enable-gpios` property, which only
`regulator-gpio` reads. `regulator-fixed` takes its enable from `gpio`/`gpios`
(SDK 6.6 `gpiolib-of.c`: `{ "regulator-fixed", "gpio", "enable-active-high" }`),
so the regulator had no GPIO and nothing claimed the pin
(`pinmux-pins`: `pin 34 (GPB-2): (MUX UNCLAIMED) (GPIO UNCLAIMED)`). PB02 kept
the state 4.4 left it in: an output driven low, since the 4.4 codec driver
powers the codec only while a PCM is open and `kx.sh` stops the player
first. With its power enable off, the CS43131 still answered I2C at idle
(RCO clock). Once a stream started (EXT MCLK, ASP and HP powered), it
NACKed every access and played noise. The "non-deterministic per stream" and
"rebind needed" symptoms below all came from that.

Fix: `gpio = <&gpb 2 GPIO_ACTIVE_HIGH …>` in the `reg_vd` node of
`sdk-6.6/tree/module_drivers/dts/x1600/hiby_r1.dts`. Nothing else changed.
Verified 2026-11-09 on the device (build #48, kexec dev loop):
- `gpio-34 (regulator-vd) out hi`; the codec answers I2C during streams;
  `PWDN_CTL` reads `0xae` mid-stream, the same as 4.4.
- 1 kHz tone at 48 kHz: clean (listener). After stop and restart at 44.1 kHz
  (MCLK 22579195 Hz, `SYS_CLK_CTL_1` 0x04): clean. `tone3` exits normally; no
  D-state hang.
- Card player (`alsa:hw:0,0`, MMAP_INTERLEAVED, S24_LE, 44100, period 1024,
  buffer 4096) playing a real track: clean (listener).

None of the "prime suspects" in the table below were the cause. 6.6 still
differs from 4.4 in AICFR (LSMP/DMODE), ASP framing (24-bit slots, CH2 at
bit 32, no 5050) and `SP_BITSIZE`, and plays clean anyway; leave them.

Diagnosis aid that would have found it at once:
`grep -E "pin (34|53) " /sys/kernel/debug/pinctrl/*/pinmux-pins` plus
`regulator_summary` (VD listed with no GPIO). Check that every board GPIO the
stock module scripts drive is claimed on 6.6.

Player volume (fixed 2026-11-09, build #49): the player drives the stock
controls `Left`/`Right Playback Volume` with raw attenuation (0 = 0 dB,
255 = mute; `(steps − vol) × raw_per_step`). Mainline cs43130 only has
`Master` (TLV, inverted: 255 = 0 dB), so #48 logged `volume: no mixer
controls, level is not applied`. Patch 0013 adds `Left`/`Right Playback
Volume` as plain `SOC_SINGLE` controls on PCM_VOL_A/PCM_VOL_B, the stock
semantics, so the player is the same on 4.4 and 6.6. `Master` stays. Unlike
stock, these don't mirror into DSD_VOL; that only matters for DoP/DSD, which
the player doesn't send. Verified on the device: Vol− from 22/40 to 19/40
gives `raw=63` in `player.log` and `0x3f` in both PCM volume registers
(`creg2`), and the listener heard the level follow the keys.

Still open on the audio side:
- The tloop capture (`hw:0,1`, `cap`) failed with EIO in the broken session.
  It has not been retried with the codec powered.
- `cs43130` runtime PM is still pinned on (patch 0011, autosuspend −1). With
  PB02 now real, autosuspend would cut codec power between streams and
  regcache-sync on resume. That is untested.

## Reference conditions

- Device: HiBy R1 (Ingenic X1600), 4.4-based custom kernel, card player image.
- Playback: `tone3 48000 24 1000` (fw-glibc ALSA test tool), S24_LE, stereo,
  1 kHz sine, via ALSA `default`; listener-confirmed clean.
- ALSA on 4.4: card `hiby-sound-card`, PCM `x1600-i2s-pcm cs43131-hifi-0`,
  hw_params `S24_LE / 2ch / 48000 / period 6000 / buffer 24000 / RW_INTERLEAVED`.
- PDMA interrupt (`pdmad`) fires continuously during playback (~10k per test).

## Wiring (from stock `module_driver/*.sh` and `docs/recovery`-era notes)

- AIC I2S, AIC is clock master: PB25 = DO, PB26 = MCLK, PB27 = BCLK,
  PB28 = LRCLK, all FUNC_0. (SDK `aic_pb` group muxes PB21–PB28 — do not use
  it on the R1: PB21 is codec reset, PB22 SD card-detect, PB23 AXP IRQ,
  PB24 TCS1421 strap.)
- CS43131 codec: bit-bang I2C bus 3 (SCL PB30, SDA PB31, ~200 kHz), address
  0x30, power enable PB02 (active high), reset PB21 (active high).
  INT pin is not wired on the R1. Mute / output-select pins: none (-1).
- Stock module params, `soc_aic`: `aic_if_send_invalid_data=1`,
  `aic_if_send_invalid_data_everytime=0`, `aic_send_invalid_data_time_ms=120`,
  `aic_select_clk=2` (MCLK from the I2S clock divider; clock family follows
  the stream rate: 22.5792 MHz for 44.1k family, 24.576 MHz for 48k family).

## AIC registers during clean playback (base 0x10079000)

| Register | Offset | Value | Decode |
|----------|--------|-------|--------|
| AICFR | 0x00 | 0x03100017 | ENB=1, SYNCD(BCKD master)=1, AUSEL=1, LSMP=0, DMODE=0, MSB=0, TFTH=16, RFTH=3 |
| AICCR | 0x04 | 0x01244002 | channels-1=1 (stereo), OSS=4 (24-bit), ISS=4, TDMS=1, ERPL=1 |
| I2SCR | 0x10 | 0x00000000 | I2S format, left-first |
| AICSR | 0x14 | 0x00003500 | live FIFO level; no underrun bits set |
| I2SSR | 0x1c | 0x00000034 | |
| AICLR | 0x28 | 0x00000000 | loopback off |
| I2SDIV | 0x30 | 0x00000008 | TX div 8 (24.576 MHz → BCLK 3.072 MHz → LRCK 48 kHz); RX div field 0 |
| AICDR | 0x34 | live | TX FIFO data port, e.g. 0x0083118B mid-sine |

Derived clocks at 48 kHz: MCLK 24.576 MHz, BCLK 3.072 MHz (64 BCLK/frame),
LRCLK 48 kHz. 4.4 debugfs exposes no `clk_summary`/pinmux dump; rates above
are by driver construction and confirmed by the clean tone.

## CS43131 registers during clean playback (bus 3, addr 0x30)

Register names are from mainline `cs43130.h` (corrected 2026-11-09; the
first capture had several labels wrong).

| Register | Value | Notes |
|----------|-------|-------|
| 0x010004 | a1 | REV_ID |
| 0x010005 | 06 | SUBREV_ID |
| 0x010006 | 04 | SYS_CLK_CTL_1: MCLK_SRC_SEL=0 (EXT), MCLK_INT=1 (22.5792 MHz). Stock writes 0x04 at every rate, even at 48k with a 24.576 MHz MCLK, and still plays clean; mainline sets the family per stream (0x00 at 48k, 0x04 at 44.1k) |
| 0x01000b | 02 | SP_SRATE = 48K (enum order 32/44.1/48/88.2/96/176.4/192/352.8/384) |
| 0x01000c | 00 | SP_BITSIZE: ASP and XSP = 32-bit (SP_BIT_SIZE: 32=0, 24=1, 16=2, 8=3) |
| 0x020000 | ae | PWDN_CTL: XSP, DSD-IF, XTAL, PLL, CLKOUT powered down; ASP and HP up |
| 0x020052 | 06 | CRYSTAL_SET |
| 0x030001–0x03001b | 00 00 00 00 40 10 80 03 | PLL settings; PLL unused in EXT mode |
| 0x040002 / 03 / 04 | 02 / 00 / 00 | PLL_SET_9, PLL_SET_10, CLKOUT_CTL |
| 0x040010–0x040013 | 01 00 08 00 | ASP_NUM=1, ASP_DEN=8 |
| 0x040014–0x040017 | 1f 00 3f 00 | LRCK hi-time = 31, LRCK period = 63 (64 BCLK frame, 50% LRCK) |
| 0x040018 | 0c | ASP_CLOCK_CONF: SCPOL_IN=1, SCPOL_OUT=1, LCPOL in/out=0, slave |
| 0x040019 | 0a | ASP_FRAME_CONF: 5050=1, STP=0, FSD=2 (power-on default; stock never writes it) |
| 0x050000 / 0x050001 | 00 / 00 | ASP_CH_1_LOC / ASP_CH_2_LOC: both 0 (5050 mode) |
| 0x05000a / 0x05000b | 07 / 0f | ASP_CH_1/2_SZ_EN: size 3 = **32-bit** (CH_BIT_SIZE: 24-bit is 2), enabled |
| 0x06000a / 0x06000b | 07 / 0f | XSP channel sizes, also 32-bit in the stock table |
| 0x070000 / 0x070001 | 4b / 4b | DSD_VOL_B / DSD_VOL_A (stock's volume controls write DSD and PCM volume together) |
| 0x070002 / 04 / 06 | a0 / 02 / 40 | DSD_PATH_CTL_1, DSD_PATH_CTL_2, DSD_PATH_CTL_3 |
| 0x080000 | 31 | HP_OUT_CTL_1 (stock table; mainline default 0x30) |
| 0x090000 | 02 | PCM_FILT_OPT |
| 0x090001 / 0x090002 | 4b / 4b | PCM_VOL_B / PCM_VOL_A (0.5 dB steps of attenuation: 0x4b = −37.5 dB) |
| 0x090003 / 0x090004 | 90 / 00 | PCM_PATH_CTL_1 (mute bits 1:0 clear), PCM_PATH_CTL_2 |
| 0x09000a / 0x09000b | f0 / 0c | in the stock table; not in the mainline register map |
| 0x0b0000 | 06 | CLASS_H_CTL (mainline default 0x1E) |
| 0x0d0001 | 00 | HP_STATUS |
| 0x0f0000 | 00 | INT_STATUS_1: no PLL/lock/error bits during playback |
| 0x100000–0x100004 | 2a 2a aa b7 b7 | not in the mainline register map |

Stock codec load sequence (4.4 `codec_cs43131` prepare): replay a fixed
24-entry register table, write the rate code to 0x01000b, set PCM path
0x090003=0x90 / 0x090004=0x00, then PWDN_CTL 0xbe → 0xae. It does not program
the PLL per-rate and does not depend on codec interrupts.

## 6.6 vs 4.4, before and after the PB02 fix

"#41–#47" is the broken state (PB02 low); "#48" is measured mid-stream on
2026-11-09 with the codec powered, 1 kHz at 48 kHz unless noted, all clean.

| Item | 4.4 known-good | 6.6 #41–#47 | 6.6 #48 (clean) |
|------|---------------|--------------|-----------------|
| PB02 codec power | high while a PCM is open | never claimed, low | `regulator-vd`, high |
| Codec I2C during stream | answers | NACKs (-6) on every access | answers |
| PWDN_CTL in stream | 0xae | 0xfe (DAPM writes NACKed) | 0xae |
| AICFR | 0x03100017 (LSMP=0, DMODE=0) | 0x07100157 (LSMP=1, DMODE=1, RFTH=7) | same as #41–#47 |
| AICCR, I2SCR | 0x01244002, 0x00000000 | identical | identical |
| I2SDIV | 0x00000008 | 0x00040008 (RX div also set) | 0x00040008 |
| SYS_CLK_CTL_1 | 0x04 at every rate | 0x00 | 0x00 at 48k, 0x04 at 44.1k; 0x06 (RCO) between streams |
| SP_BITSIZE (0x01000c) | 0x00 (32-bit) | 0x05 (24-bit) | 0x05 |
| ASP_CH size (0x05000a/b) | 07/0f (32-bit) | 06/0e (24-bit) | 06/0e |
| ASP_CH_2_LOC (0x050001) | 00 | 0x20 | 0x20 |
| ASP_FRAME_CONF (0x040019) | 0x0a (5050) | 0x02 | 0x02 |
| CLASS_H_CTL / HP_OUT_CTL_1 | 0x06 / 0x31 | 0x1e / 0x30 (defaults) | 0x1e / 0x30 |
| MCLK (div_i2s0t, EPLL 300 MHz) | follows the rate family | 24.576 MHz fixed until #46 | 24576000 / 22579195 Hz |
| PCM volume | 0x4b (player level) | 0x78 (−60 dB, probe default) | 0x78; 0x4b by hand. #49: set by the player via `Left`/`Right` (patch 0013) |
| DMA/interrupts | pdmad active, AICSR clean | hw_ptr advances, no TUR | same |

## Interpretation notes

- Digital silence on 6.6 was clean in #41–#47 and non-zero samples turned
  into noise. That fits a codec running without its power enable: the
  analog path is quiet with no signal and breaks down with one.
- The "non-deterministic per stream" behaviour and the NACKs were the same
  fault, not a race in stream setup.
- The format differences (24- vs 32-bit slots, 5050 vs explicit CH2
  location, SP_BITSIZE, MCLK_INT per family) are equivalent for an I2S
  stream in a 64-BCLK frame. Proven by #48 playing clean with all of them.
- The mainline `cs43130` driver expects the machine driver to provide:
  codec MCLK via component `set_sysclk` (22.5792/24.576 MHz only), ASP BCLK
  via DAI `set_sysclk` (it divides by rate×channels to get slot width), and
  DAI format via `set_fmt`. The SDK halley6 machine did none of these
  originally (it targets a dump/internal codec); 6.6 patches 0011/0012 add
  them, plus IRQ-optional probe and PLL-ready polling (the R1 has no codec
  IRQ, and the stock design never waits for one).
- Encoding reference: CS43130 channel bit-size field: 8→0, 16→1, 24→2, 32→3;
  SP_BITSIZE: 32→0, 24→1, 16→2, 8→3.
- AICFR bit names (SDK `asoc-aic.h`): bit0 ENB, bit1 SYNCD, bit2 BCKD,
  bit4 AUSEL, bit6 LSMP, bit8 DMODE (= independent-clk select on X1600:
  stock `aic_select_clk=2` leaves it 0), bit12 MSB, bits16–20 TFTH,
  bits24–28 RFTH.

## Reproduce the dump

- 4.4: `/data/mnt/sd_0/k6/tone3 48000 24 1000 60 &` then
  `devmem 0x10079000 32` (offsets 00/04/10/14/1c/28/30) and the register tool
  `creg2 /dev/i2c-3 <regs…>` (`out/devloop/card/k6/creg2`; bus path is argv[1]).
- 6.6 (dev loop): `devloop/aexp.sh` (copy to `k6/` on the card next to
  `creg2`, `tone3`, `cap`) wraps it:
  `chroot /rootfs sh /usr/data/mnt/sd_0/k6/aexp.sh tone 600 [rate]`, then
  `… aexp.sh snap` (AIC registers + every codec register 4.4 and mainline
  disagree on), `… aexp.sh vol 0x4b`, and A/B writes of the 4.4 values
  (`frame`, `analog`, `clk`, `aic`). The codec is `/dev/i2c-2` there
  (adapters: i2c-0 = AXP bus, i2c-1 = touch, i2c-2 = codec bit-bang).
- Do not trust `/sys/kernel/debug/regmap/2-0030/registers` on 6.6 for the
  codec: it shows the regmap cache, and direct `creg2` writes bypass it
  (`amixer sget Master` likewise). Read the chip with `creg2`.
- If a PCM sticks in D state (seen only with the unpowered codec), the codec
  unbind blocks behind it; `touch /tmp/nopet` and start a new session.
