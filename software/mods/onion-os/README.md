# Onion OS

Onion OS is the badge firmware target for linking a physical OnionDAO badge to
an attendee profile through the Onion server.

This is an ESP-IDF v5.5.x project with Arduino as a component, matching the
other badge firmware in this repo.

## Implemented

- Persistent per-badge hardware ID stored in NVS.
- Hardcoded WiFi for `CIC Guest` / `1nnovation`.
- Hardcoded Onion API server URL: `https://oniondao.dev`.
- Hardcoded MQTT broker URL: `mqtt://shortline.proxy.rlwy.net:20928`.
- Baked-in MQTT broker credentials for the `oniondao` user.
- HTTP badge handshake:
  - `POST /api/badge/handshake`
- MQTT badge bridge:
  - publishes `oniondao/badge/handshake`
  - subscribes to `oniondao/badge/{onionId}/handshake/accepted`
  - subscribes to `oniondao/badge/{onionId}/link/request`
  - subscribes to `oniondao/badge/{onionId}/transaction/request`
  - subscribes to `oniondao/badge/{onionId}/lua/request`
  - publishes link, transaction, and Lua responses.
- E-paper status and approval UI.
- Three-second boot splash generated from `main/logo.png`.
- Home menu showing linked username, Onion balance, script explorer, script sync,
  and profile refresh actions.
- SELECT/CANCEL approval controls through the TCA9534 button expander.
- HTTP fallback for link and transaction responses.
- Script manifest download and SPIFFS storage.
- Lua script execution through Espressif's Lua component.
- Server-pushed Lua script approval popup, install, and run flow.
- Badge-owned Solana Ed25519 wallet generation.
- ATECC608B-backed seed wrapping and approval attestation.
- Solana transaction signing for server-built burn/transfer transactions.

## Solana Key Custody

The badge signs Solana transactions with a software Ed25519 key because the
ATECC608B does not provide Ed25519 signing. The Ed25519 seed is generated on the
ESP32-S3 and is never stored plaintext. It is encrypted in NVS with
XChaCha20-Poly1305 using a wrapping key derived from an ATECC608B HMAC slot.

Production badges must provision `ATECC_HMAC_SLOT` (`10` in the firmware) with a
non-readable HMAC-capable secret. If that slot is empty, readable, locked with
incompatible config, or uses a different provisioning policy, wallet creation and
transaction approval will fail on the badge.

Link and transaction approvals include an `attestation` JSON object containing
the ATECC serial number, HMAC slot, purpose, nonce, subject, and HMAC. The
current `../landing-2026` server routes accept the extra fields but do not yet
verify them. Server-side verification needs the provisioning database or another
trusted record of the per-badge slot secret.

The CryptoAuthLib defaults are pinned for the badge hardware:

- SDA: GPIO 10
- SCL: GPIO 9
- ATECC address: `0xC0` 8-bit (`0x60` 7-bit)
- I2C speed: 100 kHz

## Lua Scripts

The firmware downloads a JSON script manifest, stores scripts in SPIFFS, and can
execute Lua scripts through Espressif's `espressif/lua` ESP-IDF component.
The server can also push a registry script over MQTT on
`oniondao/badge/{onionId}/lua/request`; the badge shows an install approval
popup, stores approved code in SPIFFS, runs it, and responds on
`oniondao/badge/{onionId}/lua/response` plus the HTTP fallback
`POST /api/badge/lua-response`.

Installed scripts are stored as `/scripts_*.lua`. Downloaded image assets are
stored as `/images_*.pbm` or `/images_*.bmp`. The badge's home menu opens a
script explorer that lists both manifest-downloaded scripts and server-pushed
scripts from SPIFFS; selecting a script runs it locally.

The handshake routes currently only return `onionId` and `status`. After a badge
is linked, the firmware refreshes its owner profile by Onion ID with
`GET /api/badge/profile/{onionId}` and falls back to the cached `username` with
`GET /api/public/profile/{username}` for `currentOnionPoints` or
`currentOnionTokens`. If the server has `BADGE_API_KEY` configured, provision the
same key on the badge with the serial command `api-key <badge_api_key>`.

Scripts receive a small global `onion` table:

- `onion.log(message)` writes to serial and the e-paper status line.
- `onion.hardware_id()` returns the badge hardware ID.
- `onion.onion_id()` returns the current Onion ID, or `0` before handshake.
- `onion.wallet()` returns the configured Solana public key, if any.
- `onion.clear_display()` clears the e-paper display and leaves Lua in control
  of the screen until the next button press.
- `onion.release_display()` returns screen ownership to Onion OS after a Lua
  script has drawn to the display.
- `onion.display_bitmap(name, x, y, clear)` draws a downloaded PBM or BMP image
  asset. Pass `-1` for `x` or `y` to center that axis. `clear` defaults to true.
- `onion.images()` returns a table of downloaded PBM and BMP image asset names.
- `onion.buttons()` returns a table with the current badge button state:
  `left`, `down`, `up`, `right`, `select`, `cancel`, and integer `mask`.
- `onion.button_mask(name)` returns the integer mask for a badge button name.
- `onion.sleep(ms)` pauses the Lua script for up to 60 seconds.
- `onion.gpio_read(pin, mode)` reads an expansion GPIO and returns `0` or `1`.
- `onion.gpio_poll(pin, target, timeout_ms, interval_ms, mode)` polls an
  expansion GPIO until it equals `target`, returning `matched, value,
  elapsed_ms`.

GPIO `mode` is optional and may be `"input"`, `"floating"`, `"pullup"`, `"up"`,
`"pulldown"`, or `"down"`. Lua scripts can read the side-port GPIOs only:
`48, 47, 19, 42, 41, 40, 38, 39, 16, 15, 7, 6, 5, 4`.

Expected manifest shape:

```json
{
  "scripts": [
    {
      "name": "hello.lua",
      "url": "https://example.com/hello.lua",
      "autorun": false
    }
  ],
  "images": [
    {
      "name": "poster.pbm",
      "url": "https://example.com/poster.pbm"
    },
    {
      "name": "sponsor.bmp",
      "url": "https://example.com/sponsor.bmp"
    }
  ]
}
```

Image assets should be sized for the 264x176 black-and-white e-paper panel.
PBM files may be binary `P4` or ascii `P1`; BMP files must be uncompressed
1/4/8/24/32-bit BMPs. Images larger than 192 KB or larger than the panel are
rejected.

Example Lua image script:

```lua
local ok, err = onion.display_bitmap("poster.pbm", -1, -1)
if not ok then onion.log(err) end
```

Local example scripts live in [`scripts/`](scripts/). `image-browser.lua`
enumerates downloaded PBM/BMP assets and lets the user move through them with
the badge buttons.

The inspected server currently does not expose a script manifest route, so set
the manifest URL explicitly.

## Configure

Optional compile-time defaults:

```sh
cp main/onion_config.h.example main/onion_config.h
```

Runtime serial commands are persisted in NVS:

```text
api-key <badge_api_key>
mqtt-auth [username] [password] [prefix]
scripts-url <manifest_url>
wallet
keygen confirm
handshake
scripts
run <script_name.lua>
state
help
```

Examples:

```text
api-key badge-api-secret
mqtt-auth badge badge-password oniondao
wallet
handshake
```

`keygen confirm` deletes any unlinked local wallet and creates a new wrapped key.
It refuses to rotate after the badge is linked, because the server may already
have migrated points to the previous Solana address.

## Build

From this directory:

```sh
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

Or use the helper script to auto-detect the first attached ESP32-S3 badge:

```sh
scripts/build-flash.sh
scripts/build-flash.sh --monitor
```

Pass `--port /dev/cu.usbserial-10` if you want to flash a specific port.
Set `IDF_EXPORT=/path/to/esp-idf/export.sh` if your ESP-IDF install is not in a
common location.

The project uses the same 8 MB flash / OPI PSRAM defaults as the existing badge
mods.
