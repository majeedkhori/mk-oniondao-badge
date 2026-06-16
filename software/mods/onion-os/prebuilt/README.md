# Prebuilt Onion OS image (flash & play — no toolchain)

These are ready-to-flash binaries for the **OnionDAO badge** (ESP32-S3-WROOM-1-N8R8).
They contain **Onion OS** plus the bundled Lua apps — including **ESP-Duel** (the
single-badge, vs-CPU dueling game). Use these if you just want to play without
installing ESP-IDF.

## Flash it

You only need [`esptool`](https://pypi.org/project/esptool/) and a USB-C cable:

```bash
pip install esptool
cd software/mods/onion-os/prebuilt
./flash.sh                       # auto-detects the serial port
# or: ./flash.sh /dev/cu.usbserial-10
```

Then on the badge: open the **Scripts** menu and pick **`duel`**.

> The auto-reset circuit means you don't need to hold BOOT. If flashing ever
> fails to start, hold **BOOT**, tap **RESET**, release **BOOT**, and re-run.

## What's in the image

| File | Flash offset | Contents |
|------|--------------|----------|
| `bootloader.bin`       | `0x0`      | Second-stage bootloader |
| `partition-table.bin`  | `0x8000`   | Partition table (8 MB, OTA + SPIFFS) |
| `ota_data_initial.bin` | `0xe000`   | OTA selection data |
| `onion-os.bin`         | `0x10000`  | Onion OS firmware (app) |
| `spiffs.bin`           | `0x670000` | Lua apps: `duel`, `subghz-tool`, `subghz-test`, `sound-test`, `image-browser` |

Integrity hashes are in [`SHA256SUMS`](SHA256SUMS) — verify with `shasum -a 256 -c SHA256SUMS`.

## Rebuilding from source

These binaries are produced by the project under
[`software/mods/onion-os/`](..). To rebuild and flash from source in one step
(builds the firmware and re-bundles the scripts):

```bash
cd software/mods/onion-os
scripts/build-flash.sh --scripts
```
