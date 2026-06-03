#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET="esp32s3"
BAUD="${BAUD:-460800}"
PORT="${PORT:-}"
MONITOR=0
ERASE=0

usage() {
  cat <<'EOF'
Usage: scripts/build-flash.sh [options]

Build Onion OS and flash it to the first available ESP32-S3 serial board.

Options:
  -p, --port PORT    Flash a specific serial port instead of auto-detecting
  -b, --baud BAUD    Flash baud rate (default: 460800, or $BAUD)
  --erase            Erase flash before flashing
  --monitor          Open idf.py monitor after flashing
  -h, --help         Show this help

Environment:
  IDF_EXPORT=/path/to/export.sh  ESP-IDF export script to source
  PORT=/dev/cu.usbserial-10  Same as --port
  BAUD=921600                Same as --baud
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -p|--port)
      [[ $# -ge 2 ]] || { echo "Missing value for $1" >&2; exit 2; }
      PORT="$2"
      shift 2
      ;;
    -b|--baud)
      [[ $# -ge 2 ]] || { echo "Missing value for $1" >&2; exit 2; }
      BAUD="$2"
      shift 2
      ;;
    --erase)
      ERASE=1
      shift
      ;;
    --monitor)
      MONITOR=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Required command not found: $1" >&2
    echo "Install ESP-IDF or pass IDF_EXPORT=/path/to/esp-idf/export.sh" >&2
    exit 1
  fi
}

source_esp_idf() {
  command -v idf.py >/dev/null 2>&1 && return 0

  local candidates=()
  if [[ -n "${IDF_EXPORT:-}" ]]; then
    candidates+=("$IDF_EXPORT")
  fi

  candidates+=(
    "$HOME/.espressif/v5.5.4/esp-idf/export.sh"
    "$HOME/.espressif-5.5.4/v5.5.4/esp-idf/export.sh"
    "$HOME/esp/esp-idf/export.sh"
    "$HOME"/.espressif/v5.5.*/esp-idf/export.sh
    "$HOME"/.espressif-5.5.*/v5.5.*/esp-idf/export.sh
    "$HOME/.platformio/packages/framework-espidf/export.sh"
  )

  local export_script
  local export_log="${TMPDIR:-/tmp}/onion-os-idf-export.log"
  for export_script in "${candidates[@]}"; do
    if [[ -f "$export_script" ]]; then
      echo "Loading ESP-IDF from $export_script"
      # shellcheck disable=SC1090
      if . "$export_script" >"$export_log" 2>&1; then
        command -v idf.py >/dev/null 2>&1 && return 0
      else
        echo "Failed to load ESP-IDF from $export_script" >&2
        tail -n 40 "$export_log" >&2 || true
      fi
    fi
  done

  return 1
}

esptool_command() {
  if command -v esptool.py >/dev/null 2>&1; then
    echo "esptool.py"
  elif command -v esptool >/dev/null 2>&1; then
    echo "esptool"
  else
    echo ""
  fi
}

candidate_ports() {
  local patterns=(
    "/dev/cu.usbserial"* "/dev/tty.usbserial"*
    "/dev/cu.usbmodem"* "/dev/tty.usbmodem"*
    "/dev/cu.wchusbserial"* "/dev/tty.wchusbserial"*
    "/dev/cu.SLAB_USBtoUART"* "/dev/tty.SLAB_USBtoUART"*
    "/dev/ttyUSB"* "/dev/ttyACM"*
  )
  local port
  for port in "${patterns[@]}"; do
    [[ -e "$port" ]] && printf '%s\n' "$port"
  done | sort -u
}

probe_esp32s3_port() {
  local port="$1"
  local output

  if ! output="$("$ESPTOOL" --chip auto --port "$port" chip_id 2>&1)"; then
    return 1
  fi

  grep -Eiq 'ESP32-S3|ESP32S3|esp32s3' <<<"$output"
}

detect_port() {
  local port
  while IFS= read -r port; do
    echo "Probing $port..." >&2
    if probe_esp32s3_port "$port"; then
      printf '%s\n' "$port"
      return 0
    fi
  done < <(candidate_ports)

  return 1
}

source_esp_idf || true
require_command idf.py
ESPTOOL="$(esptool_command)"
if [[ -z "$ESPTOOL" ]]; then
  echo "Required command not found: esptool.py or esptool" >&2
  echo "Install ESP-IDF or pass IDF_EXPORT=/path/to/esp-idf/export.sh" >&2
  exit 1
fi

cd "$PROJECT_DIR"

if [[ -z "$PORT" ]]; then
  echo "Searching for the first available ESP32-S3 serial board..."
  if ! PORT="$(detect_port)"; then
    echo "No ESP32-S3 board found on common serial ports." >&2
    echo "Connect the badge, or pass a port explicitly with --port /dev/..." >&2
    exit 1
  fi
else
  if [[ ! -e "$PORT" ]]; then
    echo "Serial port does not exist: $PORT" >&2
    exit 1
  fi
  if ! probe_esp32s3_port "$PORT"; then
    echo "Port exists but did not respond as an ESP32-S3: $PORT" >&2
    exit 1
  fi
fi

echo "Using ESP32-S3 on $PORT"

if [[ ! -f sdkconfig ]] || ! grep -q '^CONFIG_IDF_TARGET="esp32s3"$' sdkconfig; then
  idf.py set-target "$TARGET"
fi

idf.py build

if [[ "$ERASE" -eq 1 ]]; then
  idf.py -p "$PORT" -b "$BAUD" erase-flash
fi

if [[ "$MONITOR" -eq 1 ]]; then
  idf.py -p "$PORT" -b "$BAUD" flash monitor
else
  idf.py -p "$PORT" -b "$BAUD" flash
fi
