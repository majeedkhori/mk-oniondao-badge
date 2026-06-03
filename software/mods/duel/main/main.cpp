// ESP-Duel Phase 3 — State Machine + Commit-Reveal Protocol
// OnionDAO Badge (ESP32-S3-WROOM-1-N8R8)
// Phases 1+2: scaffold, peripherals, ATECC, ESPNow, reliability manager
// Phase 3: complete state machine, peer discovery, commit-reveal, damage, NVS W/L/D

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Preferences.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <WiFi.h>
#include <mbedtls/sha256.h>
#include <GxEPD2_BW.h>
#include <gdey/GxEPD2_270_GDEY027T91.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeMonoBold24pt7b.h>
#include <Fonts/FreeMono9pt7b.h>
#include <SparkFun_ATECCX08a_Arduino_Library.h>
#include "badge_pins.h"

// ── TCA9534 (I2C button expander) ─────────────────────────────────────────────
#define TCA9534_ADDR   0x20
#define TCA9534_INPUT  0x00
#define TCA9534_CONFIG 0x03

// ── Button masks ──────────────────────────────────────────────────────────────
#define BTN_LEFT   (1 << 0)
#define BTN_DOWN   (1 << 1)
#define BTN_UP     (1 << 2)
#define BTN_RIGHT  (1 << 3)
#define BTN_SELECT (1 << 4)
#define BTN_CANCEL (1 << 5)

// ── ESP-NOW keys (shared across all OnionDAO badges) ──────────────────────────
// Must be uint8_t* to match esp_now_set_pmk() / LMK field types
static const uint8_t ESPNOW_PMK[] = "NullCity-Badge-1";  // 17 bytes; esp_now_set_pmk reads first 16
static const uint8_t ESPNOW_LMK[] = "Badge-LinkKey-01";  // 17 bytes; LMK field uses first 16

// ── Misc constants ────────────────────────────────────────────────────────────
#define SLEEP_AFTER_MS  60000
#define PUBLIC_KEY_SIZE 64
#define SIGNATURE_SIZE  64

// ── Display ───────────────────────────────────────────────────────────────────
GxEPD2_BW<GxEPD2_270_GDEY027T91, GxEPD2_270_GDEY027T91::HEIGHT> display(
    GxEPD2_270_GDEY027T91(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY));

// ── Message / game enums ──────────────────────────────────────────────────────
enum MsgType  : uint8_t { MSG_BEACON=0, MSG_PING, MSG_PONG, MSG_CTF, MSG_GAME, MSG_SCORE, MSG_DUEL=6 };
enum DuelPhase: uint8_t { DP_INVITE, DP_ACCEPT, DP_DECLINE, DP_COMMIT, DP_REVEAL, DP_FORFEIT };
enum DuelMove : uint8_t { MV_NONE=0, MV_SLASH=1, MV_SHIELD=2, MV_BLAST=3 };

// ── DuelMsg struct — MUST be exactly 205 bytes ────────────────────────────────
// Layout (byte offsets):
//   [0]      type       1
//   [1]      phase      1
//   [2]      game_id    1
//   [3]      round      1
//   [4..19]  name      16
//  [20..25]  mac        6
//  [26]      move       1
//  [27]      my_hp      1
//  [28]      op_hp      1
//  [29..60]  move_hash 32
//  [61..76]  nonce     16
//  --- signed boundary: bytes [0..76] = 77 bytes ---
//  [77..140] pubkey    64
// [141..204] sig       64
//  TOTAL = 205 bytes
struct __attribute__((packed)) DuelMsg {
    MsgType   type;           //   0      1
    DuelPhase phase;          //   1      1
    uint8_t   game_id;        //   2      1
    uint8_t   round;          //   3      1
    char      name[16];       //   4     16
    uint8_t   mac[6];         //  20      6
    uint8_t   move;           //  26      1
    uint8_t   my_hp;          //  27      1
    uint8_t   op_hp;          //  28      1
    uint8_t   move_hash[32];  //  29     32
    uint8_t   nonce[16];      //  61     16
    // signed boundary: bytes [0..76] = 77 bytes
    uint8_t   pubkey[64];     //  77     64
    uint8_t   sig[64];        // 141     64
};                            // TOTAL = 205 bytes
static_assert(sizeof(DuelMsg) == 205, "DuelMsg must be 205 bytes");

// ── ATECC globals ─────────────────────────────────────────────────────────────
static ATECCX08A g_atecc;
static bool      g_atecc_ok                     = false;
static uint8_t   g_atecc_pubkey[PUBLIC_KEY_SIZE] = {};

// ── ESPNow globals ────────────────────────────────────────────────────────────
static char    g_callsign[16] = {};
static bool    g_espnow_up     = false;
static uint8_t g_my_mac[6]     = {};   // factory MAC, read once via esp_read_mac()

// ── Reliability manager ───────────────────────────────────────────────────────
struct PendingSend {
    DuelMsg  msg;
    bool     active;
    uint32_t first_sent_at;
    uint32_t last_sent_at;
    int      retries;
    uint8_t  peer_mac[6];
};
static PendingSend g_pending_send  = {};
static portMUX_TYPE g_espnow_mux   = portMUX_INITIALIZER_UNLOCKED;

// ── Recv flags (set in on_recv, consumed in main loop) ───────────────────────
// RULE: never call esp_now_send() or ATECC ops from inside on_recv
static volatile bool g_duel_recv_flag   = false;
static DuelMsg       g_duel_recv_msg    = {};
static uint8_t       g_duel_recv_mac[6] = {};
static uint8_t       g_duel_recv_des[6] = {};   // destination addr (broadcast vs unicast)

// ── ATECC deferred verify ─────────────────────────────────────────────────────
static volatile bool g_needs_verify   = false;
static DuelMsg       g_pending_verify = {};
static uint8_t       g_verify_mac[6]  = {};
static bool          g_verify_result  = false;  // Phase 5: result of ATECC verify
static bool g_op_sig_verified = false;   // set true when opponent ECDSA verify passes

// ── State machine enums ───────────────────────────────────────────────────────
enum DuelState : uint8_t {
    S_IDLE,
    S_INVITE_SENT,
    S_INVITE_RECV,
    S_ROUND_PICK,
    S_WAIT_COMMIT,
    S_WAIT_REVEAL,
    S_RESOLVE,
    S_RESULT
};

enum ResultReason : uint8_t {
    RR_KO,
    RR_FORFEIT,
    RR_TIMEOUT,
    RR_CHEATED,
    RR_DISCONNECTED
};

enum ResultOutcome : uint8_t { RO_WIN, RO_LOSS, RO_DRAW };

// ── Game state ────────────────────────────────────────────────────────────────
static DuelState g_duel_state        = S_IDLE;
static uint8_t   g_current_game_id   = 0;
static uint8_t   g_current_round     = 1;
static uint8_t   g_my_hp             = 100;
static uint8_t   g_op_hp             = 100;
static uint8_t   g_opponent_mac[6]   = {};
static char      g_opponent_name[16] = {};
static bool      g_move_committed    = false;
static uint32_t  g_state_enter_ms    = 0;
static uint32_t  g_last_duel_recv_at = 0;
static uint32_t  s_last_activity     = 0;
static uint8_t   g_partial_count     = 0;

// ── Current round state ───────────────────────────────────────────────────────
static DuelMove g_selected_move    = MV_SLASH;
static DuelMove g_my_move          = MV_NONE;
static uint8_t  g_my_nonce[16]     = {};
static uint8_t  g_my_move_hash[32] = {};
static uint8_t  g_op_move_hash[32] = {};

// Order-independent commit-reveal bookkeeping for the current round. Commit and
// reveal frames may arrive in any order (e.g. opponent's commit lands while we're
// still picking our move), so we buffer whatever arrives and drive the round from
// these flags rather than gating on a strict S_WAIT_COMMIT/S_WAIT_REVEAL sequence.
static bool     g_op_committed       = false;  // opponent's commit hash stored
static bool     g_op_reveal_seen     = false;  // opponent's reveal frame buffered
static uint8_t  g_op_reveal_move     = MV_NONE;
static uint8_t  g_op_reveal_nonce[16]= {};
static bool     g_op_revealed        = false;  // opponent reveal verified + applied
static DuelMove g_op_move            = MV_NONE; // verified opponent move
static bool     g_my_revealed        = false;  // we have sent our reveal

// Cache of our most recently sent reveal. The reveal is the LAST frame of a round,
// so whoever resolves first stops retransmitting it — if that frame was lost the
// opponent is stranded. We replay this cached reveal on demand (when the opponent
// is still asking about that round) to let them resolve and catch up. This cache
// deliberately survives the round transition (NOT cleared by enter_round_pick).
static bool     g_last_reveal_valid     = false;
static uint8_t  g_last_reveal_round     = 0;
static DuelMove g_last_reveal_move      = MV_NONE;
static uint8_t  g_last_reveal_nonce[16] = {};
static uint32_t g_last_replay_ms        = 0;     // throttle for replays

// ── Result state ──────────────────────────────────────────────────────────────
static ResultOutcome g_result_outcome;
static ResultReason  g_result_reason;

// ── Render flags (Phase 4) ────────────────────────────────────────────────────
static bool     g_needs_redraw   = true;   // trigger a full redraw
static bool     g_needs_partial  = false;  // trigger partial refresh (move selector row)
static uint32_t g_last_render_ms = 0;
static bool     g_debug_hud      = false;  // UP+DOWN chord toggles an on-screen debug overlay

// ── Peer scan list ────────────────────────────────────────────────────────────
#define MAX_DUEL_PEERS 20
struct DuelPeer { char name[16]; uint8_t mac[6]; uint32_t last_seen; };
static DuelPeer g_duel_peers[MAX_DUEL_PEERS] = {};
static int      g_duel_peer_count             = 0;
static int      g_peer_cursor                 = 0;

// ── NVS W/L/D ────────────────────────────────────────────────────────────────
static uint32_t g_wins = 0, g_losses = 0, g_draws = 0;

// ── Broadcast MAC ─────────────────────────────────────────────────────────────
static uint8_t BROADCAST_MAC[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// ── Discovery / scan timer ────────────────────────────────────────────────────
static uint32_t g_last_scan_at = 0;

// ── Button state ──────────────────────────────────────────────────────────────
static uint8_t g_prev_btns = 0;

// ── Button ISR ────────────────────────────────────────────────────────────────
static volatile bool g_btn_irq = false;

static void IRAM_ATTR btn_isr() { g_btn_irq = true; }

// ─────────────────────────────────────────────────────────────────────────────
// ATECC
// ─────────────────────────────────────────────────────────────────────────────

static bool check_atecc() {
    if (!g_atecc.begin(0x60, Wire, Serial)) return false;
    g_atecc.wakeUp();
    g_atecc.readConfigZone(false);
    bool config_ok = g_atecc.configLockStatus;
    bool slot_ok   = g_atecc.slot0LockStatus;
    if (config_ok && slot_ok) {
        g_atecc.generatePublicKey(0, false);
        memcpy(g_atecc_pubkey, g_atecc.publicKey64Bytes, PUBLIC_KEY_SIZE);
        g_atecc.sleep();
        g_atecc_ok = true;
        Serial.println("[atecc] provisioned, pubkey loaded");
        return true;
    }
    g_atecc.sleep();
    Serial.printf("[atecc] unsigned mode (cfg=%d slot0=%d)\n", config_ok, slot_ok);
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// ESPNow callbacks
// ─────────────────────────────────────────────────────────────────────────────

// on_send: log failures only; retry is driven by reliability manager in main loop
// ESP-IDF v5.x signature: (const esp_now_send_info_t* tx_info, esp_now_send_status_t status)
static void on_send(const esp_now_send_info_t* tx_info, esp_now_send_status_t status) {
    if (status != ESP_NOW_SEND_SUCCESS && g_pending_send.active) {
        const uint8_t* mac = tx_info->des_addr;
        Serial.printf("[espnow] send failed to %02X:%02X:%02X:%02X:%02X:%02X\n",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
}

// on_recv: ONLY copy data + set flag; never call esp_now_send or ATECC here
static void on_recv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if ((size_t)len != sizeof(DuelMsg)) return;
    DuelMsg msg;
    memcpy(&msg, data, sizeof(DuelMsg));
    if (msg.type != MSG_DUEL) return;
    portENTER_CRITICAL_ISR(&g_espnow_mux);
    if (!g_duel_recv_flag) {
        memcpy(&g_duel_recv_msg, &msg, sizeof(DuelMsg));
        memcpy(g_duel_recv_mac, info->src_addr, 6);
        memcpy(g_duel_recv_des, info->des_addr, 6);
        g_duel_recv_flag = true;
    }
    portEXIT_CRITICAL_ISR(&g_espnow_mux);
}

// ─────────────────────────────────────────────────────────────────────────────
// Reliability manager
// ─────────────────────────────────────────────────────────────────────────────

static void send_reliable(const uint8_t* mac, const DuelMsg& msg) {
    portENTER_CRITICAL(&g_espnow_mux);
    memcpy(&g_pending_send.msg, &msg, sizeof(DuelMsg));
    memcpy(g_pending_send.peer_mac, mac, 6);
    g_pending_send.active        = true;
    g_pending_send.retries       = 0;
    g_pending_send.first_sent_at = millis();
    g_pending_send.last_sent_at  = millis();
    portEXIT_CRITICAL(&g_espnow_mux);
    esp_now_send(mac, (const uint8_t*)&msg, sizeof(DuelMsg));
}

static void clear_pending() {
    portENTER_CRITICAL(&g_espnow_mux);
    g_pending_send.active = false;
    portEXIT_CRITICAL(&g_espnow_mux);
}

// ─────────────────────────────────────────────────────────────────────────────
// Peer management
// ─────────────────────────────────────────────────────────────────────────────

// Add peer without encryption (invite phase — target may not have us registered yet)
static void add_duel_peer_unencrypted(const uint8_t* mac) {
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;
    peer.encrypt = false;
    esp_err_t err = esp_now_add_peer(&peer);
    if (err == ESP_ERR_ESPNOW_FULL) {
        // Evict the oldest non-broadcast peer and retry once
        // Walk g_duel_peers to find the oldest (earliest last_seen), delete it, retry
        uint32_t oldest_time = UINT32_MAX;
        int oldest_idx = -1;
        for (int i = 0; i < g_duel_peer_count; i++) {
            // Don't evict broadcast or the peer we're trying to add
            if (memcmp(g_duel_peers[i].mac, BROADCAST_MAC, 6) == 0) continue;
            if (memcmp(g_duel_peers[i].mac, mac, 6) == 0) continue;
            if (g_duel_peers[i].last_seen < oldest_time) {
                oldest_time = g_duel_peers[i].last_seen;
                oldest_idx  = i;
            }
        }
        if (oldest_idx >= 0) {
            esp_now_del_peer(g_duel_peers[oldest_idx].mac);
            Serial.printf("[espnow] evicted LRU peer %02X:%02X to make room\n",
                          g_duel_peers[oldest_idx].mac[4], g_duel_peers[oldest_idx].mac[5]);
            // Retry once
            err = esp_now_add_peer(&peer);
        }
        if (err != ESP_OK) {
            Serial.printf("[espnow] add_duel_peer still failed after evict: %d\n", err);
        }
    } else if (err != ESP_OK) {
        Serial.printf("[espnow] add_duel_peer failed: %d\n", err);
    }
}

// Ensure the opponent is a registered peer for the duration of the duel.
//
// NOTE: peers stay UNENCRYPTED for the whole duel. The previous design upgraded
// the peer to encrypted on accept, but that created a fatal asymmetry: the
// accepter flipped its own peer record to encrypted and sent the ACCEPT
// encrypted, while the inviter still had the accepter registered unencrypted —
// so the inviter could never decrypt the ACCEPT and sat forever on "Waiting...".
// Both sides must agree on encryption before either sends an encrypted frame,
// and there is no handshake point where that is true. Commit-reveal + optional
// ATECC signing already provide cheat-resistance, so plaintext is fine here.
static void ensure_duel_peer(const uint8_t* mac) {
    add_duel_peer_unencrypted(mac);
}

// Upsert into the duel peer scan list
static void upsert_duel_peer(const uint8_t* mac, const char* name) {
    for (int i = 0; i < g_duel_peer_count; i++) {
        if (memcmp(g_duel_peers[i].mac, mac, 6) == 0) {
            strncpy(g_duel_peers[i].name, name, 15);
            g_duel_peers[i].last_seen = millis();
            return;
        }
    }
    if (g_duel_peer_count < MAX_DUEL_PEERS) {
        memcpy(g_duel_peers[g_duel_peer_count].mac, mac, 6);
        strncpy(g_duel_peers[g_duel_peer_count].name, name, 15);
        g_duel_peers[g_duel_peer_count].last_seen = millis();
        g_duel_peer_count++;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ESPNow init
// ─────────────────────────────────────────────────────────────────────────────

// Curated word lists for generated callsigns (format: Adj-Noun-XXXX).
// Adjectives are <=5 chars and nouns <=4 chars, so the worst case
// "Hyper-Stag-XXXX" is exactly 15 chars and always fits the 16-byte name field.
static const char* const CS_ADJ[] = {
    "Neon", "Cyber", "Void", "Iron", "Gold", "Onyx", "Ruby", "Jade",
    "Volt", "Dark", "Pyro", "Nova", "Acid", "Mega", "Hyper", "Vapor"
};
static const char* const CS_NOUN[] = {
    "Fox", "Wolf", "Hawk", "Bear", "Lynx", "Crow", "Owl", "Bat",
    "Orca", "Puma", "Wasp", "Ram", "Stag", "Eel", "Moth", "Kite"
};
static const int CS_ADJ_N  = sizeof(CS_ADJ)  / sizeof(CS_ADJ[0]);
static const int CS_NOUN_N = sizeof(CS_NOUN) / sizeof(CS_NOUN[0]);

static void make_default_callsign(char* out, size_t len) {
    // Deterministic per-badge name derived from the factory MAC (g_my_mac):
    //   <Adj>-<Noun>-<XXXX>   e.g. "Dark-Bear-1308"
    // Word choice is hashed from the MAC's vendor/device bytes; the 4-hex suffix
    // is the last two MAC bytes (the NIC-unique part), so two badges effectively
    // never collide even if they happen to pick the same two words.
    // NB: uses g_my_mac (read via esp_read_mac), NOT WiFi.macAddress(), which
    // returns zeros if read before the WiFi driver starts.
    uint8_t ai = (uint8_t)(g_my_mac[2] + g_my_mac[4] * 3u);
    uint8_t ni = (uint8_t)(g_my_mac[1] + g_my_mac[3] + g_my_mac[5] * 3u);
    snprintf(out, len, "%s-%s-%02X%02X",
             CS_ADJ[ai % CS_ADJ_N], CS_NOUN[ni % CS_NOUN_N],
             g_my_mac[4], g_my_mac[5]);
}

static void init_espnow() {
    if (g_espnow_up) return;

    // Read the factory MAC straight from eFuse. This works without the WiFi driver
    // being started, so it is always valid (unlike WiFi.macAddress()).
    esp_read_mac(g_my_mac, ESP_MAC_WIFI_STA);

    // Load callsign from badge NVS; fall back to MAC-derived default.
    // NOTE: older firmware (with the WiFi.macAddress()-returns-zero bug) persisted
    // the poisoned string "NCB-000000" into NVS, so every badge shows the same name
    // even after the MAC read is fixed. Treat that exact value as "unset" and
    // regenerate a unique name from the real MAC.
    {
        Preferences prefs;
        prefs.begin("badge", true);
        String cs = prefs.getString("callsign", "");
        prefs.end();
        if (cs.length() == 0 || cs == "NCB-000000") {
            make_default_callsign(g_callsign, sizeof(g_callsign));
        } else {
            strncpy(g_callsign, cs.c_str(), sizeof(g_callsign));
            g_callsign[sizeof(g_callsign) - 1] = '\0';
        }
    }

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    if (esp_now_init() != ESP_OK) {
        Serial.println("[espnow] init failed");
        return;
    }
    esp_now_register_recv_cb(on_recv);
    esp_now_register_send_cb(on_send);
    esp_now_set_pmk(ESPNOW_PMK);

    // Broadcast peer — always unencrypted (ESP-NOW spec)
    esp_now_peer_info_t peer = {};
    memset(peer.peer_addr, 0xFF, 6);
    peer.channel = 0;
    peer.encrypt = false;
    esp_now_add_peer(&peer);

    g_espnow_up = true;
    Serial.printf("[espnow] ready as %s  mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  g_callsign, g_my_mac[0], g_my_mac[1], g_my_mac[2],
                  g_my_mac[3], g_my_mac[4], g_my_mac[5]);
}

// ─────────────────────────────────────────────────────────────────────────────
// Button reading
// ─────────────────────────────────────────────────────────────────────────────

static uint8_t read_buttons_raw() {
    Wire.beginTransmission(TCA9534_ADDR);
    Wire.write(TCA9534_INPUT);
    Wire.endTransmission(false);
    Wire.requestFrom(TCA9534_ADDR, (uint8_t)1);
    if (!Wire.available()) return 0;
    uint8_t raw = Wire.read();
    return (~raw) & 0x3F;   // invert active-LOW, mask 6 buttons
}

// Debounced read: sample three times and only keep bits that are stable across
// ALL samples. The TCA9534/I2C bus on some badges glitches (boot logs show
// intermittent i2cRead errors), and a single corrupt sample can set a phantom
// bit (e.g. CANCEL) that forfeits a match the instant it starts. Requiring
// agreement across samples filters those transients without adding noticeable lag.
static uint8_t read_buttons() {
    uint8_t a = read_buttons_raw();
    delayMicroseconds(600);
    uint8_t b = read_buttons_raw();
    delayMicroseconds(600);
    uint8_t c = read_buttons_raw();
    return a & b & c;
}

// ─────────────────────────────────────────────────────────────────────────────
// Peripheral init
// ─────────────────────────────────────────────────────────────────────────────

static void init_peripherals() {
    pinMode(PIN_PWR,   OUTPUT); digitalWrite(PIN_PWR,   HIGH);
    pinMode(PIN_SE_EN, OUTPUT); digitalWrite(PIN_SE_EN, HIGH);

    Wire.begin(PIN_SDA, PIN_SCL);

    // Configure TCA9534: all 8 pins as inputs
    Wire.beginTransmission(TCA9534_ADDR);
    Wire.write(TCA9534_CONFIG);
    Wire.write(0xFF);
    Wire.endTransmission();

    SPI.begin(PIN_EPD_SCK, -1, PIN_EPD_MOSI, PIN_EPD_CS);
    display.init(115200, true, 10, false);
    display.setRotation(1);

    // Button interrupt on falling edge (active-LOW buttons via TCA9534 INT)
    pinMode(PIN_BTN_IRQ, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_BTN_IRQ), btn_isr, FALLING);
}

// ─────────────────────────────────────────────────────────────────────────────
// NVS W/L/D
// ─────────────────────────────────────────────────────────────────────────────

static void load_record() {
    Preferences p; p.begin("duel", true);
    g_wins   = p.getUInt("wins",   0);
    g_losses = p.getUInt("losses", 0);
    g_draws  = p.getUInt("draws",  0);
    p.end();
}

static void save_record() {
    Preferences p; p.begin("duel", false);
    p.putUInt("wins",   g_wins);
    p.putUInt("losses", g_losses);
    p.putUInt("draws",  g_draws);
    p.end();
}

// ─────────────────────────────────────────────────────────────────────────────
// Crypto helpers
// ─────────────────────────────────────────────────────────────────────────────

static void compute_move_hash(uint8_t round, uint8_t move,
                              const uint8_t* nonce, uint8_t* out_hash) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, &round, 1);
    mbedtls_sha256_update(&ctx, &move, 1);
    mbedtls_sha256_update(&ctx, nonce, 16);
    mbedtls_sha256_finish(&ctx, out_hash);
    mbedtls_sha256_free(&ctx);
}

// ─────────────────────────────────────────────────────────────────────────────
// Damage table — LOCKED from presearch §2.4
// ─────────────────────────────────────────────────────────────────────────────

struct DmgPair { uint8_t to_me; uint8_t to_op; };
static const DmgPair DAMAGE[4][4] = {
    // MV_NONE row
    {{0,0},{0,0},{0,0},{0,0}},
    // MV_SLASH row
    {{0,0},{30,30},{10,0},{20,30}},
    // MV_SHIELD row
    {{0,0},{0,10},{0,0},{0,0}},
    // MV_BLAST row
    {{0,0},{30,20},{0,0},{20,20}},
};

static void resolve_round(DuelMove my_move, DuelMove op_move,
                          uint8_t* my_hp, uint8_t* op_hp) {
    DmgPair d = DAMAGE[my_move][op_move];
    *my_hp = (*my_hp > d.to_me) ? (*my_hp - d.to_me) : 0;
    *op_hp = (*op_hp > d.to_op) ? (*op_hp - d.to_op) : 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Message builder helper
// ─────────────────────────────────────────────────────────────────────────────

static DuelMsg build_msg(DuelPhase phase) {
    DuelMsg m = {};
    m.type    = MSG_DUEL;
    m.phase   = phase;
    m.game_id = g_current_game_id;
    m.round   = g_current_round;
    strncpy(m.name, g_callsign, sizeof(m.name));
    memcpy(m.mac, g_my_mac, 6);
    m.my_hp   = g_my_hp;
    m.op_hp   = g_op_hp;
    return m;
}

// ─────────────────────────────────────────────────────────────────────────────
// Draw helpers (Phase 4)
// ─────────────────────────────────────────────────────────────────────────────

static void draw_hline(int y) {
    display.drawFastHLine(8, y, display.width() - 16, GxEPD_BLACK);
}

static void page_header(const char* title) {
    display.fillScreen(GxEPD_WHITE);
    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(8, 18);
    display.print(title);
    draw_hline(22);
}

static void page_footer(const char* text) {
    draw_hline(158);
    display.setFont(&FreeMono9pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(8, 174);
    display.print(text);
}

// Draws one HP bar.
// label_y: baseline of the label text
// bar_y: top-left y of the bar rect
static void draw_hp_bar(const char* label, uint8_t hp, int label_y, int bar_y) {
    display.setFont(&FreeMono9pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(8, label_y);
    display.print(label);

    // Outline
    display.drawRect(8, bar_y, 220, 10, GxEPD_BLACK);
    // Fill proportional
    int fill = (int)((hp * 220) / 100);
    if (fill > 0) display.fillRect(8, bar_y, fill, 10, GxEPD_BLACK);
}

// ── Screen 1: Idle / peer list ────────────────────────────────────────────────

static void draw_idle() {
    display.setFullWindow();
    display.firstPage();
    do {
        page_header("ESP-DUEL");
        display.setFont(&FreeMono9pt7b);
        display.setTextColor(GxEPD_BLACK);
        // Self-identity + discovery status (debug aid — confirms unique callsign
        // and that peers are being heard, without needing a serial cable).
        display.setCursor(140, 18);
        display.printf("%s", g_callsign);
        if (g_duel_peer_count == 0) {
            display.setCursor(8, 50);
            display.print("No badges nearby.");
            display.setCursor(8, 68);
            display.print("Scanning...");
        } else {
            // Show up to 5 peers; highlight cursor
            for (int i = 0; i < g_duel_peer_count && i < 5; i++) {
                int y = 42 + i * 18;
                if (i == g_peer_cursor) {
                    display.fillRect(6, y - 13, 252, 16, GxEPD_BLACK);
                    display.setTextColor(GxEPD_WHITE);
                } else {
                    display.setTextColor(GxEPD_BLACK);
                }
                display.setCursor(8, y);
                display.printf("> %.20s", g_duel_peers[i].name);
                display.setTextColor(GxEPD_BLACK);
            }
        }
        page_footer("SEL:challenge CXL:back");
    } while (display.nextPage());
    display.hibernate();
    g_partial_count = 0;
}

// ── Screen 2: Incoming challenge ──────────────────────────────────────────────

static void draw_invite_recv() {
    display.setFullWindow();
    display.firstPage();
    do {
        page_header("DUEL CHALLENGE");
        display.setFont(&FreeMono9pt7b);
        display.setTextColor(GxEPD_BLACK);
        display.setCursor(8, 50);
        display.printf("%.15s", g_opponent_name);
        display.setCursor(8, 68);
        display.print("wants to duel!");

        // Countdown
        uint32_t elapsed = millis() - g_state_enter_ms;
        int secs_left = (int)((15000 - elapsed) / 1000);
        if (secs_left < 0) secs_left = 0;
        char footer_buf[40];
        snprintf(footer_buf, sizeof(footer_buf), "SEL:accept CXL:decline (%ds)", secs_left);
        page_footer(footer_buf);
    } while (display.nextPage());
    g_partial_count = 0;
}

// ── Screen 3: Round pick ──────────────────────────────────────────────────────

static void draw_round_pick(bool full_refresh) {
    if (full_refresh || g_partial_count >= 10) {
        display.setFullWindow();
        g_partial_count = 0;
    } else {
        display.setPartialWindow(0, 0, 264, 176);
    }

    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);

        // Header
        char hdr[32];
        snprintf(hdr, sizeof(hdr), "DUEL Rd%d %.8s", g_current_round, g_opponent_name);
        display.setFont(&FreeMonoBold9pt7b);
        display.setTextColor(GxEPD_BLACK);
        display.setCursor(8, 18);
        display.print(hdr);
        draw_hline(22);

        // HP bars
        char me_label[20], op_label[20];
        snprintf(me_label, sizeof(me_label), "ME %.10s", g_callsign);
        snprintf(op_label, sizeof(op_label), "OP %.10s", g_opponent_name);
        draw_hp_bar(me_label, g_my_hp, 38, 42);
        draw_hp_bar(op_label, g_op_hp, 62, 66);

        draw_hline(80);

        // Move selector row (3 boxes side-by-side)
        const char* move_labels[] = {"SLASH", "SHIELD", "BLAST"};
        DuelMove moves[] = {MV_SLASH, MV_SHIELD, MV_BLAST};
        int box_x[] = {6, 92, 178};
        int box_w   = 80;
        int box_y   = 84;
        int box_h   = 56;

        for (int i = 0; i < 3; i++) {
            bool selected = (moves[i] == g_selected_move);
            if (selected) {
                display.fillRect(box_x[i], box_y, box_w, box_h, GxEPD_BLACK);
                display.setTextColor(GxEPD_WHITE);
            } else {
                display.drawRect(box_x[i], box_y, box_w, box_h, GxEPD_BLACK);
                display.setTextColor(GxEPD_BLACK);
            }
            display.setFont(&FreeMonoBold9pt7b);
            int label_len = strlen(move_labels[i]);
            int text_x = box_x[i] + (box_w - label_len * 11) / 2;
            if (text_x < box_x[i] + 2) text_x = box_x[i] + 2;
            display.setCursor(text_x, box_y + 22);
            display.print(move_labels[i]);
            display.setTextColor(GxEPD_BLACK);
        }

        // Footer
        page_footer("L/R:pick SEL:lock CXL:quit");
    } while (display.nextPage());

    if (full_refresh) g_partial_count = 0;
    else g_partial_count++;
}

// Partial selector-row update only (called when move selection changes)
static void draw_selector_partial() {
    if (g_partial_count >= 10) {
        draw_round_pick(true);
        return;
    }
    display.setPartialWindow(0, 82, 264, 76);
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        const char* move_labels[] = {"SLASH", "SHIELD", "BLAST"};
        DuelMove moves[] = {MV_SLASH, MV_SHIELD, MV_BLAST};
        int box_x[] = {6, 92, 178};
        int box_w = 80, box_y = 84, box_h = 56;
        for (int i = 0; i < 3; i++) {
            bool selected = (moves[i] == g_selected_move);
            if (selected) {
                display.fillRect(box_x[i], box_y, box_w, box_h, GxEPD_BLACK);
                display.setTextColor(GxEPD_WHITE);
            } else {
                display.drawRect(box_x[i], box_y, box_w, box_h, GxEPD_BLACK);
                display.setTextColor(GxEPD_BLACK);
            }
            display.setFont(&FreeMonoBold9pt7b);
            int label_len = strlen(move_labels[i]);
            int text_x = box_x[i] + (box_w - label_len * 11) / 2;
            if (text_x < box_x[i] + 2) text_x = box_x[i] + 2;
            display.setCursor(text_x, box_y + 22);
            display.print(move_labels[i]);
            display.setTextColor(GxEPD_BLACK);
        }
        draw_hline(80);
    } while (display.nextPage());
    g_partial_count++;
}

// ── Screen 4: Waiting (S_WAIT_COMMIT / S_WAIT_REVEAL) ────────────────────────

static void draw_waiting() {
    if (g_partial_count >= 10) {
        g_partial_count = 0;
    }
    display.setPartialWindow(0, 82, 264, 76);
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        draw_hline(82);
        display.setFont(&FreeMono9pt7b);
        display.setTextColor(GxEPD_BLACK);
        display.setCursor(8, 106);
        display.print("Sent - waiting");

        uint32_t elapsed = millis() - g_pending_send.first_sent_at;
        int secs_left = (int)((20000 - elapsed) / 1000);
        if (secs_left < 0) secs_left = 0;
        display.setCursor(8, 126);
        display.printf("retry %d/6, %ds left", g_pending_send.retries, secs_left);

        const char* phase_str = (g_duel_state == S_WAIT_COMMIT) ? "wait:commit" : "wait:reveal";
        display.setCursor(8, 148);
        display.print(phase_str);
    } while (display.nextPage());
    g_partial_count++;
}

// ── Screen 5: Result (WIN / LOSE / DRAW) ─────────────────────────────────────

static void draw_result() {
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);

        // Headline centred
        const char* headline = (g_result_outcome == RO_WIN)  ? "YOU WIN"  :
                               (g_result_outcome == RO_LOSS) ? "YOU LOSE" : "DRAW";
        display.setFont(&FreeMonoBold24pt7b);
        display.setTextColor(GxEPD_BLACK);
        // Rough centering: each char ~22px wide in 24pt bold
        int headline_x = (264 - (int)strlen(headline) * 22) / 2;
        if (headline_x < 4) headline_x = 4;
        display.setCursor(headline_x, 56);
        display.print(headline);

        draw_hline(64);

        // Reason line
        display.setFont(&FreeMono9pt7b);
        display.setCursor(8, 84);
        switch (g_result_reason) {
        case RR_KO:
            display.printf("K.O. round %d", g_current_round);
            break;
        case RR_FORFEIT:
            display.print("Opponent forfeited");
            break;
        case RR_TIMEOUT:
            display.print("Timeout");
            break;
        case RR_CHEATED:
            display.print("Opponent cheated");
            break;
        case RR_DISCONNECTED:
            display.print("Disconnected");
            break;
        }

        // Record line
        display.setCursor(8, 104);
        display.printf("W:%lu L:%lu D:%lu", g_wins, g_losses, g_draws);

        // ATECC indicator
        display.setCursor(8, 124);
        bool both_signed = g_atecc_ok && g_op_sig_verified;
        display.print(both_signed ? "[verified]" : "[unsigned]");

        draw_hline(136);

        // Footer
        display.setCursor(8, 154);
        display.print("SEL:rematch CXL:menu");
    } while (display.nextPage());
    display.hibernate();
    g_partial_count = 0;
}

// ── Debug HUD (toggled by UP+DOWN chord) ──────────────────────────────────────

static const char* state_name(DuelState s) {
    switch (s) {
    case S_IDLE:        return "IDLE";
    case S_INVITE_SENT: return "INV_SENT";
    case S_INVITE_RECV: return "INV_RECV";
    case S_ROUND_PICK:  return "ROUND_PICK";
    case S_WAIT_COMMIT: return "WAIT_COMMIT";
    case S_WAIT_REVEAL: return "WAIT_REVEAL";
    case S_RESOLVE:     return "RESOLVE";
    case S_RESULT:      return "RESULT";
    default:            return "?";
    }
}

static void draw_debug_hud() {
    // Periodic full refresh to clear e-paper ghosting; partial otherwise.
    if (g_partial_count >= 10) { display.setFullWindow(); g_partial_count = 0; }
    else                       { display.setPartialWindow(0, 0, 264, 176); }

    uint32_t now = millis();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setFont(&FreeMonoBold9pt7b);
        display.setTextColor(GxEPD_BLACK);
        display.setCursor(8, 14);
        display.print("== DEBUG HUD ==");
        draw_hline(18);

        display.setFont(&FreeMono9pt7b);
        char line[40];
        int  y = 34;

        snprintf(line, sizeof(line), "state: %s", state_name(g_duel_state));
        display.setCursor(8, y); display.print(line); y += 14;

        snprintf(line, sizeof(line), "me: %.11s", g_callsign);
        display.setCursor(8, y); display.print(line); y += 14;

        snprintf(line, sizeof(line), "mac %02X:%02X:%02X:%02X:%02X:%02X",
                 g_my_mac[0], g_my_mac[1], g_my_mac[2],
                 g_my_mac[3], g_my_mac[4], g_my_mac[5]);
        display.setCursor(8, y); display.print(line); y += 14;

        snprintf(line, sizeof(line), "peers:%d atecc:%s rec %lu/%lu/%lu",
                 g_duel_peer_count, g_atecc_ok ? "Y" : "N", g_wins, g_losses, g_draws);
        display.setCursor(8, y); display.print(line); y += 14;

        if (g_duel_state != S_IDLE) {
            snprintf(line, sizeof(line), "opp:%.8s %02X:%02X",
                     g_opponent_name, g_opponent_mac[4], g_opponent_mac[5]);
            display.setCursor(8, y); display.print(line); y += 14;
            snprintf(line, sizeof(line), "gid:%u rd:%u hp %u/%u",
                     g_current_game_id, g_current_round, g_my_hp, g_op_hp);
            display.setCursor(8, y); display.print(line); y += 14;
        }

        if (g_pending_send.active)
            snprintf(line, sizeof(line), "tx: ACTIVE retry %d/6", g_pending_send.retries);
        else
            snprintf(line, sizeof(line), "tx: idle");
        display.setCursor(8, y); display.print(line); y += 14;

        if (g_last_duel_recv_at > 0)
            snprintf(line, sizeof(line), "last rx: %lus ago", (now - g_last_duel_recv_at) / 1000);
        else
            snprintf(line, sizeof(line), "last rx: never");
        display.setCursor(8, y); display.print(line); y += 14;

        page_footer("UP+DOWN: close HUD");
    } while (display.nextPage());

    g_partial_count++;
}

// ── Rendering dispatcher ──────────────────────────────────────────────────────

static void render() {
    // Debug HUD takes over the screen entirely while active.
    if (g_debug_hud) {
        if (!g_needs_redraw && !g_needs_partial) return;
        g_needs_redraw  = false;
        g_needs_partial = false;
        draw_debug_hud();
        return;
    }

    if (!g_needs_redraw && !g_needs_partial) return;

    if (g_needs_partial && !g_needs_redraw) {
        if (g_duel_state == S_ROUND_PICK && !g_move_committed) {
            draw_selector_partial();
        } else if (g_duel_state == S_WAIT_COMMIT || g_duel_state == S_WAIT_REVEAL) {
            draw_waiting();
        }
        g_needs_partial = false;
        return;
    }

    g_needs_redraw  = false;
    g_needs_partial = false;

    switch (g_duel_state) {
    case S_IDLE:
        draw_idle();
        break;
    case S_INVITE_SENT:
        display.setFullWindow();
        display.firstPage();
        do {
            page_header("INVITE SENT");
            display.setFont(&FreeMono9pt7b);
            display.setTextColor(GxEPD_BLACK);
            display.setCursor(8, 50);
            display.printf("Challenged %.14s", g_opponent_name);
            display.setCursor(8, 68);
            display.print("Waiting...");
            page_footer("CXL:cancel");
        } while (display.nextPage());
        g_partial_count = 0;
        break;
    case S_INVITE_RECV:
        draw_invite_recv();
        break;
    case S_ROUND_PICK:
        draw_round_pick(true);
        break;
    case S_WAIT_COMMIT:
    case S_WAIT_REVEAL:
        draw_waiting();
        break;
    case S_RESULT:
        draw_result();
        break;
    default: break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// State transition helpers
// ─────────────────────────────────────────────────────────────────────────────

static void enter_idle() {
    g_duel_state = S_IDLE;
    g_current_game_id = 0;
    g_current_round = 1;
    g_my_hp = 100; g_op_hp = 100;
    g_move_committed = false;
    g_my_move = MV_NONE;
    g_selected_move = MV_SLASH;
    memset(g_opponent_mac, 0, 6);
    memset(g_opponent_name, 0, sizeof(g_opponent_name));
    g_last_duel_recv_at = 0;
    g_op_sig_verified = false;
    g_last_reveal_valid = false;
    clear_pending();
    g_needs_redraw = true;
    // Display hibernate when returning to idle — draw_idle() will re-hibernate after full refresh
    display.hibernate();
    Serial.println("[duel] → S_IDLE");
}

static void enter_round_pick() {
    g_duel_state = S_ROUND_PICK;
    g_state_enter_ms = millis();
    g_move_committed = false;
    g_my_move = MV_NONE;
    g_selected_move = MV_SLASH;
    memset(g_my_nonce, 0, sizeof(g_my_nonce));
    memset(g_my_move_hash, 0, sizeof(g_my_move_hash));
    memset(g_op_move_hash, 0, sizeof(g_op_move_hash));
    // Reset order-independent commit-reveal bookkeeping for the new round
    g_op_committed    = false;
    g_op_reveal_seen  = false;
    g_op_reveal_move  = MV_NONE;
    memset(g_op_reveal_nonce, 0, sizeof(g_op_reveal_nonce));
    g_op_revealed     = false;
    g_op_move         = MV_NONE;
    g_my_revealed     = false;
    // Fresh game (round 1) — drop any cached reveal from a previous match so we
    // never replay a stale reveal into a new game (e.g. on rematch).
    if (g_current_round == 1) g_last_reveal_valid = false;
    g_needs_redraw = true;
    Serial.printf("[duel] → S_ROUND_PICK round=%d my_hp=%d op_hp=%d\n",
                  g_current_round, g_my_hp, g_op_hp);
}

static void enter_result(ResultOutcome outcome, ResultReason reason) {
    g_result_outcome = outcome;
    g_result_reason  = reason;
    g_duel_state     = S_RESULT;
    g_needs_redraw   = true;
    clear_pending();
    // Update W/L/D
    if (outcome == RO_WIN)        g_wins++;
    else if (outcome == RO_LOSS)  g_losses++;
    else                          g_draws++;
    save_record();
    Serial.printf("[duel] → S_RESULT outcome=%d reason=%d W:%lu L:%lu D:%lu\n",
                  outcome, reason, g_wins, g_losses, g_draws);
}

// ─────────────────────────────────────────────────────────────────────────────
// Forfeit timeout
// ─────────────────────────────────────────────────────────────────────────────

static void on_forfeit_timeout() {
    Serial.println("[duel] forfeit timeout → WIN");
    enter_result(RO_WIN, RR_TIMEOUT);
}

// ─────────────────────────────────────────────────────────────────────────────
// Round protocol driver — order-independent commit/reveal/resolve
// Call this whenever round state changes (we commit, or a commit/reveal arrives).
// It advances as far as the currently-available information allows:
//   1. both committed       → send our reveal
//   2. opponent reveal+commit→ verify & apply their move
//   3. both revealed         → resolve the round
// ─────────────────────────────────────────────────────────────────────────────

static void advance_round_protocol() {
    // 1. Both sides have committed and we haven't revealed yet → send our reveal.
    if (g_move_committed && g_op_committed && !g_my_revealed) {
        DuelMsg rev = build_msg(DP_REVEAL);
        rev.move = (uint8_t)g_my_move;
        memcpy(rev.nonce, g_my_nonce, 16);
        send_reliable(g_opponent_mac, rev);   // overwrites the pending COMMIT
        g_my_revealed    = true;
        g_duel_state     = S_WAIT_REVEAL;
        g_state_enter_ms = millis();
        g_needs_redraw   = true;
        // Cache for on-demand replay if our reveal to the opponent gets lost.
        g_last_reveal_valid = true;
        g_last_reveal_round = g_current_round;
        g_last_reveal_move  = g_my_move;
        memcpy(g_last_reveal_nonce, g_my_nonce, 16);
        Serial.println("[duel] both committed -> sent reveal -> S_WAIT_REVEAL");
    }

    // 2. We have the opponent's commit hash AND their reveal frame → verify+apply.
    if (g_op_committed && g_op_reveal_seen && !g_op_revealed) {
        uint8_t check_hash[32];
        compute_move_hash(g_current_round, g_op_reveal_move, g_op_reveal_nonce, check_hash);
        if (memcmp(check_hash, g_op_move_hash, 32) != 0) {
            Serial.println("[duel] hash mismatch -> opponent cheated!");
            clear_pending();
            enter_result(RO_WIN, RR_CHEATED);
            return;
        }
        g_op_move     = (DuelMove)g_op_reveal_move;
        g_op_revealed = true;
        Serial.printf("[duel] opponent reveal verified move=%d\n", g_op_move);
    }

    // 3. Both revealed → resolve the round.
    if (g_my_revealed && g_op_revealed) {
        clear_pending();
        resolve_round(g_my_move, g_op_move, &g_my_hp, &g_op_hp);
        Serial.printf("[duel] resolved my=%d op=%d -> hp %d/%d\n",
                      g_my_move, g_op_move, g_my_hp, g_op_hp);
        if (g_my_hp == 0 && g_op_hp == 0)      enter_result(RO_DRAW, RR_KO);
        else if (g_my_hp == 0)                 enter_result(RO_LOSS, RR_KO);
        else if (g_op_hp == 0)                 enter_result(RO_WIN,  RR_KO);
        else { g_current_round++; enter_round_pick(); }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Commit move (called from S_ROUND_PICK on SELECT)
// ─────────────────────────────────────────────────────────────────────────────

static void commit_move() {
    g_my_move = g_selected_move;
    esp_fill_random(g_my_nonce, sizeof(g_my_nonce));
    compute_move_hash(g_current_round, (uint8_t)g_my_move, g_my_nonce, g_my_move_hash);

    DuelMsg m = build_msg(DP_COMMIT);
    memcpy(m.move_hash, g_my_move_hash, 32);

    // ATECC sign if available (sign bytes [0..76], the 77-byte signed region)
    if (g_atecc_ok) {
        uint8_t digest[32];
        mbedtls_sha256_context ctx;
        mbedtls_sha256_init(&ctx);
        mbedtls_sha256_starts(&ctx, 0);
        mbedtls_sha256_update(&ctx, (const uint8_t*)&m, 77);
        mbedtls_sha256_finish(&ctx, digest);
        mbedtls_sha256_free(&ctx);

        g_atecc.wakeUp();
        if (g_atecc.createSignature(digest, 0)) {
            memcpy(m.pubkey, g_atecc_pubkey, PUBLIC_KEY_SIZE);
            memcpy(m.sig,    g_atecc.signature, SIGNATURE_SIZE);
        }
        g_atecc.sleep();
    }

    send_reliable(g_opponent_mac, m);
    g_duel_state     = S_WAIT_COMMIT;
    g_state_enter_ms = millis();
    g_move_committed = true;
    Serial.printf("[duel] committed move=%d → S_WAIT_COMMIT\n", g_my_move);
    // The opponent may have already committed (their COMMIT arrived while we were
    // still picking) — advance immediately so we don't wait for a frame that has
    // already come and gone.
    advance_round_protocol();
}

// ─────────────────────────────────────────────────────────────────────────────
// Message dispatcher (called from loop() when a DuelMsg is ready)
// ─────────────────────────────────────────────────────────────────────────────

static void dispatch_recv(const DuelMsg& msg, const uint8_t* src, const uint8_t* des) {
    bool is_broadcast = (memcmp(des, BROADCAST_MAC, 6) == 0);

    // DP_INVITE is special: broadcast = discovery, unicast = actual challenge
    if (msg.phase == DP_INVITE) {
        if (is_broadcast) {
            // Discovery beacon from another badge
            upsert_duel_peer(src, msg.name);
            Serial.printf("[duel] peer discovered: %.15s\n", msg.name);
            return;
        }
        // Unicast invite = actual challenge.
        // Accept invites from S_IDLE and S_RESULT (so a rematch invite lands even
        // while the other player is still looking at the WIN/LOSE/DRAW screen).
        if (g_duel_state == S_IDLE || g_duel_state == S_RESULT) {
            memcpy(g_opponent_mac, src, 6);
            strncpy(g_opponent_name, msg.name, sizeof(g_opponent_name)-1);
            g_current_game_id = msg.game_id;
            g_my_hp = 100; g_op_hp = 100;
            g_current_round = 1;
            g_op_sig_verified = false;
            g_duel_state = S_INVITE_RECV;
            g_state_enter_ms = millis();
            g_needs_redraw = true;
            upsert_duel_peer(src, msg.name);
            add_duel_peer_unencrypted(src);
            Serial.printf("[duel] challenged by %.15s → S_INVITE_RECV\n", msg.name);
            return;
        }
        if (g_duel_state == S_INVITE_SENT) {
            // Mutual invite: lower-MAC badge accepts; higher-MAC converts
            if (memcmp(g_my_mac, src, 6) < 0) {
                // I am lower-MAC → accept their invite
                Serial.println("[duel] mutual invite: I am lower-MAC, accepting");
                g_current_game_id = msg.game_id;
                strncpy(g_opponent_name, msg.name, sizeof(g_opponent_name)-1);
                clear_pending();
                DuelMsg acc = build_msg(DP_ACCEPT);
                send_reliable(g_opponent_mac, acc);
                g_my_hp = 100; g_op_hp = 100;
                g_current_round = 1;
                enter_round_pick();
                ensure_duel_peer(g_opponent_mac);
            } else {
                // I am higher-MAC → convert to INVITE_RECV
                Serial.println("[duel] mutual invite: I am higher-MAC, converting");
                clear_pending();
                g_current_game_id = msg.game_id;
                strncpy(g_opponent_name, msg.name, sizeof(g_opponent_name)-1);
                g_duel_state = S_INVITE_RECV;
                g_state_enter_ms = millis();
                g_needs_redraw = true;
            }
            return;
        }
        return; // ignore invite in any other state
    }

    // For all other phases, require matching game_id (already pre-checked before dispatch)

    if (g_duel_state == S_INVITE_SENT && msg.phase == DP_ACCEPT) {
        clear_pending();
        strncpy(g_opponent_name, msg.name, sizeof(g_opponent_name)-1);
        ensure_duel_peer(g_opponent_mac);
        g_my_hp = 100; g_op_hp = 100;
        g_current_round = 1;
        enter_round_pick();
        return;
    }

    if ((g_duel_state == S_INVITE_SENT || g_duel_state == S_INVITE_RECV) &&
        (msg.phase == DP_DECLINE || msg.phase == DP_FORFEIT)) {
        clear_pending();
        Serial.println("[duel] invite declined/forfeited → S_IDLE");
        enter_idle();
        return;
    }

    // Active round = any state where a commit/reveal for the current round is valid.
    // Crucially we accept the opponent's COMMIT even while we're still in S_ROUND_PICK
    // (haven't locked our own move yet) — otherwise an early commit is lost forever.
    bool active_round = (g_duel_state == S_ROUND_PICK  ||
                         g_duel_state == S_WAIT_COMMIT ||
                         g_duel_state == S_WAIT_REVEAL);

    if (msg.phase == DP_COMMIT && active_round && msg.round == g_current_round) {
        if (!g_op_committed) {
            memcpy(g_op_move_hash, msg.move_hash, 32);
            g_op_committed = true;
            Serial.println("[duel] stored opponent commit");
            // The COMMIT is the signed frame — queue an ATECC verify if it carries a sig.
            bool has_sig = false;
            for (int i = 0; i < PUBLIC_KEY_SIZE; i++) if (msg.pubkey[i]) { has_sig = true; break; }
            if (has_sig && g_atecc_ok && !g_needs_verify) {
                portENTER_CRITICAL_ISR(&g_espnow_mux);
                memcpy(&g_pending_verify, &msg, sizeof(DuelMsg));
                memcpy(g_verify_mac, src, 6);
                g_needs_verify = true;
                portEXIT_CRITICAL_ISR(&g_espnow_mux);
            }
        }
        advance_round_protocol();
        return;
    }

    if (msg.phase == DP_REVEAL && active_round && msg.round == g_current_round) {
        if (!g_op_reveal_seen) {
            g_op_reveal_seen = true;
            g_op_reveal_move = msg.move;
            memcpy(g_op_reveal_nonce, msg.nonce, 16);
            Serial.println("[duel] stored opponent reveal");
        }
        advance_round_protocol();
        return;
    }

    // Replay path: the opponent is still retransmitting a reveal for a round we
    // have already finished (we advanced to a later round, or the game ended in a
    // KO) — meaning our final reveal to them was lost. Re-send our cached reveal
    // so they can verify, resolve, and catch up. Without this, the loser of the
    // race is stranded and both badges eventually forfeit-timeout to WIN.
    // Reaching here means the frame was NOT handled by the current-round logic
    // above. Throttled to avoid a replay ping-pong between two advanced badges.
    if (msg.phase == DP_REVEAL && g_last_reveal_valid &&
        msg.round == g_last_reveal_round) {
        uint32_t now_ms = millis();
        if (now_ms - g_last_replay_ms > 800) {
            g_last_replay_ms = now_ms;
            DuelMsg rev = build_msg(DP_REVEAL);
            rev.round = g_last_reveal_round;
            rev.move  = (uint8_t)g_last_reveal_move;
            memcpy(rev.nonce, g_last_reveal_nonce, 16);
            esp_now_send(g_opponent_mac, (const uint8_t*)&rev, sizeof(DuelMsg));
            Serial.printf("[duel] replay reveal for round %d\n", g_last_reveal_round);
        }
        return;
    }

    // Forfeit from any active state
    if (msg.phase == DP_FORFEIT &&
        g_duel_state != S_IDLE && g_duel_state != S_RESULT) {
        clear_pending();
        Serial.println("[duel] opponent forfeited → WIN");
        enter_result(RO_WIN, RR_FORFEIT);
        return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Button dispatcher (called from loop() on button edge)
// ─────────────────────────────────────────────────────────────────────────────

static void dispatch_buttons(uint8_t btns) {
    uint8_t pressed = btns & ~g_prev_btns;
    g_prev_btns = btns;
    if (!pressed) return;
    Serial.printf("[btn] raw=0x%02X pressed=0x%02X state=%d\n", btns, pressed, g_duel_state);

    // Debug HUD toggle: UP+DOWN held together. Works in any state; UP/DOWN are
    // harmless single-press actions everywhere, so the chord is safe to detect here.
    if ((btns & BTN_UP) && (btns & BTN_DOWN)) {
        g_debug_hud      = !g_debug_hud;
        g_needs_redraw   = true;
        g_last_render_ms = millis();
        Serial.printf("[duel] debug HUD %s\n", g_debug_hud ? "ON" : "OFF");
        return;
    }
    // While the HUD is shown, swallow all other input (close it with UP+DOWN).
    if (g_debug_hud) return;

    switch (g_duel_state) {
    case S_IDLE:
        if ((pressed & BTN_UP) && g_peer_cursor > 0) { g_peer_cursor--; g_needs_redraw = true; }
        if ((pressed & BTN_DOWN) && g_peer_cursor < g_duel_peer_count - 1) { g_peer_cursor++; g_needs_redraw = true; }
        if ((pressed & BTN_LEFT) && g_peer_cursor > 0) { g_peer_cursor--; g_needs_redraw = true; }
        if ((pressed & BTN_RIGHT) && g_peer_cursor < g_duel_peer_count - 1) { g_peer_cursor++; g_needs_redraw = true; }
        if ((pressed & BTN_SELECT) && g_duel_peer_count > 0) {
            // Challenge selected peer
            memcpy(g_opponent_mac, g_duel_peers[g_peer_cursor].mac, 6);
            strncpy(g_opponent_name, g_duel_peers[g_peer_cursor].name,
                    sizeof(g_opponent_name)-1);
            g_current_game_id = (uint8_t)(esp_random() & 0xFF);
            add_duel_peer_unencrypted(g_opponent_mac);
            DuelMsg inv = build_msg(DP_INVITE);
            send_reliable(g_opponent_mac, inv);
            g_duel_state = S_INVITE_SENT;
            g_state_enter_ms = millis();
            g_needs_redraw = true;
            Serial.printf("[duel] challenged %.15s → S_INVITE_SENT\n", g_opponent_name);
        }
        break;

    case S_INVITE_RECV:
        // SELECT and CANCEL are mutually exclusive: if a noisy read sets both,
        // prefer ACCEPT so a glitch can't accept-then-immediately-decline.
        if (pressed & BTN_SELECT) {
            // Accept
            clear_pending();
            ensure_duel_peer(g_opponent_mac);
            DuelMsg acc = build_msg(DP_ACCEPT);
            send_reliable(g_opponent_mac, acc);
            g_my_hp = 100; g_op_hp = 100;
            g_current_round = 1;
            enter_round_pick();
            delay(150); // debounce
        } else if (pressed & BTN_CANCEL) {
            DuelMsg dec = build_msg(DP_DECLINE);
            esp_now_send(g_opponent_mac, (const uint8_t*)&dec, sizeof(DuelMsg));
            enter_idle();
        }
        break;

    case S_ROUND_PICK:
        if (!g_move_committed) {
            if ((pressed & BTN_LEFT) || (pressed & BTN_DOWN)) {
                // Cycle left: SLASH→BLAST, SHIELD→SLASH, BLAST→SHIELD
                if (g_selected_move == MV_SLASH)       g_selected_move = MV_BLAST;
                else if (g_selected_move == MV_SHIELD)  g_selected_move = MV_SLASH;
                else                                    g_selected_move = MV_SHIELD;
                g_needs_partial = true;
            }
            if ((pressed & BTN_RIGHT) || (pressed & BTN_UP)) {
                // Cycle right: SLASH→SHIELD, SHIELD→BLAST, BLAST→SLASH
                if (g_selected_move == MV_SLASH)       g_selected_move = MV_SHIELD;
                else if (g_selected_move == MV_SHIELD)  g_selected_move = MV_BLAST;
                else                                    g_selected_move = MV_SLASH;
                g_needs_partial = true;
            }
            if (pressed & BTN_SELECT) {
                commit_move();
                g_needs_redraw = true;
                delay(150); // debounce
            }
        }
        // Forfeit only on a clean CANCEL (not when SELECT is co-pressed by a glitch).
        if ((pressed & BTN_CANCEL) && !(pressed & BTN_SELECT)) {
            DuelMsg ff = build_msg(DP_FORFEIT);
            send_reliable(g_opponent_mac, ff);
            enter_idle();
        }
        break;

    case S_WAIT_COMMIT:
    case S_WAIT_REVEAL:
        if (pressed & BTN_CANCEL) {
            DuelMsg ff = build_msg(DP_FORFEIT);
            send_reliable(g_opponent_mac, ff);
            enter_idle();
        }
        break;

    case S_RESULT:
        if (pressed & BTN_SELECT) {
            // Rematch: challenge same peer
            g_current_game_id = (uint8_t)(esp_random() & 0xFF);
            DuelMsg inv = build_msg(DP_INVITE);
            send_reliable(g_opponent_mac, inv);
            g_duel_state = S_INVITE_SENT;
            g_state_enter_ms = millis();
            g_needs_redraw = true;
        }
        if (pressed & BTN_CANCEL) {
            enter_idle();
        }
        break;

    default: break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// setup()
// ─────────────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    Serial.println("[duel] booting...");

    init_peripherals();
    check_atecc();
    init_espnow();
    load_record();

    s_last_activity = millis();

    // Boot splash screen
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setFont(&FreeMonoBold9pt7b);
        display.setTextColor(GxEPD_BLACK);
        display.setCursor(8, 30);
        display.print("ESP-DUEL");
        display.setCursor(8, 50);
        display.setFont(&FreeMono9pt7b);
        display.printf("as %s", g_callsign);
        display.setCursor(8, 68);
        display.printf("ATECC: %s", g_atecc_ok ? "signed" : "unsigned");
        display.setCursor(8, 86);
        display.printf("W:%lu L:%lu D:%lu", g_wins, g_losses, g_draws);
    } while (display.nextPage());
    display.hibernate();

    // Trigger idle screen draw on first loop() tick
    g_needs_redraw = true;

    Serial.println("[duel] ready");
}

// ─────────────────────────────────────────────────────────────────────────────
// loop()
// ─────────────────────────────────────────────────────────────────────────────

void loop() {
    uint32_t now = millis();

    // 1. Sleep suppression: keep activity timestamp current during active game
    if (g_duel_state != S_IDLE) {
        s_last_activity = now;
    }

    // 2. Game watchdog: no DuelMsg from opponent for 25s in any active state
    if (g_duel_state != S_IDLE && g_duel_state != S_RESULT &&
        g_last_duel_recv_at > 0 &&
        now - g_last_duel_recv_at > 25000) {
        Serial.println("[duel] watchdog — no msg for 25s, forfeit");
        enter_result(RO_WIN, RR_DISCONNECTED);
        return;
    }

    // 3. Reliability manager: retry / forfeit on send failure
    if (g_pending_send.active) {
        if (now - g_pending_send.first_sent_at > 20000) {
            Serial.println("[duel] reliability: 20s forfeit timeout");
            clear_pending();
            on_forfeit_timeout();
        } else if (now - g_pending_send.last_sent_at > 3000 &&
                   g_pending_send.retries < 6) {
            g_pending_send.retries++;
            g_pending_send.last_sent_at = now;
            esp_now_send(g_pending_send.peer_mac,
                         (const uint8_t*)&g_pending_send.msg,
                         sizeof(DuelMsg));
            Serial.printf("[duel] retry %d/6\n", g_pending_send.retries);
            // Update waiting screen with new retry count
            if (g_duel_state == S_WAIT_COMMIT || g_duel_state == S_WAIT_REVEAL) {
                g_needs_partial = true;
            }
        }
    }

    // 4. Discovery broadcast: in S_IDLE, broadcast presence every 5 seconds
    if (g_duel_state == S_IDLE && now - g_last_scan_at > 5000) {
        g_last_scan_at = now;
        DuelMsg scan = {};
        scan.type = MSG_DUEL;
        scan.phase = DP_INVITE;
        scan.game_id = (uint8_t)(esp_random() & 0xFF);
        scan.round = 0;
        strncpy(scan.name, g_callsign, sizeof(scan.name));
        memcpy(scan.mac, g_my_mac, 6);
        esp_now_send(BROADCAST_MAC, (const uint8_t*)&scan, sizeof(DuelMsg));
    }

    // 5. Invite / state timeouts (15s for invite states)
    if ((g_duel_state == S_INVITE_SENT || g_duel_state == S_INVITE_RECV) &&
        now - g_state_enter_ms > 15000) {
        Serial.println("[duel] invite timeout → S_IDLE");
        if (g_duel_state == S_INVITE_RECV) {
            DuelMsg dec = build_msg(DP_DECLINE);
            esp_now_send(g_opponent_mac, (const uint8_t*)&dec, sizeof(DuelMsg));
        }
        clear_pending();
        enter_idle();
    }

    // 6. Consume received DuelMsg (deferred from on_recv ISR context)
    bool    have_msg = false;
    DuelMsg rmsg     = {};
    uint8_t rmac[6]  = {};
    uint8_t rdes[6]  = {};
    portENTER_CRITICAL(&g_espnow_mux);
    if (g_duel_recv_flag) {
        memcpy(&rmsg, &g_duel_recv_msg, sizeof(DuelMsg));
        memcpy(rmac, g_duel_recv_mac, 6);
        memcpy(rdes, g_duel_recv_des, 6);
        g_duel_recv_flag = false;
        have_msg         = true;
    }
    portEXIT_CRITICAL(&g_espnow_mux);

    if (have_msg) {
        // Reject stale game_id for non-invite phases
        if (rmsg.phase != DP_INVITE && rmsg.game_id != g_current_game_id) {
            Serial.println("[duel] stale game_id, discarded");
        } else {
            g_last_duel_recv_at = now;
            bool bcast = (memcmp(rdes, BROADCAST_MAC, 6) == 0);
            // Suppress the every-5s broadcast-discovery spam; log everything else.
            if (!(bcast && rmsg.phase == DP_INVITE)) {
                Serial.printf("[duel] recv phase=%d %s from %.15s (%02X:%02X) state=%d\n",
                              rmsg.phase, bcast ? "BCAST" : "UNICAST", rmsg.name,
                              rmac[4], rmac[5], g_duel_state);
            }
            dispatch_recv(rmsg, rmac, rdes);
        }
    }

    // 7. ATECC deferred verify
    if (g_needs_verify && g_atecc_ok) {
        // Copy out of the volatile flag atomically
        DuelMsg local_verify = {};
        portENTER_CRITICAL(&g_espnow_mux);
        memcpy(&local_verify, &g_pending_verify, sizeof(DuelMsg));
        g_needs_verify = false;
        portEXIT_CRITICAL(&g_espnow_mux);

        // Verify signature over the 77-byte signed region [0..76]
        uint8_t digest[32];
        mbedtls_sha256_context ctx;
        mbedtls_sha256_init(&ctx);
        mbedtls_sha256_starts(&ctx, 0);
        mbedtls_sha256_update(&ctx, (const uint8_t*)&local_verify, 77);
        mbedtls_sha256_finish(&ctx, digest);
        mbedtls_sha256_free(&ctx);

        g_atecc.wakeUp();
        bool ok = g_atecc.verifySignature(digest, local_verify.sig, local_verify.pubkey);
        g_atecc.sleep();

        g_verify_result = ok;
        if (ok) {
            g_op_sig_verified = true;
            Serial.println("[atecc] opponent sig VERIFIED");
        } else {
            Serial.println("[atecc] opponent sig FAILED");
        }
    }

    // 8. Button handling — edge-detect via ISR flag + TCA9534 read
    if (g_btn_irq) {
        g_btn_irq = false;
        s_last_activity = now;
        uint8_t btns = read_buttons();
        dispatch_buttons(btns);
    }

    // 9. Invite countdown: redraw S_INVITE_RECV every second for the countdown timer.
    //    Debug HUD: refresh live values (~1.5s — slower to stay ahead of e-paper partials).
    if (g_debug_hud) {
        if (now - g_last_render_ms > 1500) {
            g_needs_redraw   = true;
            g_last_render_ms = now;
        }
    } else if (g_duel_state == S_INVITE_RECV && now - g_last_render_ms > 1000) {
        g_needs_redraw = true;
        g_last_render_ms = now;
    }

    // 10. Render (Phase 4)
    render();

    delay(20);
}
