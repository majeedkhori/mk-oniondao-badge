#!/usr/bin/env bash
#
# Flash a prebuilt Onion OS image (+ the bundled Lua apps, including ESP-Duel)
# to an OnionDAO badge. No ESP-IDF required — only `esptool` and a USB cable:
#
#   pip install esptool
#   ./flash.sh                 # auto-detect the serial port
#   ./flash.sh /dev/cu.usbserial-10   # or pass it explicitly
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="${1:-${PORT:-}}"
BAUD="${BAUD:-460800}"

ESPTOOL="$(command -v esptool.py || command -v esptool || true)"
if [[ -z "$ESPTOOL" ]]; then
  echo "esptool not found. Install it with:  pip install esptool" >&2
  exit 1
fi

if [[ -z "$PORT" ]]; then
  for p in /dev/cu.usbserial* /dev/cu.wchusbserial* /dev/cu.SLAB_USBtoUART* \
           /dev/ttyUSB* /dev/ttyACM*; do
    [[ -e "$p" ]] && { PORT="$p"; break; }
  done
fi
if [[ -z "$PORT" ]]; then
  echo "No serial port found. Plug in the badge, or pass one explicitly:" >&2
  echo "  ./flash.sh /dev/cu.usbserial-XXXX" >&2
  exit 1
fi

echo "Flashing Onion OS + ESP-Duel to the badge on $PORT (@${BAUD})..."
"$ESPTOOL" --chip esp32s3 -p "$PORT" -b "$BAUD" \
  --before default_reset --after hard_reset write_flash \
  0x0      "$HERE/bootloader.bin" \
  0x8000   "$HERE/partition-table.bin" \
  0xe000   "$HERE/ota_data_initial.bin" \
  0x10000  "$HERE/onion-os.bin" \
  0x670000 "$HERE/spiffs.bin"

echo
echo "Done. On the badge: open the Scripts menu and pick 'duel' to play ESP-Duel vs CPU."
