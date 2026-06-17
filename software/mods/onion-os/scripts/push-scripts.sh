#!/usr/bin/env bash
# Push the Lua apps in scripts/ to a badge's SPIFFS over USB — WITHOUT
# rebuilding/reflashing the firmware. Much faster than build-flash.sh --scripts
# when only a .lua changed (e.g. a duel.lua fix). It packs every scripts/*.lua
# into a SPIFFS image (staged as scripts_<name>.lua, the name the firmware
# lists) and writes it to the spiffs partition at 0x670000.
#
# NOTE: this OVERWRITES the whole SPIFFS partition, so it bundles ALL scripts in
# scripts/ — never just the one you changed.
#
#   Usage: scripts/push-scripts.sh [-p /dev/cu.usbserial-XXXX] [-b 460800]
#   Env:   IDF_EXPORT=/path/to/esp-idf/export.sh  PORT=/dev/cu...  BAUD=...
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPTS_DIR="$PROJECT_DIR/scripts"
SPIFFS_OFFSET=0x670000
SPIFFS_SIZE=1572864          # 0x180000 — must match partitions.csv spiffs size
PORT="${PORT:-}"
BAUD="${BAUD:-460800}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    -p|--port) PORT="$2"; shift 2 ;;
    -b|--baud) BAUD="$2"; shift 2 ;;
    -h|--help) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
done

# --- source ESP-IDF so spiffsgen.py + esptool are available -------------------
if ! command -v esptool.py >/dev/null 2>&1 && ! command -v esptool >/dev/null 2>&1; then
  for c in "${IDF_EXPORT:-}" \
           "$HOME/.espressif/v5.5.4/esp-idf/export.sh" \
           "$HOME"/.espressif/v5.5.*/esp-idf/export.sh \
           "$HOME/esp/esp-idf/export.sh"; do
    [[ -n "$c" && -f "$c" ]] || continue
    echo "Loading ESP-IDF from $c"
    # shellcheck disable=SC1090
    . "$c" >/dev/null 2>&1 && break
  done
fi

SPIFFSGEN="${IDF_PATH:-}/components/spiffs/spiffsgen.py"
[[ -f "$SPIFFSGEN" ]] || { echo "spiffsgen.py not found (source ESP-IDF or set IDF_EXPORT)" >&2; exit 1; }
if command -v esptool.py >/dev/null 2>&1; then ESPTOOL=esptool.py
elif command -v esptool   >/dev/null 2>&1; then ESPTOOL=esptool
else echo "esptool not found (source ESP-IDF)" >&2; exit 1; fi

# --- auto-detect the badge's serial port if not given ------------------------
if [[ -z "$PORT" ]]; then
  for p in /dev/cu.usbserial-* /dev/cu.usbmodem* /dev/cu.wchusbserial* /dev/cu.SLAB_USBtoUART*; do
    [[ -e "$p" ]] && { PORT="$p"; break; }
  done
  [[ -n "$PORT" ]] || { echo "No badge serial port found. Plug in a badge via USB, or pass -p." >&2; exit 1; }
fi
echo "Using port: $PORT"

# --- stage scripts/*.lua as scripts_<name>.lua, build image, flash -----------
STAGE="$(mktemp -d -t onion-scripts.XXXXXX)"
IMG="$(mktemp -t onion-spiffs.XXXXXX.bin)"
trap 'rm -rf "$STAGE" "$IMG"' EXIT

count=0
for lua in "$SCRIPTS_DIR"/*.lua; do
  [[ -e "$lua" ]] || continue
  cp "$lua" "$STAGE/scripts_$(basename "$lua")"
  count=$((count + 1))
done
[[ $count -gt 0 ]] || { echo "No .lua files in $SCRIPTS_DIR" >&2; exit 1; }
echo "Bundling $count script(s): $(cd "$STAGE" && echo scripts_*.lua)"

python "$SPIFFSGEN" "$SPIFFS_SIZE" "$STAGE" "$IMG" \
  --page-size 256 --obj-name-len 32 --meta-len 4 --use-magic --use-magic-len

echo "Flashing SPIFFS image ($(wc -c <"$IMG") bytes) to $SPIFFS_OFFSET..."
"$ESPTOOL" --chip esp32s3 -p "$PORT" -b "$BAUD" write_flash "$SPIFFS_OFFSET" "$IMG"
echo "Done. Reboot the badge and open Scripts > duel."
