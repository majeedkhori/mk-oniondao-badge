# ESP-Duel — Pre-Implementation Research

**Date:** 2026-06-02  
**Mode:** Feature (new standalone mod on existing badge platform)  
**Scope:** 1–3 days, solo dev, community PR quality  
**Team:** Architect (Opus 4.6) + Challenger (Opus 4.6) + Researcher (Opus 4.6)

---

## Research Brief

**Questions investigated:** 7  
**High-confidence findings:** All 7 grounded in actual repo source files

| Q | Question | Finding | Confidence |
|---|----------|---------|------------|
| 1 | ESPNow send callback timing | `esp_now_send()` non-blocking (enqueues); RTT 10–50 ms measured in project. Existing code never checks send result — no retry logic anywhere. | High |
| 2 | ATECC608B sign timing | ~80 ms total blocking per sign: 1.5 ms wake + 7 ms loadTempKey + 70 ms signTempKey (hard-coded `delay()` in library at lines 884/917). | High |
| 3 | GxEPD2 partial refresh timing | Full refresh ceiling 1700 ms, partial ceiling 500 ms. Sub-region partial supported but waveform time (~300–500 ms) dominates; skip `hibernate()` between consecutive partial updates. | High |
| 4 | ESPNow peer table limit | 20 total peers max, 17 encrypted max (ESP-IDF documented limit). App's 500-entry roster and ESP-NOW stack table are decoupled in existing code. | High |
| 5 | Commit-reveal viability | SHA-256(round‖move‖nonce) is proven; nonce is mandatory (only 3–4 moves = reversible without nonce). Full pattern in codebase's CTF capture flow (g_cap_nonce, lines 802–808). | High |
| 6 | ESPNow without prior beacon | Unencrypted unicast works without prior beacon; encrypted requires both sides registered. add_ping_peer() / ensure_unicast_peer() pattern already in codebase (lines 697–709). | High |
| 7 | Deep sleep compatibility | EXT0 wake on GPIO1/PBINT reusable. ESPNow state wiped by deep sleep — must re-init on every wake. Stay in active/light sleep during a live duel. | High |

**Key constraints discovered:**
- Zero retransmit logic exists anywhere in the codebase — a Critical gap for any multi-packet exchange
- 60s inactivity sleep (SLEEP_AFTER_MS) fires unconditionally, including mid-game — must suppress
- RSSI is unreliable for gameplay gating: first packet seeds -50 placeholder, only updates on promisc frames for already-known peers, fluctuates ±10 dBm

---

## Constraints (Loop 1)

| Constraint | Value |
|------------|-------|
| MCU | ESP32-S3, dual-core LX7 @ 240 MHz, 8 MB flash, 8 MB OPI PSRAM |
| Display | 264×176 e-paper, GxEPD2, SSD1680 / GDEY027T91 |
| Input | 6 buttons via TCA9534 @ I²C 0x20, interrupt on GPIO1 |
| Crypto | ATECC608B-SSHDA, ECC-P256, I²C 0x60 |
| Transport | ESPNow only (no CC1101 / audio module) |
| Framework | ESP-IDF v5.5 + Arduino core as component |
| ESPNow limit | 250-byte max payload per frame, best-effort (no MAC-layer ACK for broadcast) |
| Peer table | 20 total / 17 encrypted peers in ESP-NOW stack |
| Timing | Partial refresh: ~300–500 ms; Full refresh: ~1700 ms |
| ATECC sign | ~80 ms blocking per sign, must run in main loop not recv callback |
| Project location | `software/mods/duel/` — standalone ESP-IDF project |
| Timeline | 1–3 days |

---

## Innovation Inventory (Loop 1.5)

| # | Innovation | Category | Effort | Impact | Classification |
|---|-----------|----------|--------|--------|----------------|
| 1 | Hash commit-reveal anti-cheat | Novel application | Low | High | **CORE** — Phase 3 |
| 2 | ATECC-signed commits (optional) | Production hardening | Low (additive) | Med | **CORE** — Phase 3 (gated on g_atecc_ok) |
| 3 | App-level ACK+retry layer | Production hardening | Med | High | **CORE** — Phase 2 |
| 4 | Simultaneous resolve (no arbiter) | Novel AI/logic | Low | High | **CORE** — Phase 3 |
| 5 | RSSI-as-range mechanic | Domain intelligence | Med | Low | **CUT** (see below) |
| 6 | Retry counter in waiting display | UX excellence | Low | High | **CORE** — Phase 4 |
| 7 | Lower-MAC tie-break for simultaneous invites | Production hardening | Low | Med | **CORE** — Phase 3 |

**RSSI-gating CUT rationale:** Researcher confirmed RSSI on this hardware: first packet seeds a hardcoded `-50` placeholder, updates only in promiscuous mode on management frames from already-known peers, and fluctuates ±10 dBm at rest. A mechanic the player cannot meaningfully control is frustrating, not fun. Design space: pure RPS triangle is cleaner, faster to ship, and the SLASH/SHIELD/BLAST naming still implies range flavor without hardware-gating it.

---

## Architecture Decisions (Loop 2)

### 2.1 Turn model

| Option | Pros | Cons | Complexity |
|--------|------|------|------------|
| **Simultaneous commit-reveal** | No info advantage; ATECC commit binding has meaning; 2 round-trips/round | 2-phase exchange; needs timeout on both phases | Medium |
| Alternating turns | 1 msg/turn, no commit needed | Second player always knows first player's move → unfair; ATECC signing has no purpose | Low |
| Real-time action | Most exciting in theory | E-paper 500ms latency makes real-time physically impossible | Very High |

**LOCKED: Simultaneous commit-reveal** — only model where the signed commitment is meaningful.

---

### 2.2 Message protocol

One new `MsgType` (`MSG_DUEL = 6`) and one dedicated struct. Reusing BeaconMsg is wrong: its fields (`name`, `text`, `counter`) don't map to duel semantics. New struct reuses the signed-tail layout for compatibility with existing `verifySignature()` call.

```c
enum DuelPhase : uint8_t {
    DP_INVITE,   // challenge to duel
    DP_ACCEPT,   // accept → both seed game
    DP_DECLINE,  // explicit decline (avoids hanging the challenger)
    DP_COMMIT,   // hash(round|move|nonce)
    DP_REVEAL,   // move + nonce
    DP_FORFEIT   // quit / timeout concession
};

enum DuelMove : uint8_t { MV_NONE=0, MV_SLASH=1, MV_SHIELD=2, MV_BLAST=3 };

struct DuelMsg {             // offset  size
    MsgType   type;          //   0      1   = MSG_DUEL
    DuelPhase phase;         //   1      1
    uint8_t   game_id;       //   2      1   random per-match (stale-frame rejection)
    uint8_t   round;         //   3      1   1..N
    char      name[16];      //   4     16   sender callsign
    uint8_t   mac[6];        //  20      6   sender MAC
    uint8_t   move;          //  26      1   DuelMove — valid only in DP_REVEAL
    uint8_t   my_hp;         //  27      1   sender HP (desync detection)
    uint8_t   op_hp;         //  28      1   opponent HP (desync detection)
    uint8_t   move_hash[32]; //  29     32   SHA-256(round|move|nonce) — DP_COMMIT
    uint8_t   nonce[16];     //  61     16   revealed in DP_REVEAL
    // ── signed boundary: bytes [0..76] = 77 bytes ──
    uint8_t   pubkey[64];    //  77     64   P-256 pubkey (all-zero = unsigned)
    uint8_t   sig[64];       // 141     64   ECDSA-P256 over first 77 bytes (if ATECC)
};                           // TOTAL = 205 bytes  (<250 ESPNow limit; 45 B headroom)
```

**LOCKED: 205-byte DuelMsg with 77-byte signed region, `MSG_DUEL` type.**

---

### 2.3 Commit-reveal anti-cheat

**Protocol per round:**
1. Player picks move. Generates `nonce[16]` via `esp_random()`. Computes `SHA-256(round || move || nonce)` → `move_hash[32]`.
2. Sends `DP_COMMIT` with `move_hash`. If ATECC available (`g_atecc_ok`), signs the 77-byte signed region (move_hash is inside it). Waits for opponent's commit.
3. Once both commits received, sends `DP_REVEAL` with `move` + `nonce`. On receive, recomputes `SHA-256(round||move||nonce)` and verifies it equals the opponent's committed `move_hash`. Mismatch → opponent forfeits the round.
4. HP resolution is computed **deterministically and locally** from the two revealed moves — HP values in the wire frame are used only for desync detection, not as authoritative state.

**Sign COMMIT only (not reveal):** The commit is the binding artifact. The reveal is verified by hash check — no signature needed. Signing the reveal instead would allow a cheater to observe the opponent's reveal and pick a winning counter-move before their own reveal. Signing both adds ~80 ms latency with no additional fairness benefit over signing-commit + hash-verify-reveal.

**If ATECC absent or unprovisioned:** Play in unsigned mode (pubkey/sig zeroed). Hash-based commit-reveal still prevents move-swapping — the signature only adds identity binding. Gate on `g_atecc_ok` exactly as beacon/CTF code (main.cpp:843).

**Timeouts:** 20 s per phase. Resend own last packet every 3 s (up to 6 retries per phase). On expiry: send `DP_FORFEIT`, declare local win.

**LOCKED: Sign COMMIT only (when ATECC available); SHA-256(round‖move‖nonce) binding; 20s phase timeout; 3s resend; unsigned-mode fallback.**

---

### 2.4 Game rules

| Option | Pros | Cons | Complexity |
|--------|------|------|------------|
| **3 attacks + 100 HP** | Legible RPS triangle; HP bars are the visual payoff | Slightly more UI than 2 attacks | Low-Med |
| 4 attacks (+HEAL) | More strategy | Stalling, more UI/edge cases | Medium |
| 3-round winner (no HP) | No HP tracking | Loses the HP-bar spectacle | Low |

**Damage table (simultaneous resolution):**

| You \ Opponent | SLASH | SHIELD | BLAST |
|---------------|-------|--------|-------|
| **SLASH** | Both take 30 | You take 10 (parried), opp 0 | You take 20, opp 30 |
| **SHIELD** | Parry: opp −10, you 0 | Nothing | Nothing (blocked) |
| **BLAST** | You 30, opp 20 | Both 0 (blast blocked) | Both take 20 |

- 100 HP each. First to ≤ 0 loses. Simultaneous lethal = draw.
- **No RSSI gating.** Pure RPS with badge flavor in the attack names.

**LOCKED: 3 attacks (SLASH/SHIELD/BLAST), 100 HP, fixed damage table, no RSSI gating.**

---

### 2.5 Game state machine

```
S_IDLE
  ├─ user picks peer + SELECT ──────────────────► send DP_INVITE ► S_INVITE_SENT
  └─ recv DP_INVITE ────────────────────────────► S_INVITE_RECV

S_INVITE_SENT
  ├─ recv DP_ACCEPT (matching game_id) ─────────► S_ROUND_PICK (round=1, game reset)
  ├─ recv DP_DECLINE / DP_FORFEIT ──────────────► S_IDLE ("declined")
  └─ timeout 15s ───────────────────────────────► S_IDLE ("no answer")

S_INVITE_RECV
  ├─ user SELECT (accept) ──────────────────────► send DP_ACCEPT ► S_ROUND_PICK
  ├─ user CANCEL / timeout 15s ─────────────────► send DP_DECLINE ► S_IDLE
  └─ mutual invite race: lower MAC badge is      host; higher-MAC auto-converts its
     outgoing invite to an accept ────────────────► S_ROUND_PICK

S_ROUND_PICK
  ├─ user L/R to select move, SELECT confirms ──► compute nonce+hash, sign (if ATECC_ok)
  │                                                send DP_COMMIT ► S_WAIT_COMMIT
  └─ user CANCEL ───────────────────────────────► send DP_FORFEIT ► S_IDLE

S_WAIT_COMMIT           (I committed; waiting for opponent's commit)
  ├─ recv DP_COMMIT (matching game_id, round) ──► send DP_REVEAL ► S_WAIT_REVEAL
  ├─ recv DP_FORFEIT ───────────────────────────► S_RESULT (win: opponent quit)
  ├─ resend DP_COMMIT every 3s (up to 6 retries)
  └─ timeout 20s ───────────────────────────────► send DP_FORFEIT ► S_RESULT (win: timeout)

S_WAIT_REVEAL           (both committed; I revealed; waiting for opponent's reveal)
  ├─ recv DP_REVEAL ──┐
  │    hash matches   ──────────────────────────► S_RESOLVE
  │    hash mismatch / bad sig ────────────────► S_RESULT (win: opponent cheated)
  ├─ recv DP_FORFEIT ───────────────────────────► S_RESULT (win)
  ├─ resend DP_REVEAL every 3s (up to 6 retries)
  └─ timeout 20s ───────────────────────────────► send DP_FORFEIT ► S_RESULT (win: timeout)

S_RESOLVE               (pure local; no network; sub-millisecond)
  ├─ apply damage table; update my_hp / op_hp
  ├─ my_hp ≤ 0 || op_hp ≤ 0 ──────────────────► S_RESULT
  └─ else round++ ──────────────────────────────► S_ROUND_PICK

S_RESULT
  ├─ persist W/L/D to NVS "duel"
  ├─ user SELECT (rematch) ─────────────────────► send DP_INVITE ► S_INVITE_SENT
  └─ user CANCEL ───────────────────────────────► S_IDLE

NOTE: g_state != S_IDLE → s_last_activity = millis() each loop tick → suppresses 60s sleep.
NOTE: if no valid DuelMsg from opponent for 25s from any active state → game watchdog fires → S_RESULT (win: disconnected).
```

**LOCKED: 8-state machine; deterministic local HP resolution; game_id+round in signed digest prevents replay; watchdog on 25s silence.**

---

### 2.6 UI layout (264×176 landscape, rotation 1)

**Battle screen (S_ROUND_PICK, S_WAIT_*):**
```
┌──────────────────────────────────────────────┐  y=0
│ DUEL    Round 2    [connected to CALIGULA]    │  FreeMonoBold9pt, y~14
├──────────────────────────────────────────────┤  HLine y=20
│ ME    AUGUSTUS                                │  FreeMono9pt, y~36
│ [████████████████░░░░]  72 HP                 │  fillRect bar x=8 w=220 h=10, y=42
│                                               │
│ OP    CALIGULA                                │  y~68
│ [████████░░░░░░░░░░░░]  40 HP                 │  bar y=74
├──────────────────────────────────────────────┤  HLine y=100
│   [SLASH]    SHIELD    BLAST                  │  y~118, L/R selects, inverted = selected
│                                               │
├──────────────────────────────────────────────┤  HLine y=150
│ L/R:pick  SEL:commit  CXL:forfeit             │  footer y~166, FreeMono9pt
└──────────────────────────────────────────────┘
```

**Waiting overlay (replaces move selector row):**
```
│   Sent ✓ — waiting (retry 2/3, forfeit in 14s) │  updated on each resend tick
```

**Result screen (full refresh):**
```
   YOU WIN                 (FreeMonoBold24pt, centred)
   K.O. round 4
   W: 12   L: 7   D: 1

   SEL: rematch   CXL: menu
```

**Invite screen (S_INVITE_RECV):**
```
│ DUEL CHALLENGE                               │
│                                              │
│ AUGUSTUS wants to duel!                      │
│ -65 dBm (near)                               │
│                                              │
│ SEL: accept   CXL: decline (10s timeout)     │
```

**Refresh strategy:**
- Move cursor move: `setPartialWindow(0, 100, 264, 60)` — selector row only (~200ms practical)
- HP change: `setPartialWindow(0, 20, 264, 90)` — HP bar rows only
- Full refresh every 10 partials (anti-ghosting, matches existing codebase)
- **No `hibernate()` between consecutive partial updates during active duel** — hibernate on state exit to IDLE only
- Round transition + result screen: full refresh

**LOCKED: Header/dual HP bars/inverted 3-item move selector; sub-region partial for selector and HP; full refresh on round change and result; skip hibernate during active duel.**

---

### 2.7 Reliability layer (net-new for this codebase)

The existing codebase has zero retransmit, never checks `esp_now_send()` return value, and has no send callback registered. This is a Critical gap for any multi-packet exchange.

```c
// Reliability manager per phase:
static DuelMsg   g_pending_send    = {};  // last sent frame
static bool      g_pending_ack     = false;
static uint32_t  g_pending_sent_at = 0;
static int       g_retry_count     = 0;
static const int MAX_RETRIES       = 6;
static const uint32_t RETRY_MS     = 3000;
static const uint32_t PHASE_TIMEOUT_MS = 20000;

// In main loop:
if (g_pending_ack) {
    uint32_t elapsed = millis() - g_pending_sent_at;
    if (elapsed > PHASE_TIMEOUT_MS || g_retry_count >= MAX_RETRIES) {
        // forfeit
    } else if (elapsed > (uint32_t)(g_retry_count + 1) * RETRY_MS) {
        esp_now_send(g_duel_peer_mac, (uint8_t*)&g_pending_send, sizeof(DuelMsg));
        g_retry_count++;
    }
}
```

Cleared on receiving the expected next-phase frame from opponent (implicit ACK by progression).

**LOCKED: App-level reliability: 3s resend, 6 retries max, 20s forfeit timeout; implicit ACK via state progression.**

---

### 2.8 Persistence

| What | Persist? | Where |
|------|----------|-------|
| W/L/D record | Yes | NVS namespace "duel", keys `wins`/`losses`/`draws` (uint32) |
| Callsign | Read-only from "badge"/"callsign"; fallback "DUELIST" | Shared NVS |
| ATECC key | Reuse main firmware's slot-0 key (same identity) | ATECC slot 0 |
| In-progress match | No | RAM-only |

**LOCKED: NVS "duel" namespace for W/L/D; reuse ATECC identity; no match state persistence.**

---

## Failure Modes & Mitigations (Loop 3)

| Failure Mode | Impact | Mitigation | Designed? |
|-------------|--------|------------|-----------|
| Commit/reveal message dropped | Both players stuck waiting | App-level retry (3s, 6x) | Yes — Phase 2 |
| Badge deep-sleeps mid-game | Opponent waits forever | Suppress sleep in non-IDLE states | Yes — Phase 3 |
| Opponent disconnects | Indefinite hang | 25s game watchdog → win by forfeit | Yes — Phase 3 |
| Simultaneous invites | Two parallel games, desync | Lower-MAC = canonical host | Yes — Phase 3 |
| ESP_ERR_ESPNOW_FULL | Challenge silently fails | Check return, evict LRU peer | Yes — Phase 5 |
| ATECC absent / unprovisioned | Block gameplay | Graceful unsigned-mode fallback | Yes — Phase 2 |
| Hash mismatch on reveal | No arbiter | Both badges independently forfeit cheating side | Yes — Phase 3 |
| RSSI gating frustration | Outside player control | CUT — no RSSI gating in v1 | Yes — Cut |
| Partial refresh ghosting | Visual noise accumulates | Full refresh every 10 partials | Yes — Phase 4 |
| Double-press SELECT | Accidental double-commit | g_move_committed flag; 150ms debounce after state transition | Yes — Phase 3 |

---

## Security (Loop 3)

- No server; peer-to-peer only — attack surface is ESPNow packet injection
- `game_id` (random per match) + `round` in signed digest: rejects replayed packets from prior matches/rounds
- `mac` in signed region: rejects impersonation (can't relay a sig from badge A as badge B)
- Hash-binding of move+nonce: move cannot be swapped after commit without detection
- ATECC signature (when available): identity proof binding the commit to the hardware key
- Unsigned mode: graceful degradation; no gameplay block
- PMK "NullCity-Badge-1" + LMK "Badge-LinkKey-01": same as existing firmware, maintains encrypted-unicast compatibility

---

## Cost Analysis (Loop 3)

| Category | Estimate |
|----------|---------|
| Development | 1–3 days solo (~16–18 focused hours) |
| Hardware cost | $0 additional — base badge only |
| Runtime cost | $0 — ESPNow, no cloud, no APIs |
| Flash footprint | ~200 KB estimate (similar to tamagotchi) |

---

## Decision Confidence (Loop 3)

| Decision | Confidence | Risk if Wrong | Reversibility |
|----------|-----------|---------------|---------------|
| Simultaneous commit-reveal | High | None — proven pattern | N/A |
| No RSSI gating | High | Players miss "range" flavor | Easy to add in v2 |
| App-level retry (3s/6x/20s) | Med | Tune timing if network noisier | Easy to tune |
| 100 HP fixed damage | High | Balance | Trivial to change |
| 205-byte DuelMsg | High | Fits 250B limit with margin | Additive |
| Skip hibernate during duel | High | Display re-init cost | Revert trivially |

---

## Gap Analysis (Loop 6)

| # | Requirement | Addressed In | Phase | Confidence |
|---|-------------|-------------|-------|------------|
| 1 | Challenge/accept over ESPNow | State machine S_INVITE_SENT/RECV | Phase 3 | High |
| 2 | Simultaneous move selection | Commit-reveal protocol | Phase 3 | High |
| 3 | Anti-cheat for moves | SHA-256 hash binding + optional ATECC sig | Phase 3 | High |
| 4 | E-paper battle display | UI design §2.6 | Phase 4 | High |
| 5 | HP-based game ending | S_RESOLVE deterministic | Phase 3 | High |
| 6 | 1–3 day scope | RSSI cut, ATECC optional, tamagotchi template | All | High |

**Gaps found and patched:** 7 (retransmit, sleep suppression, RSSI cut, simultaneous-invite race, peer table full, hibernate optimization, game watchdog). All patched in architecture. Presearch locked.

---

## Rejected Alternatives

| Decision | Rejected Option | Why Rejected |
|----------|----------------|--------------|
| Turn model | Alternating turns | Unfair — 2nd player knows 1st player's move; signing has no purpose |
| Turn model | Real-time | E-paper 500ms makes real-time physically impossible |
| RSSI gating | SLASH gated by RSSI | Unreliable hardware (stale -50 placeholder, ±10 dBm noise, outside player control) |
| Signing | Sign both commit AND reveal | Signing reveal adds ~80ms latency with no fairness gain over hash-verify |
| Signing | Sign reveal only | Trivially cheatable — commit isn't binding |
| Game rules | 4 attacks (+HEAL) | Stalling games, more UI/edge cases, marginal strategy gain |
| Persistence | Persist match state | Matches are ephemeral; power loss = abandon; adds complexity without value |
