# Onion OS Scripts

## Build and Flash Helper

`build-flash.sh` builds Onion OS and flashes the first attached ESP32-S3 board
it can find:

```sh
scripts/build-flash.sh
```

Run it from the Onion OS project directory. Use `--monitor` to open the serial
monitor after flashing, or `--port /dev/cu.usbserial-10` to choose a specific
serial port.

The helper auto-loads common ESP-IDF installs, including
`~/.espressif/v5.5.4/esp-idf/export.sh`. Set
`IDF_EXPORT=/path/to/esp-idf/export.sh` if your ESP-IDF install lives somewhere
else.

## Lua Scripts

Example Lua scripts in this directory are meant to be copied into the Onion OS
script registry or served through the script manifest.

## Image Browser

`image-browser.lua` browses every downloaded image stored in SPIFFS as
`/images_*.pbm` or `/images_*.bmp`.

Controls:

- `UP` / `LEFT`: previous image
- `DOWN` / `RIGHT`: next image
- `SELECT`: redraw current image
- `CANCEL`: return to Onion OS

To install it through a manifest, serve this script and the image assets:

```json
{
  "scripts": [
    {
      "name": "image-browser.lua",
      "url": "https://example.com/image-browser.lua"
    }
  ],
  "images": [
    {
      "name": "poster.pbm",
      "url": "https://example.com/poster.pbm"
    }
  ]
}
```
