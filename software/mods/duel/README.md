# ESP-Duel

A two-player dueling game for the OnionDAO Badge. Challenge nearby badges over ESP-NOW, pick your move, and let the cryptographic commit-reveal protocol ensure fair play.

## Gameplay

Players pick one move per round simultaneously using a commit-reveal protocol:
- SLASH — aggressive attack
- SHIELD — defensive parry
- BLAST — area attack

Damage table:
| You \ Opp | SLASH | SHIELD | BLAST |
|-----------|-------|--------|-------|
| SLASH     | 30/30 | 10/0   | 20/30 |
| SHIELD    | 0/10  | 0/0    | 0/0   |
| BLAST     | 30/20 | 0/0    | 20/20 |

(Damage format: you take / opponent takes)

First player to reach 0 HP loses. Simultaneous KO = draw. No time limit on picks; game times out after 20s waiting for the opponent.

## Hardware

| Component | Detail |
|-----------|--------|
| MCU | ESP32-S3-WROOM-1-N8R8 |
| Display | GxEPD2_270_GDEY027T91, 264x176, e-paper |
| Buttons | TCA9534 @ I2C 0x20, GPIO1 INT |
| Crypto | ATECC608B @ I2C 0x60 (optional — game runs without it) |
| Transport | ESP-NOW, PMK "NullCity-Badge-1" |

## GPIO Table

| Pin | Function |
|-----|----------|
| GPIO 18 | PWR rail enable |
| GPIO 8  | ATECC SE_EN |
| GPIO 9  | SCL (I2C) |
| GPIO 10 | SDA (I2C) |
| GPIO 11 | EPD SCK (SPI) |
| GPIO 12 | EPD CS |
| GPIO 13 | EPD DC |
| GPIO 14 | EPD RST |
| GPIO 17 | EPD MOSI |
| GPIO 21 | EPD BUSY |
| GPIO 1  | Button interrupt (TCA9534 INT) |

## Build & Flash

Prerequisites: ESP-IDF v5.5.x installed.

```bash
# Set up environment
source ~/.espressif/v5.5.4/esp-idf/export.sh

# Build
cd software/mods/duel
idf.py build

# Flash (replace port as needed)
idf.py -p /dev/tty.usbserial-10 flash monitor
```

## Navigation

- In the peer list (IDLE): UP/DOWN to select, SELECT to challenge, CXL to exit
- On challenge received: SELECT to accept, CXL to decline
- Picking a move: LEFT/RIGHT to cycle moves, SELECT to lock in, CXL to forfeit
- On result screen: SELECT to rematch, CXL to return to peer list

## Known Limitations

- Requires two badges running ESP-Duel to play (or test-mode simulation with one badge)
- E-paper refresh is ~300-500ms for partial, ~1700ms for full — not real-time
- ATECC608B must be pre-provisioned by the main badge firmware; this mod reads the key only
- Peer list is populated by other badges broadcasting their presence every 5s; allow a few seconds for peers to appear after launch
- Win/loss/draw records persist in NVS (namespace "duel") and survive power cycles
