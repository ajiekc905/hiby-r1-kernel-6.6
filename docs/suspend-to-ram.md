# Suspend-to-RAM (quick wake) — Phase 0 findings (2026-11-10)

> Working notes copied from the hiby-r1-modder repository. Paths such as
> `fresh-kernel/…`, `player/…`, `tools/…`, `tests/…` and `out/…` refer to that
> repository; `sdk-6.6/tree` and `sdk-6.6/patches` are `tree/` and `patches/` here.

Goal: the card player sleeps in suspend-to-RAM instead of today's
screen-off-only sleep ([power.c](hiby-r1-modder repo)),
and wakes fast on transport keys. Priority 1: wake from Next / Play.
Priority 2: wake from a phone streaming BT audio in (A2DP sink).

Test device: R1 on the 6.6.18-rt23 PROD kernels (builds #18–#26 during
this session), card player running, access via telnet (Wi-Fi) and the
USB serial console ([r1serial.py](hiby-r1-modder repo);
the serial path is the reliable one — Wi-Fi is dead after every resume,
see below). Probe scripts and logs live on the card in
`player/suspend-probe/` (not committed).

## Verdict up front

STR works. `echo mem` suspends and resumes cleanly (five consecutive
successful cycles this session, `suspend_stats` fail = 0). Two kernel
bugs blocked it; both are fixed and carried as patches (below). Of the
wake sources: **Power wakes, Next wakes, Play/Vol (the ADC ladder) do
not and cannot, BT-in does not.** Wi-Fi and the player's own
screen-state machine need resume work before this is a product feature.

## What blocked STR, and the fixes

Stock evidence said STR should work: stock `hiby_player` runs
`echo mem > /sys/power/state` (with `bt_suspend`/`bt_resume` around
it). On our 6.6 kernel every `echo mem` / `echo standby` rebooted the
device instead (uptime reset, suspend never registered).

Bisection with `CONFIG_PM_DEBUG` + `pm_test` (patch: `r1.config` gained
`CONFIG_PM_DEBUG=y`, `CONFIG_PM_SLEEP_DEBUG=y`) showed freezer /
devices / platform / processors all pass; `core` never calls
`x1600_pm_enter`. So the fault was inside the SoC sleep code, not in
device drivers. Progress markers stamped into a reserved DRAM address
(0x03ffff00; survives a watchdog-style reset — but *not* reliably a
long-press power cut, values decayed to 0/2 on two recoveries) and into
the battery-backed RTC alarm register (`RTASR` = `0x5EED00nn`;
a test stamp `0x5EED0042` survived a full long-press power cycle)
localised it step by step:

1. **Marker 61**: execution reached the first statement of the
   TCSM-resident `cpu_sleep` —
   `sleep_param->cpu_div = cpm_inl(CPM_CPCCR) & 0xf;` — and never
   returned. Disassembly showed why: this build compiles `cpm_inl` to
   a real `jal inl` (plus `jal outl`, `jal __sync`; 7 calls in
   `cpu_sleep`). `load_func_to_tcsm` only relocates MIPS `j`
   (opcode 2); a `jal` executed from TCSM (0xb342…) recomputes its
   target inside the 0xb… 256 MB region and jumps into the IO
   aperture. In the SDK's own build those helpers no doubt inlined;
   in ours they did not. Function-size truncation was ruled out
   (`cpu_sleep` 1000 of 3072 bytes, `cpu_resume` 548 of 4096).
   **Fix** — [0025-pm-tcsm-no-calls.patch](../patches/0025-pm-tcsm-no-calls.patch):
   CPM access in `pm.c` is a direct volatile deref of 0xb0000000,
   the syncs are inline asm. Verified on the built vmlinux:
   **0 `jal`** in `cpu_sleep`, `cpu_resume`, `cpu_resume_bootup`.
2. The already-known UART print blocker —
   [0023-pm-no-uart-debug.patch](../patches/0023-pm-no-uart-debug.patch):
   drops `PRINT_DEBUG` in `pm.h` (the sleep code printed to a UART
   the R1 does not have; `uart_base` is NULL).
   [0024-pm-str-markers.patch](../patches/0024-pm-str-markers.patch)
   carries the DRAM+RTC progress markers; it is debug scaffolding
   and should be dropped once STR is productised. (Caveat: the RTC
   stamps never landed during the actual PM path — final `RTASR`
   stayed at the cleared value — presumably the WRDY/WEN polling
   cannot complete in that atomic context. The DRAM stamps did land:
   final marker 10 = end of resume, every successful cycle.)

Current card image for all results below: kernel #26,
xImage md5 `38340ce5…`, `.upt` md5 `38e5e893…`
(`out/r1-6.6-str.upt`); previous image kept on the card as
`r1-6.6-pre-str-backup.upt`.

## Wake-source results (`echo mem` unless noted)

- **Power (GPIO PC31): wakes.** `standby6` and `mem1` both resumed on
  a short press (`suspend_stats` success 1, then 2; fail 0).
  Behaviour the player must fix: first press is consumed by the
  kernel wake; the screen shows home for ~1 s, then re-blanks
  (the player's `g_sleep` is still set and its idle timeout is long
  expired, so `sleep_poll` re-sleeps at once). A second short press
  (the normal `screen.toggle`) lights it properly. So after STR a
  wake press must count as fresh activity / clear the sleep state,
  not just toggle.
- **Next (GPIO PC28): wakes.** `mem2next`: success 3, Next IRQ count
  0 → 2 across the cycle (Power untouched). The screen stayed on.
  Whether the waking Next is also *delivered* as `player.next` is
  still unproven — no playlist was loaded in that test, so nothing
  could skip. Retest with a queue loaded (expect either a skip, or
  silence = the press was consumed as pure wake and the player needs
  a wake-reason latch, e.g. a `wakeup_count` delta read on resume).
- **Play / Vol+ / Vol− (ADC ladder, SADC ch 0): no wake.** In the
  `mem3ladder` cycle each was pressed and observed for ~15 s:
  nothing; Power then woke it (success 4, ladder IRQ counts flat).
  Expected from the hardware: the ladder is polled (`adc-keys`,
  50 ms); in STR the SADC is stopped and nothing compares the
  voltage. The proposed “wake on any ladder key, decode which after
  resume, re-sleep if it was Vol” therefore does not work as stated:
  there is no ladder wake event to decode after. The only remaining
  speculation is re-muxing the ladder pin as a GPIO wake source
  (Play sits near 0 V); no evidence yet that the pin supports it.
  Deprioritised; not Phase 1.
- **RTC/other timer wake: untested.** `rtc0` exists; a wakealarm
  timed wake is the natural mechanism for “STR for hours, then real
  power-off”, still to probe.

## Priority 2: BT streaming in — no wake

`mem4bt`: device in STR, the owner's phone (previously paired,
A2DP-sink aptX-HD) tried to connect for ~1 min: phone reported
connecting → failed; the R1 never woke. After a Power wake
(success 5), `r1btd.log` shows no connection attempt during the
suspend window and the UART0 IRQ moved only +10 (resume traffic).
So while the host is in STR the phone cannot even establish the
link: either the BCM43430 is not page-scanning with the host asleep
(`brcm_patchram_plus --enable_lpm` is in use), its host-wake line
does not reach a wake-capable GPIO (Wi-Fi's `WL_HOST_WAKE` PA08 is
wired; no BT equivalent is known), or the UART/rfkill was suspended
under it. This matches the stock design, which kills BT entirely
before STR (`bt_suspend`) and re-inits it after.

Product consequence, at least for now: **STR must not be entered
while BT-in is armed or a phone is connected** — stay in today's
screen-off sleep for that case (the existing
[power_policy()](hiby-r1-modder repo)
busy/no-output conditions already distinguish it). BT wake can be
revisited only if a host-wake GPIO for BT is ever identified.

## Resume side effects (the “nap” recovery list)

- **Wi-Fi is dead after every resume** (standby and mem): no ping,
  no `r1.local`; the Wi-Fi page claims connected, the home screen
  shows no Wi-Fi icon. Kernel at resume:
  `brcmfmac: brcmf_sdio_dpc: failed backplane access over SDIO,
  halting operation`, then firmware reload and
  `brcmf_cfg80211_get_station: GET STA INFO failed, -50`.
  Toggling Wi-Fi off/on (sometimes more than once, sometimes with
  an error) recovers it. So the SDIO link to the BCM43430 does not
  survive the nap despite `keep-power-in-suspend` on MSC0. Product
  fix first: on wake, treat Wi-Fi as suspect and re-establish it
  (bounce/reassociate), exactly like the manual toggle. Kernel-side
  SDIO keep-power remains a later nice-to-have.
- Player screen state: see the two-press behaviour under Power.
- USB (cable attached throughout): the USB serial console and the
  player came back fine after every cycle, apparently without user
  action.
- Nothing else in five cycles complained: panel/touch, playback
  stack and the SD library all worked post-resume per the owner's
  checks; a scripted 50× suspend loop with a playback check is
  still owed before productising.

## Suspend current

Gauge is %/µV only (neither cw2015 `current_now` nor the AXP sysfs
values are real measurements), so the method is a timed drain.
First attempt (`drain1`) never suspended: `asoc-aic-i2s` refused
with error -1 (`PM: Some devices failed to suspend`, fail count 1) —
devices *can* veto a nap, which the player must handle by
quiescing its own sink first instead of assuming `echo mem` works.

Second attempt (`drain2`, on battery, cable already out and
settled): entered `mem`, woke 31–32 min later on the Power key —
kernel log confirms the whole cycle, wake IRQ = 75 (Power GPIO).
Capacity went 99 % → 97 %, voltage 4251 → 4281 mV. Nominal
estimate: 2 % of the 1600 mAh cell ≈ 32 mAh over ~0.52 h ≈
**~60 mA average**. Treat this as an upper bound with big error
bars, not a measurement: the top of the gauge is nonlinear, the
% steps are coarse, and the post-resume voltage is a rested
reading. It is far above what DDR self-refresh alone should cost,
so either STR on this board keeps a lot powered (radios, rails) or
the gauge overstates the drop near full. A trustworthy number
needs an overnight run starting near 80 % against the player's
screen-off idle baseline from `battery.tsv`. Note the device
clock does not advance across STR, so script-side epoch stamps
cannot measure sleep duration (the pre/post stamps were 2 s
apart); the duration above is the owner's wall clock.

After that same resume the owner also found **USB down** (the
USB serial console never re-enumerated until further recovery),
in addition to dead Wi-Fi and BT — so the wake-time recovery list
is: Wi-Fi bounce, BT re-init, USB gadget check.
- **Wall clock loses the nap duration**: the system clock does
  not advance across STR (after all the day's naps it was 44 min
  behind; synced by hand with `date -s @<host epoch>`). The RTC
  counter keeps its own unsynced base and there is no usable
  `/dev/rtc0` in the shell namespace to `hwclock -w` to, so the
  player must re-sync time on wake (NTP once Wi-Fi is back, or a
  proper RTC persistent-clock fix in the kernel).

## Suggested Phase 1 shape (player side)

- Enter STR only when paused/stopped, screen already off, idle for
  a further timeout; never while playing, BT-in armed/connected,
  UAC/USB-DAC live, rescanning, MTP/WebDAV busy (same signals as
  `power_policy`). Longer STR deadline → real power-off (RTC
  wakealarm, to be probed).
- On resume: treat the wake press as activity, clear the sleep
  state instead of toggling into the 1-second re-blank; run the
  “nap recovery”: Wi-Fi re-establish, then BT check.
- Wake semantics: Power = wake + screen on; Next = wake, and (once
  event delivery is proven) skip-and-play with the screen off,
  re-suspending on pause.

## Phase 1 (player side, 2026-11-10)

Implemented in [power.c](hiby-r1-modder repo)
(`str_poll`, called from the device loop next to `sleep_poll`):

- **Entry**: screen off for `str_timeout` seconds (setting
  `str_timeout`, default 300, 0 = off; `PLAYER_STR_TIMEOUT` overrides
  for tests), then write `mem` to `/sys/power/state`. Held off while
  playing, BT-in live or a phone connected, USB input live, BT output
  routed, USB DAC output, library rescan, or an MTP transfer
  (`str: held off (<why>)`, throttled). A device veto (write returns,
  suspend success count unchanged) logs
  `str: suspend refused or vetoed` and backs off 60 s.
  Path overrides `PLAYER_STR_STATE` / `PLAYER_STR_SUCCESS` /
  `PLAYER_STR_WAKEUP` exist for the gate.
- **Resume**: `str: resumed (wakeup_count A -> B)`. The wake key is
  given a 1.5 s window (`str_wake_key` in
  [ui_input.c](hiby-r1-modder repo)):
  a transport key keeps the screen off (`str: transport wake`),
  Power lights it, no key lights it after the window
  (`str: wake (no key delivered)`). The delivered toggle that follows
  is eaten once (`sleep: STR wake press eaten`) — this was the Phase 0
  two-press bug. Nap recovery: Wi-Fi off/on bounce
  (`str: Wi-Fi bounce (nap recovery)`), then an NTP re-sync once
  Wi-Fi is back (`after STR wake`).
- **Gates**: `tests/test_player_sleep.py` (source) and
  `./player/vm.sh sleep` (qemu: fake `/sys/power` files + a watcher
  that bumps the success/wakeup counts; asserts entry, resume delta,
  eaten wake press, touch after wake). Both green.

Field results (R1, first Phase 1 build, `str_timeout` 20 for tests):

- **Next wake is delivered as an evdev event** — the Phase 0 open
  question is settled. From STR: `str: entering` →
  `str: resumed (wakeup_count 684 -> 686)`, then
  `key: 163 (gpio-keys) -> player.next`, track skipped and playing.
  No `wakeup_count` latch needed. In that build the resume also lit
  the screen; the screen-off transport wake above was built and
  gated after, but is **not field-tested yet**.
- **BT-connected hold works**: with the owner's phone connected
  (idle), STR stayed off (`str: held off (phone connected (BT in))`)
  and Next simply skipped in screen-off sleep.
- **Wi-Fi recovery is not reliable yet.** After a later unattended
  nap the device woke with Wi-Fi dead; the player's Wi-Fi toggle did
  nothing and a reboot was needed. This is the Phase 0 SDIO wedge
  biting harder than the single off/on bounce handles. Until that is
  hardened (deeper bounce: interface/module level, retry loop with
  verification), **STR is disabled on the owner's device**
  (`str_timeout: 0` in settings, set 2026-11-10) so the player stays
  usable.

Still open: field-test the screen-off Next wake; harden Wi-Fi nap
recovery and re-enable; 50x suspend/resume soak; suspend-current
measurement; drop kernel patch 0024 + `CONFIG_PM_DEBUG` before any
release image.
