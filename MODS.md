# Badge Mods by Majeed Khori

This repository is my personal build of the **OnionDAO conference badge** — an
open-source ESP32-S3 hardware badge with an e-paper display, a secure element,
and swappable RF / audio expansion modules.

The **badge hardware and the core Onion OS platform** are the work of the
[OnionDAO team](https://github.com/OnionDAO-git/oniondao-badge). What follows are
the features **I designed and built on top of that platform** — a wireless game,
a sub-GHz radio toolkit, and several smaller apps. Provenance for every feature
is listed under [Credits](#credits) so it's clear what is mine and what is the
upstream project's.

---

## 1. ESP-Duel — a wireless dueling game with cryptographic fair play

**Code:** [`software/mods/duel/`](software/mods/duel) (standalone) ·
[`software/mods/onion-os/scripts/duel.lua`](software/mods/onion-os/scripts/duel.lua) (Onion OS)

ESP-Duel is a 1-v-1 fighting game for the badge. Each round both players secretly
choose a move — **SLASH**, **SHIELD**, or **BLAST** — and the moves resolve
simultaneously against a damage table; first to 0 HP loses.

The interesting part is making "choose simultaneously" actually fair over a
wireless link where either side could cheat by waiting to see the other's move.
ESP-Duel solves this with a **commit–reveal protocol**:

1. Each badge hashes its move with a random nonce — `SHA-256(round | move | nonce)`
   — and broadcasts only the **commit** (the hash).
2. Once both commits are in, each badge **reveals** its move + nonce.
3. Each side verifies the other's reveal against the committed hash before
   damage is applied. You can't change your move after seeing your opponent's,
   and you can't lie about what you committed.

Additional engineering:

- **Hardware-signed moves** — when the badge's **ATECC608B** secure element is
  present, commits are signed and the result screen shows `[verified]`, proving
  the moves came from that physical badge (anti-spoofing). The game still runs
  fully without the secure element.
- **Reliable messaging over ESP-NOW** — ESP-NOW is fire-and-forget with no
  delivery guarantee, so I added an application-level ACK + retry layer (3 s
  retry, capped attempts, 20 s forfeit, game watchdog) and made the round
  protocol **arrival-order-independent**, so a dropped or reordered packet
  recovers instead of desyncing the two badges. These were validated in live
  two-badge matches.
- **Two form factors, one game** — a standalone ESP-IDF firmware
  ([`software/mods/duel/`](software/mods/duel)) for badge-vs-badge play, and an
  Onion OS Lua version ([`duel.lua`](software/mods/onion-os/scripts/duel.lua))
  that plays vs a CPU opponent on a single badge. The opponent is a pluggable
  function, so the same game loop drives both.

> The design rationale (protocol choices, rejected alternatives) is written up in
> [`software/mods/duel/presearch.md`](software/mods/duel/presearch.md) and
> [`PRD.md`](software/mods/duel/PRD.md).

---

## 2. Sub-GHz CC1101 radio toolkit

**Code:** [`software/mods/onion-os/scripts/subghz-tool.lua`](software/mods/onion-os/scripts/subghz-tool.lua) ·
firmware bindings in [`software/mods/onion-os/main/main.cpp`](software/mods/onion-os/main/main.cpp) ·
patch record in [`local-patches/subghz_firmware.patch`](software/mods/onion-os/local-patches/subghz_firmware.patch)

A Flipper-style multitool ("OnionGHz") for the badge's **CC1101** sub-GHz radio
module (315 / 433 / 868 / 915 MHz), built as an Onion OS Lua app backed by
firmware extensions I added to the radio driver:

- **Receive monitor / TX beacon / band scanner / frequency + modulation control**
  over the packet engine.
- **Raw carrier RSSI** (`subghz_rssi`) read straight off the CC1101 status
  register — this senses raw RF energy rather than decoded packets, which is what
  makes a real **spectrum / "find signal"** meter possible (it reacts to
  arbitrary transmitters, not just other badges).
- **Raw OOK record & replay** (`subghz_raw_record` / `subghz_raw_replay`, with
  SPIFFS save/load) — captures the on/off-keyed pulse timing of a remote in
  asynchronous transparent mode and replays it, for cloning **fixed-code**
  remotes you own.

> **Responsible use:** replay works only on *fixed-code* remotes. Modern
> rolling-code openers are not defeated — they reject a replayed capture by
> design. Use only on equipment you own and are authorized to test.

I brought the physical CC1101 module up on the badge (SPI + power debugging,
confirmed genuine TI silicon, validated TX/RX across all four ISM bands).

---

## 3. Onion OS apps & bindings

**Code:** [`software/mods/onion-os/`](software/mods/onion-os)

Smaller additions to the Onion OS Lua platform:

- **Tic-Tac-Toe** and an **ID card** app for the badge menu.
- **Lua bindings** for audio (tone playback + PDM-mic level), 2-D graphics, and
  key/value storage, used by the games above.
- A one-command **build-and-flash helper** that also bundles the Lua apps into
  the badge's SPIFFS filesystem (see [Try it](#try-it)).

---

## Try it

**Already have a badge and just want to play?** Flash the prebuilt image — no
toolchain needed, only `esptool`:

```bash
pip install esptool
cd software/mods/onion-os/prebuilt
./flash.sh
```

**Building from source** (ESP-IDF v5.5.x) — builds the firmware *and* loads the
Lua apps in one step:

```bash
cd software/mods/onion-os
scripts/build-flash.sh --scripts
```

Then open the **Scripts** menu on the badge and pick **`duel`**.

---

## Credits

**My contributions:** ESP-Duel (standalone + Onion OS versions), the sub-GHz
CC1101 toolkit and its firmware bindings, Tic-Tac-Toe, the ID card app, and the
Onion OS audio/graphics/storage Lua bindings & build tooling.

**OnionDAO team** (this badge would not exist without them):

- **Badge hardware** — schematics, PCB, case, BOM (KiCad sources in [`pcb/`](pcb)).
- **Onion OS platform**, MQTT proxy, ESP-IDF migration — *spacemandev / Dev Bharel*.
- **Solana wallet (QR), MQTT chat, and the ESP-NOW security stack** — interrupt
  buttons, PSRAM attendance log, ATECC hardware-signed beacons, and the
  "Capture the Badge" CTF — *Adem*.

The upstream project lives at
[OnionDAO-git/oniondao-badge](https://github.com/OnionDAO-git/oniondao-badge).
