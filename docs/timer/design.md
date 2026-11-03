<p align="right">
  <a href="design.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Speaker Timer

An offline timer for events where each speaker shares for 3 minutes and then takes questions for 2. One press starts the 5-minute round; the device beeps once when the sharing time is up and twice when the round ends. It replaces the baseline demo menu: the device boots straight into the timer.

## Controls

The device has four keys: the hardware power key (hold 0.5 s to turn on, about 2 s to turn off) and three function keys, Up, Down, and OK. Each function key has one meaning and acts the moment it is pressed:

- **OK** starts a round.
- **Down** jumps to the next segment.
- **Up** resets to Ready.

| Screen | OK | Down | Up |
| --- | --- | --- | --- |
| Ready | Start the round | Nothing | Nothing |
| Sharing | Nothing | Skip to discussion, silently | Back to Ready, silently |
| Discussion | Nothing | End the round, back to Ready, silently | Back to Ready, silently |
| Time's up | Back to Ready | Back to Ready | Back to Ready |

OK does nothing while a round runs, so it cannot cut a speaker off. A press acts on the segment the screen shows: Down pressed right at the 3:00 beep skips the sharing rather than ending the discussion that just began. Holding a key does nothing more than pressing it. A single press takes effect at once, so a stray Up during a talk clears the time; the design accepts this because the timekeeper holds the device.

## Screens

From top to bottom: battery, segment label, large digits, a two-part progress bar (3 : 2), and the key hint. All text on the device is Chinese; the exact strings are in `main/timer_ui.c`.

- **Ready**: a gray "share 3 + discuss 2" label, white `3:00`, an empty bar, and the hint "press OK to start".
- **Sharing**: a green "share" label; the digits count the segment down from 3:00 and turn yellow for the last 30 seconds. Hint on two lines: "Down: skip to discussion" and "Up: reset".
- **Discussion**: a blue "discuss" label; the digits count down from 2:00, yellow for the last 30 seconds. Hint: "Down: end the round" and "Up: reset".
- **Time's up**: a red "time's up" label; red digits count the overtime up from `+0:00`, capped at `+9:59`. Hint: "press OK to go back".

## Sound

The timer beeps only at two moments: once (220 ms) when the 3 minutes end and twice (160 ms each, 110 ms apart) when the 5 minutes end. Each beep is an 880 Hz sine with 15 ms fades, about 10 dB below digital full scale. Keys never beep. The volume is fixed at the codec's full output (100 %) and cannot be changed on the device. If the codec fails to start, Ready shows "sound unavailable" and the timer still works without sound.

## Power

While a segment counts down, the screen stays on and the chip never sleeps. In Ready or Time's up, ten minutes without a key turn the screen off and the chip enters deep sleep, following the BSP shutdown order. Any key wakes it into Ready, and that press does nothing else. While a USB host is attached, the chip stays awake so the serial port remains available, and only the screen turns off; the first key turns the screen back on and does nothing else.

## Implementation

| File | Role |
| --- | --- |
| `main/round_timer.c` | State machine and display values, free of ESP-IDF and LVGL; host-tested in `tests/test_round_timer.c` |
| `main/timer_ui.c` | The single LVGL screen; redraws only what changed |
| `main/timer_sound.c` | Beep worker task with a non-blocking queue |
| `main/main.c` | Board bring-up, key queue and wake guards, the 50 ms clock loop, battery polling, idle deep sleep |
| `tools/timer_fonts.py` | Regenerates `assets/fonts/timer/` from the pinned Noto Sans SC Medium; `tests/test_timer_fonts.py` checks glyph coverage |

Segments end on their computed boundary rather than on the tick that notices them, so a slow loop cannot stretch a round. The timer stores nothing in NVS. The baseline demo sources remain in `main/` for the upstream host tests but are not built. Bluetooth is disabled in `sdkconfig.defaults`; the partition table is the default one.

## Status

Implemented on `feature/timer` (based on `main` at `1802df5`). Host tests and the firmware build pass. The first version, with hold-to-skip and adjustable volume, was replaced the same day by the key scheme and fixed volume above. On 2026-11-03 that build (merged image SHA-256 `452d190d…a0f4`, built from the uncommitted tree) was flashed to the device. The user reported that every on-device check passed: Chinese text and layout on all four screens, the three keys in every state, the 30-second yellow warning, one beep at 3:00 and two at 5:00 at full volume, the overtime count, and the optional screen-off, deep-sleep and wake checks. Battery life and standby current have not been measured.

A device that last ran Blog Cards has a different partition table and a bootloader built with `CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP`; install the timer on it with the merged image (bootloader, partition table, and app), not the app alone.
