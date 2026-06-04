# Sound Module self-test

Bring-up / solder-verification firmware for the OnionDAO Badge **Sound
Module** — NS4168 Class-D amp (speaker) + SPH0641 PDM mic. Flash it right
after soldering the module to confirm both halves work and every joint is
solid.

## What it does

1. Asserts the peripheral power rail (`GPIO18` HIGH).
2. **Speaker** — plays a 1 kHz tone for ~2 s. You should *hear a beep*.
3. **Mic** — captures PDM audio continuously and prints `RMS / peak` plus a
   live bar graph over serial (115200 baud). Tap or blow on the mic and the
   numbers should jump well above the quiet-room noise floor.

## Pick your port variant

The module can be wired to three different side-port populations (L1 / L2 /
R), each with a different GPIO map. The top of [`main/main.cpp`](firmware/main/main.cpp)
has a `#define` block — `VARIANT_L1` is enabled by default (most production
boards). If the test fails, switch to the next variant and re-flash before
suspecting hardware:

| Variant | Mic data | BCLK | WS | SDO | CTRL |
|---------|----------|------|----|-----|------|
| **L1**  | 48 | 47 | 19 | 42 | 41 |
| **L2**  | 40 | 41 | 42 | 19 | 47 |
| **R**   | 38 | 39 | 16 | 15 |  7 |

> The Sound Module shares these GPIOs with the CC1101 radio slot — only one
> can be active. Don't run CC1101 firmware at the same time.

## Build & flash

```sh
cd firmware
idf.py set-target esp32s3      # first time only
idf.py build flash monitor
```

Requires ESP-IDF **v5.5.x** (see [`../../guides/esp-idf-vscode-setup.md`](../../guides/esp-idf-vscode-setup.md)).

## Reading the result

| Symptom | Likely cause |
|---------|--------------|
| Tone plays, mic RMS reacts to taps | ✅ both halves good — soldering is solid |
| **No tone**, mic works | Check **SDO**, **CTRL**, **BCLK**, **WS** + amp power/GND joints |
| Tone plays, **mic RMS flat / stuck at 0 or full-scale** | Check **Mic data** + **BCLK** + mic VCC/GND joints |
| Neither works, or board resets / brownout | Shared **BCLK/WS** joint, or a **VCC↔GND short** — power off, recheck with a meter |
| Garbage / `ESP_ERROR_CHECK` abort in boot log | Wrong port variant — try L2 then R |

Before flashing, it's worth a quick **meter pass with the board off**: VCC↔GND
must read open (no short), and each module pin should have continuity through
to its GPIO in the table above. That catches bridges and cold joints without
risking the board.
