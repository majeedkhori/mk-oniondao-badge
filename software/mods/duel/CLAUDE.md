# ESP-Duel — Project Conventions

## Tech Stack (LOCKED)

| Layer | Choice |
|-------|--------|
| MCU | ESP32-S3-WROOM-1-N8R8 (8MB flash, 8MB OPI PSRAM) |
| Framework | ESP-IDF v5.5.x + Arduino core as managed component |
| Display | GxEPD2_270_GDEY027T91 (SSD1680), 264×176, `setRotation(1)` |
| Display lib | GxEPD2 (in `../../components/GxEPD2`) |
| Fonts | FreeMonoBold9pt7b, FreeMono9pt7b, FreeMonoBold24pt7b |
| Buttons | TCA9534 @ I²C 0x20, GPIO1 interrupt |
| Crypto | SparkFun_ATECCX08a library (in `../../components/SparkFun_ATECCX08a`) |
| Transport | ESP-NOW with PMK "NullCity-Badge-1", LMK "Badge-LinkKey-01" |
| Persistence | Arduino Preferences, namespace "duel" |
| Hash | mbedtls/sha256.h (already linked in esp-idf) |

## Commands

```bash
# Build
idf.py build

# Flash + monitor (replace port as needed)
idf.py -p /dev/tty.usbserial-10 flash monitor

# Clean
idf.py fullclean
```

## Architecture Rules

- **All game state is RAM-only.** Only W/L/D counters persist to NVS. Power loss during a duel = abandoned match, not corruption.
- **Never call `esp_now_send()` from inside `on_recv()`.** Set a flag; dispatch from the main loop. (Same pattern as main.cpp for `g_pong_flag`, `g_challenge_flag`.)
- **Never call ATECC ops from inside `on_recv()`.** ATECC sign/verify are ~70–80ms blocking I²C calls. Defer via `g_needs_verify` pattern (see main.cpp:2036).
- **`DuelMsg` is the only message struct for this mod.** Do not reuse `BeaconMsg` — fields don't map.
- **HP is computed deterministically local.** HP values in `DuelMsg` fields (`my_hp`, `op_hp`) are desync-detection only, never authoritative. Both badges run `resolve_round()` independently on the same two revealed moves.
- **`game_id` + `round` are in the signed region.** Never accept a DuelMsg where `game_id` doesn't match `g_current_game_id`.
- **Sleep is suppressed during any non-IDLE state.** Refresh `s_last_activity = millis()` each loop tick when `g_duel_state != S_IDLE`.

## Message Protocol Rules

- `DuelMsg` is exactly 205 bytes (`static_assert` enforces this at compile time)
- Signed region = bytes [0..76] (77 bytes): type through nonce
- Signature = ECDSA-P256 over `SHA-256(signed_region)`, stored in `sig[64]`
- All-zero `pubkey` → unsigned mode (ATECC absent or unprovisioned); receiver accepts but marks unverified
- Commit hash = `SHA-256(round || move || nonce)` — nonce is 16 bytes from `esp_fill_random()`
- Verify reveal by recomputing hash locally; do NOT trust `move_hash` field in reveal frame

## Reliability Rules

- Every phase transition message (DP_COMMIT, DP_REVEAL, DP_FORFEIT) goes through `send_reliable()`
- `send_reliable()` stores the pending frame and arms the retry timer
- Retransmit every 3 s, max 6 retries (18 s of retries within the 20 s forfeit window)
- `clear_pending()` disarms retry — call it when the expected next-phase frame arrives
- Forfeit timeout (20 s with no ACK by progression) → transition to S_RESULT(WIN, TIMEOUT)
- Game watchdog (25 s with no valid DuelMsg from current opponent in any active state) → S_RESULT(WIN, DISCONNECTED)

## Display Rules

- Always `setRotation(1)` (landscape: 264 wide × 176 tall)
- Baseline-anchored text cursors (y is the text baseline, not the top)
- `FreeMonoBold9pt7b`: xAdvance=11px, line height=18px — use for headers and selected items
- `FreeMono9pt7b`: xAdvance=11px, line height=18px — use for body and footer
- `FreeMonoBold24pt7b`: use only for result headline (WIN/LOSE/DRAW)
- Partial refresh regions:
  - Move selector row: `setPartialWindow(0, 100, 264, 70)`
  - HP bars: `setPartialWindow(0, 20, 264, 90)`
  - Full screen: `setFullWindow()`
- Full refresh every 10 partials (track `g_partial_count`, reset on full)
- **No `display.hibernate()` between consecutive partial updates during an active duel**
- Call `display.hibernate()` on transition to S_IDLE

## Button Rules

- Button bitmasks: `BTN_LEFT=(1<<0)`, `BTN_DOWN=(1<<1)`, `BTN_UP=(1<<2)`, `BTN_RIGHT=(1<<3)`, `BTN_SELECT=(1<<4)`, `BTN_CANCEL=(1<<5)`
- Detect press via `pressed = current_btns & ~prev_btns` (edge detection, matches main.cpp pattern)
- Once SELECT commits a move (`g_move_committed = true`), ignore further SELECT until next round
- Add 150 ms software debounce after any SELECT that triggers a state transition

## Security Rules

- PMK and LMK strings are shared across all OnionDAO badges; do not change them
- Commit-reveal: sign the COMMIT (move_hash is inside signed region); verify reveal by hash recompute — never sign the reveal
- Reject any DuelMsg where `game_id != g_current_game_id`
- On hash mismatch between revealed move+nonce and committed hash: that player forfeits the round; both mismatch = round draw
- Peer registration: peers stay **unencrypted for the entire duel**. Do NOT upgrade to encrypted on accept — doing so creates a fatal asymmetry (the accepter flips its peer to encrypted and sends ACCEPT encrypted while the inviter still has it unencrypted, so the inviter can never decrypt the ACCEPT and hangs on "Waiting…"). Commit-reveal + optional ATECC signing already provide cheat-resistance, so plaintext frames are acceptable.

## ESPNow Peer Rules

- Before sending DP_INVITE: add target as unencrypted peer (mirrors `add_ping_peer`)
- Keep the opponent peer unencrypted for the whole duel (`ensure_duel_peer`). Never `esp_now_mod_peer` it to `encrypt=true` — see Security Rules above.
- Use the factory MAC from `esp_read_mac(mac, ESP_MAC_WIFI_STA)` (cached in `g_my_mac`), NOT `WiFi.macAddress()` — the latter returns all-zeros if read before the WiFi driver has started, which made every badge name itself `NCB-000000` and broke the mutual-invite MAC tie-break.
- Check `esp_now_add_peer()` return value — never ignore it silently
- If `ESP_ERR_ESPNOW_FULL`: delete LRU registered peer (`esp_now_del_peer`), retry once
- A duel only needs 2 registered peers: broadcast (unencrypted) + current opponent

## ATECC Rules

- Init pattern: `g_atecc.begin(0x60, Wire, Serial)` → check `configLockStatus && slot0LockStatus` → load pubkey
- Never auto-provision the ATECC (provisioning belongs to the main badge firmware)
- Gate all signing/verifying behind `if (g_atecc_ok)`
- Always `wakeUp()` before and `sleep()` after ATECC operations
- ATECC absent or unprovisioned → unsigned mode; zero out pubkey/sig fields; game continues normally

## Testing Rules

- Phase 1: at least build + flash succeeds
- Phase 2: 10+ tests (button, ESPNow, reliability, ATECC, struct size)
- Phase 3: 20+ tests (full state machine, commit-reveal, damage table, forfeit, NVS)
- Phase 4: 10+ tests (rendering, HP bars, move selector, refresh behavior)
- Phase 5: 10+ tests (ATECC round-trip, NVS persist, peer eviction, end-to-end)
- `static_assert(sizeof(DuelMsg) == 205)` — compile-time enforced

## Key Constraints

- E-paper partial refresh ~300–500 ms; full refresh ~1700 ms — do not attempt real-time animation
- ATECC sign = ~80 ms blocking — never call in recv callback, button ISR, or any interrupt context
- ESP-NOW peer table: 20 total, 17 encrypted max — clean up after each duel
- Deep sleep wipes all WiFi + ESP-NOW state — never deep sleep during an active duel
- All game state is ROM-only after compile; no config files at runtime

## Environment Variables / NVS Keys

| Namespace | Key | Type | Purpose |
|-----------|-----|------|---------|
| `badge` | `callsign` | String | Player display name (read-only in duel) |
| `duel` | `wins` | uint32 | Win count |
| `duel` | `losses` | uint32 | Loss count |
| `duel` | `draws` | uint32 | Draw count |

## Reference Documents

- `presearch.md` — architecture decisions, team debate, gap analysis
- `PRD.md` — phased implementation plan with per-phase test checklists
- `../../docs/HARDWARE.md` — pin assignments and peripheral details
- `../../docs/PINOUT.md` — complete GPIO table
- `../../examples/basic-menu-adjustable-homescreen/firmware/main/main.cpp` — canonical source for ESPNow init, ATECC sign/verify, display patterns, button handling
- `../tamagotchi/` — standalone mod project template to copy for scaffold
