# ESP-Duel — Product Requirements Document

**Mod path:** `software/mods/duel/`  
**Platform:** OnionDAO Badge (ESP32-S3, base badge only)  
**Framework:** ESP-IDF v5.5 + Arduino core  
**Timeline:** 1–3 days  
**References:** `presearch.md`

---

## Phase Dependency Map

```
Phase 1 (Scaffold)
  └── Phase 2 (Core Infrastructure)
        └── Phase 3 (State Machine + Protocol)
              └── Phase 4 (UI Rendering)
                    └── Phase 5 (Polish + ATECC + Peer Mgmt)
```

---

## Phase 1: Project Scaffold

**Goal:** Standalone ESP-IDF project that compiles and flashes.  
**Depends on:** Nothing  
**Estimated effort:** 2 hours

### Requirements
- [ ] Create `software/mods/duel/` directory structure mirroring `software/mods/tamagotchi/`
- [ ] `CMakeLists.txt` (root): `cmake_minimum_required`, `include($ENV{IDF_PATH}/tools/cmake/project.cmake)`, `project(duel)`, `EXTRA_COMPONENT_DIRS` pointing to `../../components`
- [ ] `main/CMakeLists.txt`: `idf_component_register(SRCS "main.cpp" INCLUDE_DIRS "." REQUIRES espressif__arduino-esp32 GxEPD2 Adafruit_GFX Adafruit_BusIO SparkFun_ATECCX08a)`
- [ ] Copy `sdkconfig.defaults` from tamagotchi verbatim (S3 target, 8MB flash, OPI PSRAM, same partition table)
- [ ] Copy `partitions.csv` from tamagotchi verbatim
- [ ] Copy or symlink `badge_pins.h` from tamagotchi or basic-menu firmware (defines PIN_EPD_*, PIN_SDA, PIN_SCL, PIN_BTN_IRQ, PIN_PWR, PIN_SE_EN)
- [ ] `main/idf_component.yml` with `espressif__arduino-esp32` dependency (same version as tamagotchi)
- [ ] Minimal `main.cpp`: `setup()` + empty `loop()` that compiles
- [ ] `.gitignore` excluding `build/`, `sdkconfig`, `dependencies.lock` (managed files)

### Acceptance Criteria
- `idf.py build` completes without errors
- `idf.py flash monitor` runs and shows serial output

---

## Phase 2: Core Infrastructure

**Goal:** Display, buttons, ESPNow (with send callback), and ATECC all working.  
**Depends on:** Phase 1  
**Estimated effort:** 4 hours

### Requirements

**Display init (lifted from main.cpp):**
- [ ] `GxEPD2_BW<GxEPD2_270_GDEY027T91, ...> display(...)` with pins from `badge_pins.h`
- [ ] `display.init(115200, true, 10, false); display.setRotation(1)`
- [ ] `init_peripherals()`: GPIO18 HIGH (power rail), GPIO8 HIGH (SE_EN), Wire.begin, TCA9534 all-input, SPI.begin, display init
- [ ] `read_buttons()`: I²C TCA9534 reg 0x00, invert active-LOW, mask 0x3F — returns bitmask
- [ ] Button ISR on GPIO1 FALLING edge (sets `g_btn_irq = true`)
- [ ] Button defines: `BTN_LEFT`, `BTN_DOWN`, `BTN_UP`, `BTN_RIGHT`, `BTN_SELECT`, `BTN_CANCEL`

**ESPNow init:**
- [ ] `init_espnow()`: WiFi STA, `esp_now_init()`, `esp_now_register_recv_cb(on_recv)`, **`esp_now_register_send_cb(on_send)`** (new — codebase omits this), `esp_now_set_pmk("NullCity-Badge-1")`, add broadcast peer unencrypted
- [ ] `on_send(mac, status)`: if `status != ESP_NOW_SEND_SUCCESS` and `g_pending_ack`: log failure (retransmit timer handles retry — don't retry in callback)
- [ ] `g_callsign[16]`: load from NVS "badge"/"callsign"; fallback to MAC-derived "NCB-XXXXXX"

**Reliability manager:**
- [ ] `struct PendingSend { DuelMsg msg; bool active; uint32_t first_sent_at; uint32_t last_sent_at; int retries; uint8_t peer_mac[6]; }`
- [ ] `send_reliable(mac, msg)`: stores in `g_pending_send`, sends immediately, sets `active=true`
- [ ] In main loop: if `active && millis()-last_sent_at > 3000 && retries < 6` → resend; if `retries >= 6 || millis()-first_sent_at > 20000` → forfeit callback
- [ ] `clear_pending()`: sets `active=false`; called when expected next-phase frame arrives
- [ ] `on_forfeit_timeout()`: transition to S_RESULT(WIN, reason=TIMEOUT)

**ATECC init (exact pattern from main.cpp:530-570):**
- [ ] `check_atecc()` → sets `g_atecc_ok`, loads `g_atecc_pubkey[64]`
- [ ] Unsigned-mode fallback when chip absent or unprovisioned (never block init)

**DuelMsg types:**
- [ ] `enum MsgType` extended with `MSG_DUEL = 6`
- [ ] `enum DuelPhase { DP_INVITE, DP_ACCEPT, DP_DECLINE, DP_COMMIT, DP_REVEAL, DP_FORFEIT }`
- [ ] `enum DuelMove { MV_NONE=0, MV_SLASH=1, MV_SHIELD=2, MV_BLAST=3 }`
- [ ] `struct DuelMsg` exactly as specified in presearch §2.2 (205 bytes)

**Sleep suppression:**
- [ ] `s_last_activity` refresh: in main loop, if `g_duel_state != S_IDLE` → `s_last_activity = millis()` (prevents 60s deep sleep during active game)

### Tests (10+ minimum)
- [ ] `read_buttons()` returns correct bitmask for each button (bench test with button press)
- [ ] ESPNow init succeeds; send callback registered (verify via `esp_now_send` to self + loopback)
- [ ] Reliability manager retransmits after 3s (simulate dropped frame via never-ACKing peer)
- [ ] Reliability manager fires forfeit after 20s (no acks)
- [ ] `clear_pending()` stops retransmit
- [ ] ATECC init with provisioned chip: `g_atecc_ok == true`, pubkey non-zero
- [ ] ATECC init with absent chip: `g_atecc_ok == false`, no crash, no hang
- [ ] DuelMsg struct size == 205 bytes (`static_assert(sizeof(DuelMsg) == 205)`)
- [ ] Sleep suppression: last_activity updates every loop when duel_state != S_IDLE
- [ ] Callsign loads from NVS when present; uses MAC fallback when absent

### Acceptance Criteria
- Badge boots, displays "ESP-DUEL" text on e-paper, shows callsign on serial
- ESPNow sends a broadcast DuelMsg; send callback fires
- ATECC reports provisioned/absent on serial

---

## Phase 3: State Machine + Commit-Reveal Protocol

**Goal:** Complete game logic from IDLE through RESULT.  
**Depends on:** Phase 2  
**Estimated effort:** 6 hours

### Requirements

**State enum:**
```c
enum DuelState {
    S_IDLE, S_INVITE_SENT, S_INVITE_RECV,
    S_ROUND_PICK, S_WAIT_COMMIT, S_WAIT_REVEAL,
    S_RESOLVE, S_RESULT
};
```

**S_IDLE:**
- [ ] Peer selection UI (reuse existing peer table from ESPNow, or a simple "pick from scanned callsigns" list)
- [ ] SELECT on chosen peer → generate `game_id = esp_random() & 0xFF`, send `DP_INVITE`, transition to `S_INVITE_SENT`
- [ ] 15s timeout → S_IDLE

**S_INVITE_SENT / S_INVITE_RECV:**
- [ ] `recv DP_ACCEPT` with matching `game_id` → `S_ROUND_PICK(round=1, my_hp=100, op_hp=100)`
- [ ] `recv DP_DECLINE` or `DP_FORFEIT` → S_IDLE
- [ ] `recv DP_INVITE` from same peer while in S_INVITE_SENT (mutual invite): lower-MAC badge sends DP_ACCEPT and treats itself as S_INVITE_RECV/accept; higher-MAC drops its own pending invite
- [ ] Mutual-invite tie-break: `memcmp(my_mac, their_mac, 6) < 0` → I am lower-MAC → I accept theirs
- [ ] S_INVITE_RECV: SELECT→accept (send DP_ACCEPT), CANCEL/timeout 15s → send DP_DECLINE → S_IDLE

**S_ROUND_PICK:**
- [ ] `g_selected_move`: navigated L/R through {MV_SLASH, MV_SHIELD, MV_BLAST}
- [ ] SELECT → commit: generate `nonce[16]` via `esp_fill_random()`, compute `SHA-256(round || move || nonce)` → `move_hash`
- [ ] If `g_atecc_ok`: sign the first 77 bytes of DuelMsg containing the move_hash (via existing `createSignature` pattern); store sig + pubkey
- [ ] Send `DP_COMMIT` via `send_reliable()`; transition to `S_WAIT_COMMIT`
- [ ] CANCEL → send `DP_FORFEIT` → S_IDLE
- [ ] `g_move_committed = true` after commit; ignore further SELECT until round resolves

**S_WAIT_COMMIT:**
- [ ] `recv DP_COMMIT` (matching game_id, round, valid sig if sig_present): store opponent commit hash; `clear_pending()`; send `DP_REVEAL` via `send_reliable()`; transition to `S_WAIT_REVEAL`
- [ ] `recv DP_FORFEIT` → S_RESULT(WIN, reason=OPPONENT_QUIT)
- [ ] Reliability manager timeout → on_forfeit_timeout()

**S_WAIT_REVEAL:**
- [ ] `recv DP_REVEAL`:
  - Recompute `SHA-256(round || move || nonce)` from received reveal data
  - If hash matches stored commit: proceed to S_RESOLVE with opponent move
  - If hash mismatch: S_RESULT(WIN, reason=OPPONENT_CHEATED)
  - If sig present and ATECC available: `verifySignature(digest, sig, pubkey)` (deferred to main loop, not in recv callback)
- [ ] `recv DP_FORFEIT` → S_RESULT(WIN)
- [ ] Reliability manager timeout → on_forfeit_timeout()

**S_RESOLVE (pure local, no network):**
```c
// Damage table lookup: damage_to_me, damage_to_opponent
// Returns new (my_hp, op_hp)
static void resolve_round(DuelMove my_move, DuelMove op_move,
                          uint8_t* my_hp, uint8_t* op_hp);
```
- [ ] Apply table from presearch §2.4 exactly
- [ ] Clamp HP to 0 minimum
- [ ] `g_move_committed = false`; `round++`
- [ ] If `my_hp <= 0 || op_hp <= 0` → determine win/loss/draw → S_RESULT
- [ ] Else → S_ROUND_PICK

**S_RESULT:**
- [ ] Persist: load NVS "duel" keys, increment wins/losses/draws, write back
- [ ] Display result screen
- [ ] SELECT → send DP_INVITE to same peer → S_INVITE_SENT (rematch)
- [ ] CANCEL → S_IDLE

**Game watchdog:**
- [ ] `g_last_duel_recv_at`: updated on every valid DuelMsg from current opponent
- [ ] In main loop: if not S_IDLE && not S_RESULT && `millis()-g_last_duel_recv_at > 25000` → on_forfeit_timeout()

**ESPNow peer registration for duel:**
- [ ] On DP_INVITE send: `add_duel_peer_unencrypted(mac)` (mirrors `add_ping_peer`)
- [ ] On DP_ACCEPT recv: `ensure_duel_peer_encrypted(mac)` (mirrors `ensure_unicast_peer`)
- [ ] `add_duel_peer_unencrypted`: check `esp_now_add_peer()` return; if `ESP_ERR_ESPNOW_FULL` → `evict_lru_peer()` (del oldest `g_last_seen` registered peer, retry once)

### Tests (20+ minimum)
- [ ] State machine: IDLE → INVITE_SENT → (recv ACCEPT) → ROUND_PICK
- [ ] State machine: IDLE → INVITE_RECV → (SELECT) → ROUND_PICK
- [ ] State machine: IDLE → INVITE_SENT → (timeout 15s) → IDLE
- [ ] State machine: INVITE_RECV → (CANCEL) → IDLE + DP_DECLINE sent
- [ ] Mutual invite: lower-MAC badge accepts; higher-MAC converts to INVITE_RECV
- [ ] Commit-reveal: hash(round|move|nonce) matches on both sides for all 3 moves
- [ ] Commit-reveal: mismatch → S_RESULT(WIN, CHEATED)
- [ ] Damage table: all 9 combinations correct (unit test, no hardware)
- [ ] HP clamping: move that would bring to -10 → 0
- [ ] Game end: my_hp ≤ 0 → S_RESULT(LOSS); op_hp ≤ 0 → S_RESULT(WIN); both ≤ 0 → DRAW
- [ ] Round progression: S_RESOLVE → S_ROUND_PICK with round++
- [ ] Forfeit on DP_FORFEIT recv → S_RESULT(WIN) from any active state
- [ ] Forfeit timeout (20s) → S_RESULT(WIN, TIMEOUT) from S_WAIT_COMMIT/WAIT_REVEAL
- [ ] Game watchdog: no msg for 25s → S_RESULT
- [ ] Sleep suppression: s_last_activity updated when state != S_IDLE
- [ ] g_move_committed prevents double-commit on rapid SELECT
- [ ] NVS: W/L/D increment and persist correctly across power cycle
- [ ] Peer registration: ESP_ERR_ESPNOW_FULL → evict LRU → retry
- [ ] ATECC: if g_atecc_ok → sig in DP_COMMIT; if !g_atecc_ok → pubkey/sig zeroed
- [ ] On_recv: does not call esp_now_send directly; sets flags for main-loop dispatch

### Acceptance Criteria
- Two-badge test: complete a duel from IDLE to S_RESULT on both badges
- Both badges reach S_RESULT with same final HP values
- Forfeit timeout fires and resolves cleanly on both badges

---

## Phase 4: UI Rendering

**Goal:** All game screens drawn correctly; e-paper refresh strategy applied.  
**Depends on:** Phase 3  
**Estimated effort:** 4 hours

### Requirements

**Fonts (already in components):**
- `FreeMonoBold9pt7b` — headers, move selector selected item
- `FreeMono9pt7b` — body, footer
- `FreeMonoBold24pt7b` — WIN/LOSE result headline

**Screen: S_IDLE / peer picker:**
- [ ] Simple list of scanned callsigns (reuse peer table populated by ESPNow beacon mode)
- [ ] UP/DN navigate, SELECT picks target, footer "SEL:challenge CXL:back"
- [ ] If no peers: "No badges nearby. Send a beacon first."

**Screen: S_INVITE_RECV:**
- [ ] Header "DUEL CHALLENGE"
- [ ] Body: "[NAME] wants to duel!"
- [ ] Timeout countdown (update on each loop tick via partial refresh of footer only)
- [ ] Footer "SEL:accept CXL:decline (Ns)"

**Screen: S_ROUND_PICK:**
- [ ] Header: "DUEL  Round N  [peer_name]"
- [ ] HP bars: `drawRect` outline (x=8, w=220, h=10) + `fillRect` proportional fill; `hp/100 * 220` px
- [ ] "ME [callsign]" above my bar, "OP [peer_name]" above their bar
- [ ] Move selector: 3 items horizontal, selected item inverted (fillRect + white text)
- [ ] Footer: "L/R:pick  SEL:commit  CXL:forfeit"
- [ ] Partial refresh: `setPartialWindow(0, 100, 264, 70)` for selector-row changes only
- [ ] Full refresh on entering the screen from a new round (clears ghosting)
- [ ] `g_partial_count` counter: full refresh every 10 partials (anti-ghosting, matches codebase)

**Screen: S_WAIT_COMMIT / S_WAIT_REVEAL:**
- [ ] Replace move selector row with: "Sent ✓ — waiting (retry N/6, Xs left)"
- [ ] Update on each reliability-manager resend tick (partial refresh of waiting row only)
- [ ] Countdown = `(20000 - (millis() - phase_start_ms)) / 1000`

**Screen: S_RESULT:**
- [ ] Full refresh
- [ ] `FreeMonoBold24pt7b` headline: "YOU WIN" / "YOU LOSE" / "DRAW" centred at ~y=60
- [ ] Reason line (FreeMono9pt): "K.O. round N" / "Opponent forfeited" / "Timeout" / "Opponent cheated"
- [ ] Record line: "W:N  L:N  D:N" (loaded from NVS)
- [ ] ATECC indicator: "[verified]" if both badges signed (sig was present + valid); "[unsigned]" otherwise
- [ ] Footer: "SEL:rematch  CXL:menu"

**Refresh rules (enforced):**
- [ ] No `display.hibernate()` between consecutive partial updates while in active duel states
- [ ] `display.hibernate()` called on transition to S_IDLE (done playing)
- [ ] `g_partial_count` reset on every full refresh

### Tests (10+ minimum)
- [ ] HP bar renders at 100%: fills full 220px
- [ ] HP bar renders at 50%: fills exactly 110px
- [ ] HP bar renders at 0%: draws only outline (0 fill)
- [ ] Move selector: all 3 items visible; selected item has inverted bg/text
- [ ] L/R wraps around (BLAST→SLASH, SLASH→BLAST)
- [ ] Waiting screen shows retry counter incrementing
- [ ] WIN result shows correct headline and reason
- [ ] LOSE result shows correct headline
- [ ] DRAW result shows correct headline
- [ ] Result screen shows updated W/L/D from NVS
- [ ] No hibernate between two consecutive partial refreshes (verify via serial log)

### Acceptance Criteria
- All screens render without visual glitches on the actual badge hardware
- HP bars correctly reflect damage after a round
- Waiting screen updates visibly during opponent wait

---

## Phase 5: Polish, ATECC Integration, Peer Management

**Goal:** ATECC signing wired in; peer table robustness; final README.  
**Depends on:** Phase 4  
**Estimated effort:** 2 hours

### Requirements
- [ ] ATECC sign in `S_ROUND_PICK` commit: deferred to main loop (not in recv callback), using existing `g_atecc.wakeUp() → createSignature() → sleep()` pattern
- [ ] `verifySignature()` for opponent commits/reveals: deferred via flag+copy pattern (mirrors `g_needs_verify` in main.cpp:2036)
- [ ] W/L/D shown on result screen, loaded from NVS "duel" namespace
- [ ] `esp_now_del_peer()` + LRU eviction on `ESP_ERR_ESPNOW_FULL`
- [ ] `README.md` in `software/mods/duel/`: gameplay description, build & flash instructions, GPIOs used table, attack rules table, known limitations

### Tests (10 minimum)
- [ ] ATECC sign+verify round-trip: sign a DuelMsg commit, verify on same badge → true
- [ ] ATECC verify with tampered msg: change one byte of move_hash → false
- [ ] ATECC absent: unsigned-mode compiles and runs without crash
- [ ] NVS: wins increments on WIN, losses on LOSS, draws on DRAW
- [ ] NVS: values persist across simulated power cycle (NVS write + re-read)
- [ ] Peer full: evict fires on ESP_ERR_ESPNOW_FULL; peer added on retry
- [ ] README: build instructions accurate (copy from tamagotchi, update project name)
- [ ] `static_assert(sizeof(DuelMsg) == 205)` passes at compile time
- [ ] Full end-to-end with ATECC: both badges show "[verified]" on result screen
- [ ] Full end-to-end without ATECC: both show "[unsigned]", game completes normally

### Acceptance Criteria
- Two-badge test with ATECC: result screen shows "[verified]" on both badges
- Two-badge test without ATECC: game completes normally with "[unsigned]"
- README accurately describes build, flash, and gameplay

---

## MVP Validation Checklist

| # | Requirement | Phase | Test Coverage |
|---|-------------|-------|---------------|
| 1 | Challenge/accept over ESPNow | Phase 3 | State machine tests |
| 2 | Simultaneous commit-reveal | Phase 3 | Hash verify + all combos |
| 3 | E-paper battle display with HP bars | Phase 4 | HP bar rendering tests |
| 4 | Win/loss/draw resolution | Phase 3 | Damage table + end-condition tests |
| 5 | Timeout/forfeit handling | Phase 2+3 | Reliability manager tests |
| 6 | Rematch flow | Phase 3 | S_RESULT → S_INVITE_SENT test |
| 7 | W/L/D record persistence | Phase 5 | NVS persist tests |
| 8 | ATECC optional (graceful degrade) | Phase 2+5 | Unsigned-mode tests |
| 9 | Standalone ESP-IDF project | Phase 1 | Builds + flashes |
| 10 | Sleep suppression during duel | Phase 2 | s_last_activity test |

---

## Stretch Goals (ordered by impact)

1. **RSSI range mechanic (v2):** Add RSSI-gated SLASH with 3-sample moving average and hysteresis. Only after reliable play proven in v1.
2. **Score leaderboard on home screen:** Show top 3 W/L ratio from NVS badge roster.
3. **Spectator mode:** Broadcast DuelMsg phases at low priority so nearby badges can display "[AUGUSTUS vs CALIGULA — Round 3]" on their ESPNow LOG tab.
4. **Audio feedback (sound module only):** Play hit/miss tones on damage resolve.
