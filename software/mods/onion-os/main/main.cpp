#include <Arduino.h>
#include <SPI.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_now.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include <esp_sntp.h>
#include <mqtt_client.h>
#include <driver/i2s_std.h>
#include <driver/i2s_pdm.h>
#include <cJSON.h>
#include <mbedtls/base64.h>
#include <sodium.h>

extern "C" {
#include "cryptoauthlib.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include <GxEPD2_BW.h>
#include <gdey/GxEPD2_270_GDEY027T91.h>
#include <Fonts/FreeMono9pt7b.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeMonoBold18pt7b.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if __has_include("onion_config.generated.h")
#include "onion_config.generated.h"
#elif __has_include("onion_config.h")
#include "onion_config.h"
#else
#define ONION_DEFAULT_WIFI_SSID ""
#define ONION_DEFAULT_WIFI_PASSWORD ""
#define ONION_DEFAULT_SERVER_BASE_URL "https://oniondao.dev"
#define ONION_DEFAULT_BADGE_API_KEY ""
#define ONION_DEFAULT_MQTT_URI ""
#define ONION_DEFAULT_MQTT_USERNAME "oniondao"
#define ONION_DEFAULT_MQTT_PASSWORD "02eb3d5e04fd2dc9cbb6f3f5c6c9d89d9c96acd987cc24ddf9f717f9480e8786"
#define ONION_DEFAULT_MQTT_TOPIC_PREFIX "oniondao"
#define ONION_DEFAULT_SCRIPT_MANIFEST_URL ""
#endif

#include "badge_pins.h"
#include "logo_bitmap.h"

#ifndef ONION_HARDCODED_WIFI_SSID
#define ONION_HARDCODED_WIFI_SSID "CIC Guest"
#endif
#ifndef ONION_HARDCODED_WIFI_PASSWORD
#define ONION_HARDCODED_WIFI_PASSWORD "1nnovation"
#endif
#ifndef ONION_HARDCODED_SERVER_BASE_URL
#define ONION_HARDCODED_SERVER_BASE_URL "https://oniondao.dev"
#endif
#ifndef ONION_HARDCODED_MQTT_URI
#define ONION_HARDCODED_MQTT_URI "mqtt://shortline.proxy.rlwy.net:20928"
#endif
#ifndef ONION_HARDCODED_MQTT_USERNAME
#define ONION_HARDCODED_MQTT_USERNAME "oniondao"
#endif
#ifndef ONION_HARDCODED_MQTT_PASSWORD
#define ONION_HARDCODED_MQTT_PASSWORD "02eb3d5e04fd2dc9cbb6f3f5c6c9d89d9c96acd987cc24ddf9f717f9480e8786"
#endif

#define TCA9534_ADDR   0x20
#define TCA9534_INPUT  0x00
#define TCA9534_CONFIG 0x03

#define BTN_LEFT   (1 << 0)
#define BTN_DOWN   (1 << 1)
#define BTN_UP     (1 << 2)
#define BTN_RIGHT  (1 << 3)
#define BTN_SELECT (1 << 4)
#define BTN_CANCEL (1 << 5)

#define SERIAL_BAUD 115200
#define WIFI_CONNECT_TIMEOUT_MS 15000
#define HANDSHAKE_INTERVAL_MS 30000
#define PROFILE_REFRESH_INTERVAL_MS 60000
#define MQTT_RECONNECT_INTERVAL_MS 5000
#define MQTT_HANDSHAKE_ACCEPT_WINDOW_MS 10000
#define BOOT_SPLASH_MS 3000
// Lua allocates from PSRAM where available; scripts can be large enough for
// pushed apps and streaming voice helpers without relying on fragmented RAM.
#define MAX_SCRIPT_BYTES (256 * 1024)
#define MQTT_CLIENT_BUFFER_BYTES (32 * 1024)
#define MQTT_CLIENT_OUT_BUFFER_BYTES 4096
#define MQTT_RX_MAX_BYTES (MAX_SCRIPT_BYTES * 2 + 4096)
#define MQTT_RX_TEMP_JSON_PATH "/mqtt_lua_push.json"
#define LUA_PUSH_TEMP_PATH "/scripts_push_tmp.lua"
#define MAX_IMAGE_BYTES (192 * 1024)
#define ATECC_HMAC_SLOT 10
#define ATECC_I2C_ADDRESS_8BIT 0xC0
#define ATECC_SERIAL_LEN 9
#define SOLANA_PUBKEY_LEN 32
#define SOLANA_SIGNATURE_LEN 64
#define SOLANA_SECRET_KEY_LEN 64
#define SOLANA_SEED_LEN 32
#define SOLANA_KEY_NONCE_LEN crypto_aead_xchacha20poly1305_ietf_NPUBBYTES
#define SOLANA_KEY_MAC_LEN crypto_aead_xchacha20poly1305_ietf_ABYTES
#define LUA_GPIO_POLL_MAX_MS 30000
#define LUA_SLEEP_MAX_MS 60000
#define LUA_ESPNOW_RECV_MAX_MS 30000
#define ONION_ESPNOW_MAX_PAYLOAD 240
#define LUA_KV_MAX_VALUE 240          // kv values mirror the ESP-NOW payload cap
// Static internal RAM (~32 KB); the queue is filled from the WiFi task
// callback, so it must not live in PSRAM. Sized so a receiver can sit inside
// a full e-ink refresh (~1.7 s) while a peer streams 240-byte voice frames at
// ~66 frames/s (~112 frames) without dropping any.
#define ONION_ESPNOW_QUEUE_LEN 128
#define LUA_HTTP_MAX_TIMEOUT_MS 30000
#define LUA_HTTP_DEFAULT_TIMEOUT_MS 10000
#define LUA_MQTT_RECV_MAX_MS 30000
#define ONION_LUA_MQTT_MAX_TOPIC 128
#define ONION_LUA_MQTT_MAX_PAYLOAD 512
#define ONION_LUA_MQTT_QUEUE_LEN 8
#define ONION_LUA_MQTT_MAX_SUBS 8
#define ONION_CHECKIN_SCAN_INTERVAL_MS 5000
#define ONION_CHECKIN_PROMPT_COOLDOWN_MS 300000
#define ONION_CHECKIN_RESULT_MS 15000
#define ONION_CHECKIN_DEFAULT_MIN_RSSI -62
// Swappable side-port modules (see docs/MODULES.md). CC1101 and the Sound
// module share the same physical pins, so only one may be active at a time.
#define CC1101_XOSC_MHZ 26.0
#define CC1101_SPI_HZ 4000000
#define SUBGHZ_MAX_PACKET 61
#define SUBGHZ_RX_MAX_MS 30000
#define SOUND_SPK_SAMPLE_RATE 44100
#define SOUND_MIC_SAMPLE_RATE 16000
#define SOUND_TONE_MAX_MS 10000
#define SOUND_PLAY_MAX_BYTES 65536
#define SOUND_MIC_MAX_SAMPLES 4096
#define SOUND_MIC_READ_MAX_TIMEOUT_MS 5000
#define SOUND_MIC_MAX_DISCARD_MS 1000
#define SOUND_AMP_UNMUTE_MS 100
#define ONION_DISPLAY_WIDTH 264
#define ONION_DISPLAY_HEIGHT 176
#ifndef PIN_BATTERY_ADC
#define PIN_BATTERY_ADC -1
#endif
#ifndef BATTERY_ADC_DIVIDER_RATIO
#define BATTERY_ADC_DIVIDER_RATIO 2.0f
#endif
#ifndef BATTERY_ADC_OFFSET_MV
#define BATTERY_ADC_OFFSET_MV 0
#endif
#define BATTERY_SAMPLE_INTERVAL_MS 60000
#define BATTERY_REDRAW_PERCENT_STEP 5

GxEPD2_BW<GxEPD2_270_GDEY027T91, GxEPD2_270_GDEY027T91::HEIGHT> display(
    GxEPD2_270_GDEY027T91(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY)
);
static GFXcanvas1 g_luaCanvas(ONION_DISPLAY_WIDTH, ONION_DISPLAY_HEIGHT);

// UI framebuffer — all screen draw functions render here; flushFrame() decides
// whether to push a partial or full refresh to the e-ink panel.
static GFXcanvas1 g_frame(ONION_DISPLAY_WIDTH, ONION_DISPLAY_HEIGHT);
static const int FRAME_BPR   = (ONION_DISPLAY_WIDTH + 7) / 8;   // bytes per row = 33
static const int FRAME_BYTES = FRAME_BPR * ONION_DISPLAY_HEIGHT; // 5808
static uint8_t   g_prevFrame[FRAME_BPR * ONION_DISPLAY_HEIGHT];  // last flushed snapshot
static uint16_t  g_partialCount    = 0;
static bool      g_forceFullRefresh = true;   // first flush is always full
static bool      g_luaDeferFlush    = false;  // true between display_begin/commit
static bool      g_luaFramePending  = false;  // draw happened since last deferred flush

enum Screen : uint8_t {
    SCREEN_BOOT_SPLASH,
    SCREEN_STATUS,
    SCREEN_SCRIPT_EXPLORER,
    SCREEN_LINK_PROMPT,
    SCREEN_TX_PROMPT,
    SCREEN_LUA_PROMPT,
    SCREEN_CHECKIN_PROMPT,
    SCREEN_CHECKIN_RESULT,
    SCREEN_LOG,
    SCREEN_SETTINGS,
    SCREEN_WIFI_OVERVIEW,
    SCREEN_WIFI_SCANNING,
    SCREEN_WIFI_LIST,
    SCREEN_WIFI_PASSWORD,
    SCREEN_WIFI_CONNECTING,
    SCREEN_WIFI_RESULT,
    SCREEN_DELETE_CONFIRM,
};

enum HomeItem : int {
    HOME_ITEM_SCRIPTS,
    HOME_ITEM_REFRESH,
    HOME_ITEM_SETTINGS,
    HOME_ITEM_COUNT,
};

struct RuntimeConfig {
    String wifiSsid;
    String wifiPassword;
    String serverBaseUrl;
    String badgeApiKey;
    String mqttUri;
    String mqttUsername;
    String mqttPassword;
    String mqttTopicPrefix;
    String scriptManifestUrl;
    String moduleVariant;
};

struct BadgeIdentity {
    String hardwareId;
    uint64_t onionId = 0;
    String status = "booting";
    String username;
    String onionCount = "0";
    String solanaPublicKey;
    bool linked = false;
};

struct LinkPrompt {
    String requestId;
    String username;
    bool active = false;
};

struct TransactionPrompt {
    String operationId;
    String requestId;
    String type;
    int amount = 0;
    String transactionBase64;
    bool active = false;
};

struct LuaScriptPrompt {
    String requestId;
    String scriptId;
    String title;
    String fileName;
    String description;
    String authorUsername;
    String downloadUrl;
    String code;
    String codePath;
    int sizeBytes = 0;
    bool active = false;
};

struct CheckInPrompt {
    String beaconId;
    String room;
    String label;
    uint8_t beaconMac[6] = {};
    uint8_t nonce[8] = {};
    int8_t rssi = 0;
    int8_t minRssi = ONION_CHECKIN_DEFAULT_MIN_RSSI;
    bool active = false;
};

struct CheckInResult {
    String beaconId;
    String message;
    int points = 0;
    bool awarded = false;
    uint32_t shownAt = 0;
};

static RuntimeConfig g_config;
static BadgeIdentity g_identity;
static LinkPrompt g_linkPrompt;
static TransactionPrompt g_txPrompt;
static LuaScriptPrompt g_luaPrompt;
static CheckInPrompt g_checkinPrompt;
static CheckInResult g_checkinResult;
static Preferences g_prefs;
static esp_mqtt_client_handle_t g_mqtt = nullptr;
static bool g_mqttConnected = false;
static Screen g_screen     = SCREEN_BOOT_SPLASH;
static Screen g_lastScreen = SCREEN_BOOT_SPLASH; // last screen rendered; drives full-refresh on change
static bool g_needsRedraw = true;
static uint8_t g_lastButtons = 0;
static uint32_t g_lastButtonPoll = 0;
static uint32_t g_lastHandshake = 0;
static uint32_t g_lastProfileRefresh = 0;
static uint32_t g_lastMqttAttempt = 0;
static uint32_t g_lastWifiAttempt = 0;
static uint32_t g_lastCheckInScan = 0;
static uint32_t g_lastCheckInPromptAt = 0;
static uint32_t g_mqttHandshakeSentAt = 0;
static bool g_mqttHandshakePending = false;
static String g_log = "Booting";
static String g_lastLuaError;
static int g_homeSelection = 0;
static int g_scriptSelection = 0;
static std::vector<String> g_scripts;
static String g_pendingDeletePath;
static bool   g_deleteChoice = false; // false=CANCEL, true=DELETE
static bool g_luaDisplayActive = false;
static bool g_ateccReady = false;
static uint8_t g_ateccSerial[ATECC_SERIAL_LEN] = {};
#if PIN_BATTERY_ADC >= 0
static int g_batteryPercent = -1;
static int g_batteryVoltageMv = 0;
static int g_batteryDisplayedPercent = -1;
static uint32_t g_lastBatterySample = 0;
#endif

// ── WiFi setup state ─────────────────────────────────────────────────────────
struct WifiNetwork {
    char ssid[33];
    int8_t rssi;
    bool secured;
};
static std::vector<WifiNetwork> g_wifiNetworks;
static int g_wifiListSel = 0;
static String g_wifiConnectSsid;
static char g_wifiPassBuf[65] = {};
static int g_wifiPassLen = 0;
static int g_settingsSel = 0;
static int g_wifiOverviewSel = 0;
static String g_wifiResultMsg;

#define WIFI_WORKER_IDLE    0
#define WIFI_WORKER_RUNNING 1
#define WIFI_WORKER_DONE    2
#define WIFI_WORKER_FAILED  3
static std::atomic<int> g_wifiWorkerResult(WIFI_WORKER_IDLE);

// Full QWERTY keyboard: 5 char rows + 1 control row (row 5)
static const char* kKbNormal[5] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl",
    "zxcvbnm",
    "!@#$%&*-_=+.",
};
static const char* kKbCaps[5] = {
    "1234567890",
    "QWERTYUIOP",
    "ASDFGHJKL",
    "ZXCVBNM",
    "!@#$%&*-_=+.",
};
static const int kKbTotalRows = 6; // 5 char rows + 1 control row
static int g_kbRow = 0;
static int g_kbCol = 0;
static bool g_kbCaps = false;

struct WifiConnectArgs {
    char ssid[33];
    char pass[65];
};
static WifiConnectArgs g_wifiConnectArgs;
// ─────────────────────────────────────────────────────────────────────────────

static const int kLuaReadableGpios[] = {48, 47, 19, 42, 41, 40, 38, 39, 16, 15, 7, 6, 5, 4};

struct LuaButton {
    const char* name;
    uint8_t mask;
};

struct EspNowQueuedMessage {
    uint8_t mac[6] = {};
    uint8_t len = 0;
    char payload[ONION_ESPNOW_MAX_PAYLOAD + 1] = {};
    int8_t rssi = 0;
    uint32_t receivedAt = 0;
};

static const LuaButton kLuaButtons[] = {
    {"left", BTN_LEFT},
    {"down", BTN_DOWN},
    {"up", BTN_UP},
    {"right", BTN_RIGHT},
    {"select", BTN_SELECT},
    {"cancel", BTN_CANCEL},
};

static const uint8_t kEspNowBroadcastMac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

enum CheckInPacketType : uint8_t {
    CHECKIN_PACKET_ADVERTISE = 1,
    CHECKIN_PACKET_APPROVE = 2,
    CHECKIN_PACKET_RESULT = 3,
};

static const char kCheckInMagic[6] = {'O', 'N', 'C', 'H', 'K', '1'};
static const uint8_t kCheckInVersion = 1;

struct __attribute__((packed)) CheckInPacketHeader {
    char magic[6];
    uint8_t version;
    uint8_t type;
};

struct __attribute__((packed)) CheckInAdvertisePacket {
    CheckInPacketHeader header;
    char beaconId[32];
    char room[32];
    char label[48];
    int8_t minRssi;
    uint8_t nonce[8];
    uint32_t sequence;
};

struct __attribute__((packed)) CheckInApprovePacket {
    CheckInPacketHeader header;
    char beaconId[32];
    uint8_t nonce[8];
    char hardwareId[65];
    uint64_t onionId;
    char username[32];
    char wallet[48];
    int8_t rssi;
    uint32_t approvedAt;
    uint8_t badgeMac[6];
};

struct __attribute__((packed)) CheckInResultPacket {
    CheckInPacketHeader header;
    char beaconId[32];
    uint8_t nonce[8];
    uint8_t awarded;
    uint16_t points;
    char message[80];
};

struct CheckInPendingOffer {
    uint8_t beaconMac[6] = {};
    char beaconId[33] = {};
    char room[33] = {};
    char label[49] = {};
    uint8_t nonce[8] = {};
    int8_t rssi = 0;
    int8_t minRssi = ONION_CHECKIN_DEFAULT_MIN_RSSI;
    uint32_t seenAt = 0;
};

struct CheckInPendingResult {
    char beaconId[33] = {};
    uint8_t nonce[8] = {};
    bool awarded = false;
    uint16_t points = 0;
    char message[81] = {};
};

static bool g_espnowStarted = false;
static uint32_t g_espnowSent = 0;
static uint32_t g_espnowReceived = 0;
static portMUX_TYPE g_espnowMux = portMUX_INITIALIZER_UNLOCKED;
static EspNowQueuedMessage g_espnowQueue[ONION_ESPNOW_QUEUE_LEN];
static uint8_t g_espnowQueueHead = 0;
static uint8_t g_espnowQueueCount = 0;
static portMUX_TYPE g_checkinMux = portMUX_INITIALIZER_UNLOCKED;
static CheckInPendingOffer g_checkinPendingOffer;
static CheckInPendingResult g_checkinPendingResult;
static bool g_checkinOfferPending = false;
static bool g_checkinResultPending = false;
static String g_lastCheckInBeaconId;

static bool refreshPublicProfile(bool quiet);

struct MqttQueuedMessage {
    char topic[ONION_LUA_MQTT_MAX_TOPIC + 1] = {};
    char payload[ONION_LUA_MQTT_MAX_PAYLOAD + 1] = {};
    uint16_t topicLen = 0;
    uint16_t payloadLen = 0;
    uint32_t receivedAt = 0;
};

// Topics a Lua script asked to receive, plus a ring buffer the MQTT task fills
// and onion.mqtt_receive() drains. Guarded by g_luaMqttMux because the MQTT
// client event handler runs on its own task.
static portMUX_TYPE g_luaMqttMux = portMUX_INITIALIZER_UNLOCKED;
static char g_luaMqttSubs[ONION_LUA_MQTT_MAX_SUBS][ONION_LUA_MQTT_MAX_TOPIC + 1];
static uint8_t g_luaMqttSubCount = 0;
static MqttQueuedMessage g_luaMqttQueue[ONION_LUA_MQTT_QUEUE_LEN];
static uint8_t g_luaMqttQueueHead = 0;
static uint8_t g_luaMqttQueueCount = 0;
static String g_mqttRxTopic;
static uint8_t* g_mqttRxPresent = nullptr;
static File g_mqttRxFile;
static int g_mqttRxExpectedLen = 0;
static int g_mqttRxReceivedLen = 0;
static size_t g_mqttRxPresentBytes = 0;
static int g_mqttRxMsgId = 0;

static String prefString(const char* key, const char* fallback) {
    String value = g_prefs.getString(key, "");
    return value.length() ? value : String(fallback);
}

static void saveConfigValue(const char* key, const String& value) {
    g_prefs.putString(key, value);
}

static void setLog(const String& message) {
    bool changed = g_log != message;
    g_log = message;
    Serial.printf("[onion-os] %s\n", message.c_str());
    if (changed && !g_luaDisplayActive) g_needsRedraw = true;
}

static String generateHardwareId() {
    char buf[65];
    for (int i = 0; i < 32; i += 4) {
        uint32_t r = esp_random();
        snprintf(buf + (i * 2), 9, "%08lx", (unsigned long)r);
    }
    buf[64] = '\0';
    return String(buf);
}

static void loadConfig() {
    g_prefs.begin("onion-os", false);
    g_config.wifiSsid = prefString("wifi_ssid", ONION_HARDCODED_WIFI_SSID);
    g_config.wifiPassword = prefString("wifi_pass", ONION_HARDCODED_WIFI_PASSWORD);
    g_config.serverBaseUrl = ONION_HARDCODED_SERVER_BASE_URL;
    g_config.badgeApiKey = prefString("api_key", ONION_DEFAULT_BADGE_API_KEY);
    g_config.mqttUri = ONION_HARDCODED_MQTT_URI;
    g_config.mqttUsername = ONION_HARDCODED_MQTT_USERNAME;
    g_config.mqttPassword = ONION_HARDCODED_MQTT_PASSWORD;
    g_config.mqttTopicPrefix = prefString("mqtt_prefix", ONION_DEFAULT_MQTT_TOPIC_PREFIX);
    g_config.scriptManifestUrl = prefString("script_url", ONION_DEFAULT_SCRIPT_MANIFEST_URL);
    g_config.moduleVariant = prefString("mod_variant", "L1");
    g_identity.hardwareId = prefString("hw_id", "");
    if (!g_identity.hardwareId.length()) {
        g_identity.hardwareId = generateHardwareId();
        g_prefs.putString("hw_id", g_identity.hardwareId);
    }
    g_identity.onionId = g_prefs.getULong64("onion_id", 0);
    g_identity.status = prefString("status", "new");
    g_identity.username = prefString("username", "");
    g_identity.onionCount = prefString("onions", "0");
    g_identity.solanaPublicKey = prefString("sol_pub", "");
    g_identity.linked = g_prefs.getBool("linked", false);
}

static String bytesToHex(const uint8_t* data, size_t len) {
    static const char* hex = "0123456789abcdef";
    String out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out += hex[data[i] >> 4];
        out += hex[data[i] & 0x0F];
    }
    return out;
}

static bool prefsGetBytes(const char* key, uint8_t* out, size_t len) {
    if (!g_prefs.isKey(key)) return false;
    return g_prefs.getBytesLength(key) == len && g_prefs.getBytes(key, out, len) == len;
}

static String jsonEscape(const String& value) {
    String out;
    out.reserve(value.length() + 8);
    for (size_t i = 0; i < value.length(); ++i) {
        char ch = value[i];
        if (ch == '"' || ch == '\\') {
            out += '\\';
            out += ch;
        } else if (ch == '\n') {
            out += "\\n";
        } else if (ch == '\r') {
            out += "\\r";
        } else if (ch == '\t') {
            out += "\\t";
        } else {
            out += ch;
        }
    }
    return out;
}

static bool base64Decode(const String& input, std::vector<uint8_t>& out) {
    size_t olen = 0;
    int rc = mbedtls_base64_decode(nullptr, 0, &olen,
        reinterpret_cast<const unsigned char*>(input.c_str()), input.length());
    if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL && rc != 0) return false;
    out.assign(olen, 0);
    rc = mbedtls_base64_decode(out.data(), out.size(), &olen,
        reinterpret_cast<const unsigned char*>(input.c_str()), input.length());
    if (rc != 0) return false;
    out.resize(olen);
    return true;
}

static String base64Encode(const uint8_t* data, size_t len) {
    size_t olen = 0;
    mbedtls_base64_encode(nullptr, 0, &olen, data, len);
    std::vector<uint8_t> out(olen + 1, 0);
    if (mbedtls_base64_encode(out.data(), out.size(), &olen, data, len) != 0) return String();
    return String(reinterpret_cast<const char*>(out.data())).substring(0, olen);
}

static String base58Encode(const uint8_t* data, size_t len) {
    static const char* alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    size_t zeros = 0;
    while (zeros < len && data[zeros] == 0) zeros++;

    std::vector<uint8_t> b58((len - zeros) * 138 / 100 + 1);
    size_t length = 0;
    for (size_t i = zeros; i < len; ++i) {
        int carry = data[i];
        size_t j = 0;
        for (auto it = b58.rbegin(); (carry != 0 || j < length) && it != b58.rend(); ++it, ++j) {
            carry += 256 * (*it);
            *it = carry % 58;
            carry /= 58;
        }
        length = j;
    }

    String out;
    out.reserve(zeros + length);
    for (size_t i = 0; i < zeros; ++i) out += '1';
    auto it = b58.begin() + (b58.size() - length);
    while (it != b58.end()) out += alphabet[*it++];
    return out;
}

static bool readSolanaShortVec(const std::vector<uint8_t>& data, size_t& offset, size_t& value) {
    value = 0;
    int shift = 0;
    for (int i = 0; i < 3; ++i) {
        if (offset >= data.size()) return false;
        uint8_t byte = data[offset++];
        value |= (size_t)(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) return true;
        shift += 7;
    }
    return false;
}

static void restartSharedI2cBus() {
    Wire.begin(PIN_SDA, PIN_SCL);
    Wire.setClock(100000);
}

static bool ateccHmac(const std::vector<uint8_t>& message, uint8_t digest[32], String* serialHex, String& error) {
    Wire.end();
    delay(5);
    atcab_release();

    ATCA_IFACECFG_I2C_ADDRESS(&cfg_ateccx08a_i2c_default) = ATECC_I2C_ADDRESS_8BIT;
    cfg_ateccx08a_i2c_default.atcai2c.bus = 0;
    cfg_ateccx08a_i2c_default.atcai2c.baud = 100000;

    ATCA_STATUS status = atcab_init(&cfg_ateccx08a_i2c_default);
    if (status != ATCA_SUCCESS) {
        error = "ATECC init failed " + String((int)status);
        restartSharedI2cBus();
        return false;
    }

    status = atcab_read_serial_number(g_ateccSerial);
    if (status != ATCA_SUCCESS) {
        error = "ATECC serial failed " + String((int)status);
        atcab_release();
        restartSharedI2cBus();
        return false;
    }
    g_ateccReady = true;
    if (serialHex) *serialHex = bytesToHex(g_ateccSerial, sizeof(g_ateccSerial));

    status = atcab_sha_hmac(message.data(), message.size(), ATECC_HMAC_SLOT, digest, SHA_MODE_TARGET_OUT_ONLY);
    atcab_release();
    restartSharedI2cBus();
    if (status != ATCA_SUCCESS) {
        error = "ATECC HMAC failed " + String((int)status);
        return false;
    }
    return true;
}

static bool ateccRandom(uint8_t* out, size_t count, String& error) {
    if (count == 0) return true;

    Wire.end();
    delay(5);
    atcab_release();

    ATCA_IFACECFG_I2C_ADDRESS(&cfg_ateccx08a_i2c_default) = ATECC_I2C_ADDRESS_8BIT;
    cfg_ateccx08a_i2c_default.atcai2c.bus = 0;
    cfg_ateccx08a_i2c_default.atcai2c.baud = 100000;

    ATCA_STATUS status = atcab_init(&cfg_ateccx08a_i2c_default);
    if (status != ATCA_SUCCESS) {
        error = "ATECC init failed " + String((int)status);
        restartSharedI2cBus();
        return false;
    }

    // atcab_random() returns 32 random bytes per call; loop to fill larger buffers.
    size_t produced = 0;
    while (produced < count) {
        uint8_t block[32];
        status = atcab_random(block);
        if (status != ATCA_SUCCESS) {
            error = "ATECC random failed " + String((int)status);
            atcab_release();
            restartSharedI2cBus();
            return false;
        }
        size_t chunk = count - produced;
        if (chunk > sizeof(block)) chunk = sizeof(block);
        memcpy(out + produced, block, chunk);
        produced += chunk;
    }

    atcab_release();
    restartSharedI2cBus();
    return true;
}

static std::vector<uint8_t> stringBytes(const String& value) {
    return std::vector<uint8_t>(
        reinterpret_cast<const uint8_t*>(value.c_str()),
        reinterpret_cast<const uint8_t*>(value.c_str()) + value.length()
    );
}

static bool deriveWrappingKey(uint8_t key[32], String& error) {
    String context = "onion-os:solana-seed-wrap:v1:" + g_identity.hardwareId;
    std::vector<uint8_t> bytes = stringBytes(context);
    return ateccHmac(bytes, key, nullptr, error);
}

static bool createAteccAttestation(const String& purpose, const String& subject, String& json, String& error) {
    uint8_t nonce[32];
    uint8_t mac[32];
    esp_fill_random(nonce, sizeof(nonce));

    String context = "onion-os:attestation:v1:" + purpose + ":" + subject + ":" +
        bytesToHex(nonce, sizeof(nonce)) + ":" + g_identity.solanaPublicKey;
    std::vector<uint8_t> bytes = stringBytes(context);
    String serialHex;
    if (!ateccHmac(bytes, mac, &serialHex, error)) return false;

    json = "{\"version\":1,\"slot\":" + String(ATECC_HMAC_SLOT) +
        ",\"serial\":\"" + serialHex +
        "\",\"purpose\":\"" + jsonEscape(purpose) +
        "\",\"subject\":\"" + jsonEscape(subject) +
        "\",\"nonce\":\"" + bytesToHex(nonce, sizeof(nonce)) +
        "\",\"hmac\":\"" + bytesToHex(mac, sizeof(mac)) + "\"}";
    return true;
}

static bool decryptSolanaSeed(uint8_t seed[SOLANA_SEED_LEN], String& error) {
    uint8_t nonce[SOLANA_KEY_NONCE_LEN];
    uint8_t ciphertext[SOLANA_SEED_LEN + SOLANA_KEY_MAC_LEN];
    if (!prefsGetBytes("key_nonce", nonce, sizeof(nonce)) ||
        !prefsGetBytes("key_ct", ciphertext, sizeof(ciphertext))) {
        error = "No wrapped wallet seed";
        return false;
    }

    uint8_t wrapKey[32];
    if (!deriveWrappingKey(wrapKey, error)) return false;

    unsigned long long plainLen = 0;
    int rc = crypto_aead_xchacha20poly1305_ietf_decrypt(
        seed, &plainLen, nullptr,
        ciphertext, sizeof(ciphertext),
        nullptr, 0,
        nonce, wrapKey
    );
    sodium_memzero(wrapKey, sizeof(wrapKey));
    if (rc != 0 || plainLen != SOLANA_SEED_LEN) {
        error = "Wallet unwrap failed";
        return false;
    }
    return true;
}

static bool storeSolanaSeed(const uint8_t seed[SOLANA_SEED_LEN], const uint8_t pubkey[SOLANA_PUBKEY_LEN], String& error) {
    uint8_t nonce[SOLANA_KEY_NONCE_LEN];
    uint8_t ciphertext[SOLANA_SEED_LEN + SOLANA_KEY_MAC_LEN];
    uint8_t wrapKey[32];
    randombytes_buf(nonce, sizeof(nonce));
    if (!deriveWrappingKey(wrapKey, error)) return false;

    unsigned long long cipherLen = 0;
    int rc = crypto_aead_xchacha20poly1305_ietf_encrypt(
        ciphertext, &cipherLen,
        seed, SOLANA_SEED_LEN,
        nullptr, 0, nullptr,
        nonce, wrapKey
    );
    sodium_memzero(wrapKey, sizeof(wrapKey));
    if (rc != 0 || cipherLen != sizeof(ciphertext)) {
        error = "Wallet wrap failed";
        return false;
    }

    g_prefs.putBytes("key_nonce", nonce, sizeof(nonce));
    g_prefs.putBytes("key_ct", ciphertext, sizeof(ciphertext));
    g_identity.solanaPublicKey = base58Encode(pubkey, SOLANA_PUBKEY_LEN);
    g_prefs.putString("sol_pub", g_identity.solanaPublicKey);
    return true;
}

static bool loadOrCreateSolanaKey(bool rotate, String& error) {
    uint8_t seed[SOLANA_SEED_LEN];
    uint8_t pubkey[SOLANA_PUBKEY_LEN];
    uint8_t secret[SOLANA_SECRET_KEY_LEN];
    bool hasWrappedKey = g_prefs.isKey("key_nonce") || g_prefs.isKey("key_ct");

    if (!rotate && hasWrappedKey) {
        if (!decryptSolanaSeed(seed, error)) return false;
        crypto_sign_seed_keypair(pubkey, secret, seed);
        g_identity.solanaPublicKey = base58Encode(pubkey, SOLANA_PUBKEY_LEN);
        g_prefs.putString("sol_pub", g_identity.solanaPublicKey);
        sodium_memzero(seed, sizeof(seed));
        sodium_memzero(secret, sizeof(secret));
        return true;
    }

    randombytes_buf(seed, sizeof(seed));
    crypto_sign_seed_keypair(pubkey, secret, seed);
    bool ok = storeSolanaSeed(seed, pubkey, error);
    sodium_memzero(seed, sizeof(seed));
    sodium_memzero(secret, sizeof(secret));
    return ok;
}

static void clearSolanaKey() {
    g_prefs.remove("key_nonce");
    g_prefs.remove("key_ct");
    g_prefs.remove("sol_pub");
    g_identity.solanaPublicKey = "";
}

static String topic(const String& suffix) {
    String prefix = g_config.mqttTopicPrefix.length() ? g_config.mqttTopicPrefix : "oniondao";
    if (prefix.endsWith("/")) prefix.remove(prefix.length() - 1);
    return prefix + "/" + suffix;
}

static uint64_t handshakeOnionIdFromTopic(const String& incomingTopic) {
    String base = topic("badge/");
    if (!incomingTopic.startsWith(base) || !incomingTopic.endsWith("/handshake/accepted")) return 0;

    int idStart = base.length();
    int idEnd = incomingTopic.indexOf('/', idStart);
    if (idEnd <= idStart) return 0;

    String idText = incomingTopic.substring(idStart, idEnd);
    char* end = nullptr;
    uint64_t onionId = strtoull(idText.c_str(), &end, 10);
    return (end && *end == '\0') ? onionId : 0;
}

static void initPeripherals() {
    pinMode(PIN_PWR, OUTPUT);
    // GPIO18 drives the battery power-hold latch. Keep it released so the
    // physical ON/OFF switch can actually cut VSYS and the display power.
    digitalWrite(PIN_PWR, LOW);

    pinMode(PIN_SE_EN, OUTPUT);
    digitalWrite(PIN_SE_EN, HIGH);
    delay(50);

    Wire.begin(PIN_SDA, PIN_SCL);
    Wire.setClock(100000);

    Wire.beginTransmission(TCA9534_ADDR);
    Wire.write(TCA9534_CONFIG);
    Wire.write(0xFF);
    Wire.endTransmission();

    SPI.begin(PIN_EPD_SCK, -1, PIN_EPD_MOSI, PIN_EPD_CS);
    display.init(SERIAL_BAUD, true, 10, false);
    display.setRotation(1);

#if PIN_BATTERY_ADC >= 0
    pinMode(PIN_BATTERY_ADC, INPUT);
    analogReadResolution(12);
    analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);
#endif

    SPIFFS.begin(true);
}

static uint8_t readButtons() {
    Wire.beginTransmission(TCA9534_ADDR);
    Wire.write(TCA9534_INPUT);
    if (Wire.endTransmission(false) != 0) return 0;
    if (Wire.requestFrom(TCA9534_ADDR, 1) != 1) return 0;
    return (~Wire.read()) & 0x3F;
}

static void printLine(const char* text, int y, const GFXfont* font = &FreeMono9pt7b) {
    g_frame.setFont(font);
    g_frame.setTextColor(GxEPD_BLACK);
    g_frame.setCursor(6, y);
    g_frame.print(text);
}

static void printString(const String& text, int y, const GFXfont* font = &FreeMono9pt7b) {
    printLine(text.c_str(), y, font);
}

static String clipped(const String& value, size_t len) {
    if (value.length() <= len) return value;
    return value.substring(0, len - 3) + "...";
}

static void sampleBattery(bool force = false) {
#if PIN_BATTERY_ADC >= 0
    struct CurvePoint {
        int mv;
        int percent;
    };
    static const CurvePoint curve[] = {
        {4200, 100},
        {4110, 90},
        {4020, 80},
        {3920, 70},
        {3840, 60},
        {3790, 50},
        {3750, 40},
        {3710, 30},
        {3670, 20},
        {3610, 10},
        {3300, 0},
    };

    uint32_t now = millis();
    if (!force && now - g_lastBatterySample < BATTERY_SAMPLE_INTERVAL_MS) return;
    g_lastBatterySample = now;

    analogRead(PIN_BATTERY_ADC);
    uint32_t pinMv = 0;
    const int samples = 8;
    for (int i = 0; i < samples; ++i) {
        pinMv += analogReadMilliVolts(PIN_BATTERY_ADC);
        delay(2);
    }
    pinMv /= samples;

    int batteryMv = (int)lroundf((float)pinMv * BATTERY_ADC_DIVIDER_RATIO) + BATTERY_ADC_OFFSET_MV;
    int percent = 0;
    if (batteryMv >= curve[0].mv) {
        percent = 100;
    } else {
        for (size_t i = 1; i < sizeof(curve) / sizeof(curve[0]); ++i) {
            if (batteryMv >= curve[i].mv) {
                int highMv = curve[i - 1].mv;
                int lowMv = curve[i].mv;
                int highPct = curve[i - 1].percent;
                int lowPct = curve[i].percent;
                percent = lowPct + ((batteryMv - lowMv) * (highPct - lowPct)) / (highMv - lowMv);
                break;
            }
        }
    }
    bool shouldRedraw = force || g_batteryDisplayedPercent < 0 ||
        abs(percent - g_batteryDisplayedPercent) >= BATTERY_REDRAW_PERCENT_STEP;

    g_batteryVoltageMv = batteryMv;
    g_batteryPercent = percent;
    if (shouldRedraw) {
        g_batteryDisplayedPercent = percent;
        if (g_screen == SCREEN_STATUS) g_needsRedraw = true;
    }
#else
    (void)force;
#endif
}

static String storedScriptDisplayName(const String& path) {
    String name = path;
    if (name.startsWith("/")) name.remove(0, 1);
    if (name.startsWith("scripts_")) name.remove(0, 8);
    return name;
}

static bool validAssetFileName(const String& name, const char* requiredSuffix = nullptr) {
    if (!name.length() || name.length() > 96 ||
        name.indexOf('/') >= 0 || name.indexOf('\\') >= 0) return false;
    if (requiredSuffix && !name.endsWith(requiredSuffix)) return false;
    for (size_t i = 0; i < name.length(); ++i) {
        char ch = name[i];
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_';
        if (!ok) return false;
    }
    return true;
}

static bool validImageFileName(const String& name) {
    if (!validAssetFileName(name)) return false;
    return name.endsWith(".pbm") || name.endsWith(".bmp");
}

static String imagePathForName(const String& name) {
    if (!validImageFileName(name)) return String();
    return "/images_" + name;
}

// arduino-esp32 cores differ in whether File::name() keeps the leading "/";
// without it the startsWith("/images_") filter goes quiet and onion.images()
// silently returns an empty list (the script explorer handles this inline).
static String normalizedSpiffsPath(const String& path) {
    if (path.startsWith("/")) return path;
    return "/" + path;
}

static void refreshScriptList() {
    g_scripts.clear();
    File root = SPIFFS.open("/");
    if (!root) return;

    File file = root.openNextFile();
    while (file) {
        String name = file.name();
        if (!file.isDirectory() &&
            (name.startsWith("/scripts_") || name.startsWith("scripts_")) &&
            name.endsWith(".lua")) {
            g_scripts.push_back(name.startsWith("/") ? name : "/" + name);
        }
        file = root.openNextFile();
    }

    std::sort(g_scripts.begin(), g_scripts.end(), [](const String& a, const String& b) {
        return strcmp(a.c_str(), b.c_str()) < 0;
    });
    if (g_scriptSelection > (int)g_scripts.size()) g_scriptSelection = (int)g_scripts.size();
}

static void drawBootSplash() {
    g_frame.fillScreen(GxEPD_WHITE);
    int x = (ONION_DISPLAY_WIDTH  - ONION_LOGO_WIDTH)  / 2;
    int y = (ONION_DISPLAY_HEIGHT - ONION_LOGO_HEIGHT) / 2;
    g_frame.drawBitmap(x, y, ONION_LOGO_BLACK_BITMAP, ONION_LOGO_WIDTH, ONION_LOGO_HEIGHT, GxEPD_BLACK);
}

static void drawHomeItem(HomeItem item, const String& label, int y) {
    String prefix = g_homeSelection == item ? "> " : "  ";
    printString(prefix + label, y, g_homeSelection == item ? &FreeMonoBold9pt7b : &FreeMono9pt7b);
}

// Small battery icon drawn in the status header top-right.
// Body is a 28×13 rect; a 3×5 nub on the right edge acts as the terminal.
// When the level is known the body is filled proportionally left-to-right.
// When unknown (PIN_BATTERY_ADC not wired) a centred dash is drawn instead.

static void drawStatus() {
    g_frame.fillScreen(GxEPD_WHITE);
    printLine("ONION OS", 22, &FreeMonoBold18pt7b);

    String user = g_identity.username.length() ? g_identity.username : (g_identity.linked ? "linked" : "not linked");
    printString("User: " + clipped(user, 21), 48, &FreeMonoBold9pt7b);
    printString("Onions: " + clipped(g_identity.onionCount, 18), 66);
    // ID + connection status — no longer collides with a battery text line
    printString("ID: " + String(g_identity.onionId ? String(g_identity.onionId) : "pending") +
        "  " + String(g_mqttConnected ? "MQTT" : (WiFi.status() == WL_CONNECTED ? "WiFi" : "offline")), 84);
    drawHomeItem(HOME_ITEM_SCRIPTS, "Scripts Explorer", 100);
    drawHomeItem(HOME_ITEM_REFRESH, "Refresh Profile", 116);
    drawHomeItem(HOME_ITEM_SETTINGS, "Settings", 132);
    printString(clipped(g_log, 30), 168);
}

static void drawScriptExplorer() {
    g_frame.fillScreen(GxEPD_WHITE);
    printLine("SCRIPTS", 22, &FreeMonoBold18pt7b);

    int start = 0;
    const int visibleRows = 5;
    if (g_scriptSelection >= visibleRows) start = g_scriptSelection - visibleRows + 1;
    int itemCount = (int)g_scripts.size() + 1;
    for (int row = 0; row < visibleRows && start + row < itemCount; ++row) {
        int idx = start + row;
        String prefix = idx == g_scriptSelection ? "> " : "  ";
        String label = idx == 0 ? "Update Scripts" : clipped(storedScriptDisplayName(g_scripts[idx - 1]), 24);
        printString(prefix + label, 52 + row * 20,
            idx == g_scriptSelection ? &FreeMonoBold9pt7b : &FreeMono9pt7b);
    }
    if (g_scripts.empty()) printString("No scripts installed", 154);
}

static void drawDeleteConfirm() {
    g_frame.fillScreen(GxEPD_WHITE);
    printLine("DELETE LUA?", 24, &FreeMonoBold18pt7b);
    printString(clipped(storedScriptDisplayName(g_pendingDeletePath), 24), 58, &FreeMonoBold9pt7b);
    printString("This removes it from badge", 86);
    printString(String(g_deleteChoice ? "  CANCEL    > DELETE" : "> CANCEL      DELETE"), 128,
        &FreeMonoBold9pt7b);
}

static void drawLinkPrompt() {
    g_frame.fillScreen(GxEPD_WHITE);
    printLine("LINK BADGE?", 24, &FreeMonoBold18pt7b);
    printString("User:", 54, &FreeMonoBold9pt7b);
    printString(clipped(g_linkPrompt.username, 22), 74);
    printString("Wallet: " + String(g_identity.solanaPublicKey.length() ? "ready" : "create on approve"), 112);
}

static void drawTransactionPrompt() {
    g_frame.fillScreen(GxEPD_WHITE);
    printLine("SIGN ONIONS?", 24, &FreeMonoBold18pt7b);
    printString("Type: " + clipped(g_txPrompt.type, 18), 54);
    printString("Amount: " + String(g_txPrompt.amount), 74);
    printString("Signer: Ed25519 + ATECC", 112);
}

static void drawLuaPrompt() {
    g_frame.fillScreen(GxEPD_WHITE);
    printLine("INSTALL LUA?", 24, &FreeMonoBold18pt7b);
    printString(clipped(g_luaPrompt.title.length() ? g_luaPrompt.title : g_luaPrompt.fileName, 24), 54, &FreeMonoBold9pt7b);
    printString("By: " + clipped(g_luaPrompt.authorUsername, 18), 76);
    printString(clipped(g_luaPrompt.description, 28), 96);
    printString("Size: " + String(g_luaPrompt.sizeBytes) + " bytes", 116);
}

static void drawCheckInPrompt() {
    g_frame.fillScreen(GxEPD_WHITE);
    printLine("CHECK IN?", 24, &FreeMonoBold18pt7b);
    printString(clipped(g_checkinPrompt.label.length() ? g_checkinPrompt.label : "Workshop attendance", 25),
        54, &FreeMonoBold9pt7b);
    printString("Room: " + clipped(g_checkinPrompt.room.length() ? g_checkinPrompt.room : g_checkinPrompt.beaconId, 21), 78);
    printString("Signal: " + String((int)g_checkinPrompt.rssi) + " dBm", 100);
    printString("SELECT yes", 138, &FreeMonoBold9pt7b);
    printString("CANCEL no", 158);
}

static void drawCheckInResult() {
    g_frame.fillScreen(GxEPD_WHITE);
    printLine(g_checkinResult.awarded ? "CHECKED IN" : "CHECK IN", 24, &FreeMonoBold18pt7b);
    if (g_checkinResult.points > 0) {
        printString("Points: +" + String(g_checkinResult.points), 54, &FreeMonoBold9pt7b);
        printString(clipped(g_checkinResult.message, 27), 78);
    } else {
        printString(clipped(g_checkinResult.message.length() ? g_checkinResult.message : "Waiting for beacon...", 27),
            58, &FreeMonoBold9pt7b);
    }
    printString("SELECT/CANCEL to close", 142);
}

// Forward declarations for WiFi screens (defined after ensureWifi)
static void drawSettingsScreen();
static void drawWifiOverview();
static void drawWifiScanning();
static void drawWifiList();
static void drawWifiPassword();
static void drawWifiConnecting();
static void drawWifiResult();

// Push g_frame to the e-ink panel using a partial refresh when possible.
// Convention: in GFXcanvas1, bit=1 = white (background), bit=0 = black (ink).
// Flush uses drawBitmap(…, GxEPD_WHITE, GxEPD_BLACK) so the panel sees the
// same pixel values as the canvas.
static void flushFrame() {
    const uint8_t* cur = g_frame.getBuffer();

    // Force a full refresh whenever the screen type changes.
    bool screenChanged = (g_screen != g_lastScreen);
    g_lastScreen = g_screen;

    // Compute dirty bounding box in byte-columns (each byte = 8 pixels) × rows.
    int y0 = ONION_DISPLAY_HEIGHT, y1 = -1;
    int bx0 = FRAME_BPR, bx1 = -1;
    for (int y = 0; y < ONION_DISPLAY_HEIGHT; ++y) {
        for (int bx = 0; bx < FRAME_BPR; ++bx) {
            if (cur[y * FRAME_BPR + bx] != g_prevFrame[y * FRAME_BPR + bx]) {
                if (y  < y0)  y0  = y;
                if (y  > y1)  y1  = y;
                if (bx < bx0) bx0 = bx;
                if (bx > bx1) bx1 = bx;
            }
        }
    }

    if (y1 < 0) return; // nothing changed — skip panel entirely

    int dw = (bx1 - bx0 + 1) * 8;
    int dh = y1 - y0 + 1;
    float dirtyPct = (float)(dw * dh) / (float)(ONION_DISPLAY_WIDTH * ONION_DISPLAY_HEIGHT);

    // Full refresh: first frame, screen change, large dirty area, or after 30
    // consecutive partials (periodic ghost-clearing).
    bool fullRefresh = g_forceFullRefresh || screenChanged ||
                       dirtyPct > 0.75f   || g_partialCount >= 30;

    if (fullRefresh) {
        display.setFullWindow();
        display.firstPage();
        do {
            display.drawBitmap(0, 0, cur,
                ONION_DISPLAY_WIDTH, ONION_DISPLAY_HEIGHT,
                GxEPD_WHITE, GxEPD_BLACK);
        } while (display.nextPage());
        g_partialCount    = 0;
        g_forceFullRefresh = false;
    } else {
        int px0 = bx0 * 8;
        int pw  = dw;
        if (px0 + pw > ONION_DISPLAY_WIDTH) pw = ONION_DISPLAY_WIDTH - px0;
        display.setPartialWindow(px0, y0, pw, dh);
        display.firstPage();
        do {
            display.drawBitmap(0, 0, cur,
                ONION_DISPLAY_WIDTH, ONION_DISPLAY_HEIGHT,
                GxEPD_WHITE, GxEPD_BLACK);
        } while (display.nextPage());
        ++g_partialCount;
        // A partial refresh leaves the panel's DC/DC booster running (a full
        // one powers it down at the end of its waveform), and a running e-ink
        // booster puts audible glitches into the Sound module's mic/speaker
        // path. Power off after every partial; the panel keeps its image and
        // its differential RAM.
        display.powerOff();
    }

    memcpy(g_prevFrame, cur, FRAME_BYTES);
}

static void redraw() {
    if (!g_needsRedraw) return;
    if (g_screen == SCREEN_BOOT_SPLASH)      drawBootSplash();
    else if (g_screen == SCREEN_LINK_PROMPT)     drawLinkPrompt();
    else if (g_screen == SCREEN_SCRIPT_EXPLORER) drawScriptExplorer();
    else if (g_screen == SCREEN_TX_PROMPT)       drawTransactionPrompt();
    else if (g_screen == SCREEN_LUA_PROMPT)      drawLuaPrompt();
    else if (g_screen == SCREEN_CHECKIN_PROMPT)  drawCheckInPrompt();
    else if (g_screen == SCREEN_CHECKIN_RESULT)  drawCheckInResult();
    else if (g_screen == SCREEN_SETTINGS)        drawSettingsScreen();
    else if (g_screen == SCREEN_WIFI_OVERVIEW)   drawWifiOverview();
    else if (g_screen == SCREEN_WIFI_SCANNING)   drawWifiScanning();
    else if (g_screen == SCREEN_WIFI_LIST)       drawWifiList();
    else if (g_screen == SCREEN_WIFI_PASSWORD)   drawWifiPassword();
    else if (g_screen == SCREEN_WIFI_CONNECTING) drawWifiConnecting();
    else if (g_screen == SCREEN_WIFI_RESULT)     drawWifiResult();
    else if (g_screen == SCREEN_DELETE_CONFIRM)  drawDeleteConfirm();
    else drawStatus();
    flushFrame();
    g_needsRedraw = false;
}

// Undo walkie-mode 802.11 LR (see onion.wifi_disconnect) before any AP
// association attempt: an LR radio can never join a normal 802.11 AP, so a
// stale LR setting would brick WiFi (and MQTT/sync) until reboot.
static void restoreWifiProtocol() {
    esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
}

static bool ensureWifi() {
    if (WiFi.status() == WL_CONNECTED) return true;
    if (g_wifiWorkerResult.load() == WIFI_WORKER_RUNNING) return false;
    if (g_screen == SCREEN_WIFI_OVERVIEW || g_screen == SCREEN_WIFI_SCANNING ||
        g_screen == SCREEN_WIFI_LIST || g_screen == SCREEN_WIFI_PASSWORD ||
        g_screen == SCREEN_WIFI_CONNECTING) return false;
    if (!g_config.wifiSsid.length()) {
        setLog("Provision WiFi in Settings");
        return false;
    }

    WiFi.mode(WIFI_STA);
    // Fail-safe: a Lua script may have switched the PHY to LR and exited
    // without onion.wifi_reconnect (crash, CANCEL paths).
    restoreWifiProtocol();
    WiFi.begin(g_config.wifiSsid.c_str(), g_config.wifiPassword.c_str());
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
        delay(250);
    }

    if (WiFi.status() == WL_CONNECTED) {
        setLog("WiFi connected");
        if (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET) {
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_init();
        }
        return true;
    }
    setLog("WiFi connect failed");
    return false;
}

// Non-blocking background reconnect — fires WiFi.begin() and returns immediately.
// WiFi driver connects asynchronously; loop() picks up WL_CONNECTED on next check.
static void triggerWifiReconnect() {
    if (WiFi.status() == WL_CONNECTED) return;
    if (g_wifiWorkerResult.load() == WIFI_WORKER_RUNNING) return;
    if (g_screen == SCREEN_WIFI_OVERVIEW || g_screen == SCREEN_WIFI_SCANNING ||
        g_screen == SCREEN_WIFI_LIST || g_screen == SCREEN_WIFI_PASSWORD ||
        g_screen == SCREEN_WIFI_CONNECTING) return;
    if (!g_config.wifiSsid.length()) return;

    WiFi.mode(WIFI_STA);
    // A Lua script may have left the radio in walkie mode (802.11 LR PHY,
    // auto-reconnect off — see onion.wifi_disconnect) and exited without
    // wifi_reconnect; an LR radio can never associate, so restore first.
    restoreWifiProtocol();
    WiFi.setAutoReconnect(true);
    WiFi.begin(g_config.wifiSsid.c_str(), g_config.wifiPassword.c_str());
    setLog("WiFi reconnecting...");
}

// ── WiFi async worker tasks ───────────────────────────────────────────────────

static void wifiScanTask(void*) {
    WiFi.disconnect();
    WiFi.mode(WIFI_STA);
    // An LR-only PHY (walkie mode) cannot hear B/G probe responses.
    restoreWifiProtocol();
    int n = WiFi.scanNetworks(false, false);
    g_wifiNetworks.clear();
    if (n >= 0) {
        for (int i = 0; i < n && i < 20; i++) {
            WifiNetwork net;
            String ssid = WiFi.SSID(i);
            strncpy(net.ssid, ssid.c_str(), 32);
            net.ssid[32] = '\0';
            net.rssi = (int8_t)WiFi.RSSI(i);
            net.secured = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
            if (net.ssid[0]) g_wifiNetworks.push_back(net);
        }
        WiFi.scanDelete();
        g_wifiWorkerResult.store(WIFI_WORKER_DONE);
    } else {
        g_wifiWorkerResult.store(WIFI_WORKER_FAILED);
    }
    vTaskDelete(nullptr);
}

static void wifiConnectTask(void* arg) {
    const WifiConnectArgs* a = reinterpret_cast<const WifiConnectArgs*>(arg);
    WiFi.disconnect();
    WiFi.mode(WIFI_STA);
    restoreWifiProtocol();
    WiFi.setAutoReconnect(true);
    WiFi.begin(a->ssid, a->pass);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    g_wifiWorkerResult.store(WiFi.status() == WL_CONNECTED ? WIFI_WORKER_DONE : WIFI_WORKER_FAILED);
    vTaskDelete(nullptr);
}

static void startWifiScan() {
    g_wifiWorkerResult.store(WIFI_WORKER_RUNNING);
    g_wifiNetworks.clear();
    xTaskCreate(wifiScanTask, "wifi_scan", 4096, nullptr, 5, nullptr);
}

static void startWifiConnect(const char* ssid, const char* pass) {
    g_wifiWorkerResult.store(WIFI_WORKER_RUNNING);
    strncpy(g_wifiConnectArgs.ssid, ssid, 32); g_wifiConnectArgs.ssid[32] = '\0';
    strncpy(g_wifiConnectArgs.pass, pass, 64); g_wifiConnectArgs.pass[64] = '\0';
    xTaskCreate(wifiConnectTask, "wifi_conn", 4096, &g_wifiConnectArgs, 5, nullptr);
}

// ── Keyboard helpers ──────────────────────────────────────────────────────────

static int kbRowLen(int row) {
    if (row < 5) return (int)strlen(kKbNormal[row]);
    return 4; // CAPS, SPACE, DEL, OK
}

static int kbCellW(int row) {
    if (row == 2) return 29;
    if (row == 3) return 37;
    if (row == 4) return 22;
    return 26; // rows 0,1 and control row (handled separately)
}

static int kbStartX(int row) {
    if (row == 2) return 2;  // asdfghjkl: 9×29=261, center in 264
    if (row == 3) return 3;  // zxcvbnm:   7×37=259, center in 264
    return 2;                // rows 0,1: 10×26=260, center in 264
                             // row 4: 12×22=264, start 0 (handled separately)
}

static int kbBoxY(int row) { return 36 + row * 23; }

// ── WiFi screen draw functions ────────────────────────────────────────────────

static void drawSettingsScreen() {
    g_frame.fillScreen(GxEPD_WHITE);
    printLine("SETTINGS", 22, &FreeMonoBold18pt7b);
    printString((g_settingsSel == 0 ? "> " : "  ") + String("WiFi"), 58,
        g_settingsSel == 0 ? &FreeMonoBold9pt7b : &FreeMono9pt7b);
    printString((g_settingsSel == 1 ? "> " : "  ") + String("About"), 78,
        g_settingsSel == 1 ? &FreeMonoBold9pt7b : &FreeMono9pt7b);
}

static void drawWifiOverview() {
    g_frame.fillScreen(GxEPD_WHITE);
    printLine("WIFI", 22, &FreeMonoBold18pt7b);
    bool conn = WiFi.status() == WL_CONNECTED;
    printString(String("Status: ") + (conn ? "Connected" : "Offline"), 48, &FreeMonoBold9pt7b);
    if (conn) {
        printString("SSID: " + clipped(WiFi.SSID(), 22), 66);
        printString("IP: " + WiFi.localIP().toString(), 84);
    } else if (g_config.wifiSsid.length()) {
        printString("Last: " + clipped(g_config.wifiSsid, 22), 66);
    }
    const char* items[] = {"Scan Networks", "Disconnect", "Back"};
    for (int i = 0; i < 3; i++) {
        printString((g_wifiOverviewSel == i ? "> " : "  ") + String(items[i]),
            108 + i * 18, g_wifiOverviewSel == i ? &FreeMonoBold9pt7b : &FreeMono9pt7b);
    }
}

static void drawWifiScanning() {
    g_frame.fillScreen(GxEPD_WHITE);
    printLine("WIFI SCAN", 22, &FreeMonoBold18pt7b);
    printString("Scanning networks...", 60, &FreeMonoBold9pt7b);
    printString("Please wait (1-3s)", 80);
    printString("CANCEL to abort", 140);
}

static void drawWifiList() {
    g_frame.fillScreen(GxEPD_WHITE);
    printLine("NETWORKS", 22, &FreeMonoBold18pt7b);
    if (g_wifiNetworks.empty()) {
        printString("No networks found", 60, &FreeMonoBold9pt7b);
        return;
    }
    const int vis = 5;
    int start = g_wifiListSel >= vis ? g_wifiListSel - vis + 1 : 0;
    for (int r = 0; r < vis && start + r < (int)g_wifiNetworks.size(); r++) {
        int idx = start + r;
        const WifiNetwork& net = g_wifiNetworks[idx];
        char buf[32];
        snprintf(buf, sizeof(buf), "%ddB%s", net.rssi, net.secured ? "*" : " ");
        String line = (idx == g_wifiListSel ? "> " : "  ") +
                      clipped(String(net.ssid), 14) + " " + buf;
        printString(line, 48 + r * 20,
            idx == g_wifiListSel ? &FreeMonoBold9pt7b : &FreeMono9pt7b);
    }
}

static void drawWifiPassword() {
    g_frame.fillScreen(GxEPD_WHITE);

    // Header — FreeMono9pt7b, same as rest of UI
    // Net baseline y=12 (text y=2..12), Pass baseline y=28 (asterisk top ~y=20)
    // Gap between lines ~8px; keyboard starts y=36 (8px below pass baseline)
    g_frame.setFont(&FreeMono9pt7b);
    g_frame.setTextColor(GxEPD_BLACK);
    g_frame.setCursor(4, 12);
    g_frame.print("Net: " + g_wifiConnectSsid);
    g_frame.setCursor(4, 28);
    String passLine = "Pass: ";
    passLine.reserve(g_wifiPassLen + 8);
    for (int i = 0; i < g_wifiPassLen; i++) passLine += '*';
    passLine += '_';
    g_frame.print(passLine);

    // Keyboard — kbBoxY = 36 + row*23, kCellH = 22 (1px gap between rows)
    // Row 5: y=36+5*23=151, height=22, bottom=173 → 3px white edge at bottom
    const int kCellH    = 22;
    const int kBaseline = 16; // baseline within 22px cell

    // Control row: CAPS(58) + SPACE(90) + DEL(58) + OK(58) = 264px
    const int         kCtrlW[4]     = {58, 90, 58, 58};
    const char* const kCtrlLabel[4] = {"CAPS", "SPACE", "DEL", "OK"};

    for (int row = 0; row < kKbTotalRows; row++) {
        const int boxY = kbBoxY(row);

        if (row < 5) {
            const char* rowStr = g_kbCaps ? kKbCaps[row] : kKbNormal[row];
            const int   len    = (int)strlen(rowStr);
            const int   cw     = (row == 4) ? 22 : kbCellW(row);
            const int   sx     = (row == 4) ?  0 : kbStartX(row);

            for (int col = 0; col < len; col++) {
                const int  cx  = sx + col * cw;
                const bool sel = (g_kbRow == row && g_kbCol == col);
                if (sel) {
                    g_frame.fillRect(cx, boxY, cw - 1, kCellH, GxEPD_BLACK);
                    g_frame.setTextColor(GxEPD_WHITE);
                } else {
                    g_frame.fillRect(cx, boxY, cw - 1, kCellH, GxEPD_WHITE);
                    g_frame.drawRect(cx, boxY, cw - 1, kCellH, GxEPD_BLACK);
                    g_frame.setTextColor(GxEPD_BLACK);
                }
                g_frame.setFont(&FreeMono9pt7b);
                char ch[2] = {rowStr[col], '\0'};
                int16_t tx1, ty1; uint16_t tw, th;
                g_frame.getTextBounds(ch, 0, 0, &tx1, &ty1, &tw, &th);
                g_frame.setCursor(cx + ((cw - 1) - (int)tw) / 2 - tx1, boxY + kBaseline);
                g_frame.print(ch);
            }
        } else {
            // Control row
            int x = 0;
            for (int col = 0; col < 4; col++) {
                const int  w       = kCtrlW[col];
                const bool sel     = (g_kbRow == 5 && g_kbCol == col);
                const bool capsLit = (col == 0 && g_kbCaps);
                const bool inv     = sel || capsLit;
                if (inv) {
                    g_frame.fillRect(x, boxY, w - 1, kCellH, GxEPD_BLACK);
                    g_frame.setTextColor(GxEPD_WHITE);
                } else {
                    g_frame.fillRect(x, boxY, w - 1, kCellH, GxEPD_WHITE);
                    g_frame.drawRect(x, boxY, w - 1, kCellH, GxEPD_BLACK);
                    g_frame.setTextColor(GxEPD_BLACK);
                }
                g_frame.setFont(&FreeMono9pt7b);
                int16_t tx1, ty1; uint16_t tw, th;
                g_frame.getTextBounds(kCtrlLabel[col], 0, 0, &tx1, &ty1, &tw, &th);
                g_frame.setCursor(x + ((w - 1) - (int)tw) / 2 - tx1, boxY + kBaseline);
                g_frame.print(kCtrlLabel[col]);
                x += w;
            }
        }
    }
    g_frame.setTextColor(GxEPD_BLACK);
}

static void drawWifiConnecting() {
    g_frame.fillScreen(GxEPD_WHITE);
    printLine("WIFI", 22, &FreeMonoBold18pt7b);
    printString("Connecting to:", 50, &FreeMonoBold9pt7b);
    printString(clipped(g_wifiConnectSsid, 26), 68);
    printString("Please wait...", 100);
    printString("CANCEL to abort", 140);
}

static void drawWifiResult() {
    g_frame.fillScreen(GxEPD_WHITE);
    printLine("WIFI", 22, &FreeMonoBold18pt7b);
    printString(g_wifiResultMsg, 58, &FreeMonoBold9pt7b);
    if (WiFi.status() == WL_CONNECTED) {
        printString("IP: " + WiFi.localIP().toString(), 78);
    }
    printString("SELECT or CANCEL to continue", 120);
}

// ─────────────────────────────────────────────────────────────────────────────

static esp_err_t httpCaptureEvent(esp_http_client_event_t* event) {
    if (event->event_id == HTTP_EVENT_ON_DATA && event->user_data && event->data && event->data_len > 0) {
        String* response = static_cast<String*>(event->user_data);
        response->concat(static_cast<const char*>(event->data), event->data_len);
    }
    return ESP_OK;
}

static int httpPostJson(const String& path, const String& body, String* response) {
    if (!ensureWifi()) return -1;
    String url = g_config.serverBaseUrl;
    if (url.endsWith("/")) url.remove(url.length() - 1);
    url += path;

    String responseBuffer;
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.event_handler = httpCaptureEvent;
    cfg.user_data = &responseBuffer;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (g_config.badgeApiKey.length()) {
        String auth = "Bearer " + g_config.badgeApiKey;
        esp_http_client_set_header(client, "Authorization", auth.c_str());
    }
    esp_http_client_set_post_field(client, body.c_str(), body.length());
    esp_err_t err = esp_http_client_perform(client);
    int code = err == ESP_OK ? esp_http_client_get_status_code(client) : -1;
    if (response) *response = responseBuffer;
    esp_http_client_cleanup(client);
    return code;
}

static String jsonString(cJSON* obj, const char* key) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) && item->valuestring ? String(item->valuestring) : String();
}

static bool jsonValueString(cJSON* obj, const char* key, String& out) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        out = item->valuestring;
        return true;
    }
    if (cJSON_IsNumber(item)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.0f", item->valuedouble);
        out = buf;
        return true;
    }
    return false;
}

static int jsonInt(cJSON* obj, const char* key, int fallback = 0) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static int jsonHexNibble(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static void appendUtf8Codepoint(String& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out += (char)cp;
    } else if (cp <= 0x7FF) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

static bool writeUtf8Codepoint(File& out, uint32_t cp, size_t& written) {
    uint8_t bytes[3];
    size_t len = 0;
    if (cp <= 0x7F) {
        bytes[len++] = (uint8_t)cp;
    } else if (cp <= 0x7FF) {
        bytes[len++] = (uint8_t)(0xC0 | (cp >> 6));
        bytes[len++] = (uint8_t)(0x80 | (cp & 0x3F));
    } else {
        bytes[len++] = (uint8_t)(0xE0 | (cp >> 12));
        bytes[len++] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        bytes[len++] = (uint8_t)(0x80 | (cp & 0x3F));
    }
    if (out.write(bytes, len) != len) return false;
    written += len;
    return true;
}

static int jsonRawFieldStart(const String& json, const char* key) {
    String needle = "\"" + String(key) + "\"";
    int pos = json.indexOf(needle);
    while (pos >= 0) {
        int prev = pos - 1;
        while (prev >= 0 && (json[prev] == ' ' || json[prev] == '\n' || json[prev] == '\r' || json[prev] == '\t')) prev--;
        bool fieldBoundary = prev < 0 || json[prev] == '{' || json[prev] == ',';

        int i = pos + needle.length();
        while (i < (int)json.length() && (json[i] == ' ' || json[i] == '\n' || json[i] == '\r' || json[i] == '\t')) i++;
        if (fieldBoundary && i < (int)json.length() && json[i] == ':') {
            i++;
            while (i < (int)json.length() && (json[i] == ' ' || json[i] == '\n' || json[i] == '\r' || json[i] == '\t')) i++;
            return i;
        }

        pos = json.indexOf(needle, pos + 1);
    }
    return -1;
}

static bool jsonExtractStringField(const String& json, const char* key, String& out) {
    int i = jsonRawFieldStart(json, key);
    if (i < 0 || i >= (int)json.length() || json[i] != '"') return false;
    i++;
    out = "";

    while (i < (int)json.length()) {
        char ch = json[i++];
        if (ch == '"') return true;
        if (ch != '\\') {
            out += ch;
            continue;
        }
        if (i >= (int)json.length()) return false;
        char esc = json[i++];
        switch (esc) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (i + 4 > (int)json.length()) return false;
                uint32_t cp = 0;
                for (int n = 0; n < 4; n++) {
                    int v = jsonHexNibble(json[i++]);
                    if (v < 0) return false;
                    cp = (cp << 4) | (uint32_t)v;
                }
                appendUtf8Codepoint(out, cp);
                break;
            }
            default:
                return false;
        }
    }
    return false;
}

static int jsonExtractIntField(const String& json, const char* key, int fallback = 0) {
    int i = jsonRawFieldStart(json, key);
    if (i < 0 || i >= (int)json.length()) return fallback;
    bool neg = false;
    if (json[i] == '-') {
        neg = true;
        i++;
    }
    long value = 0;
    bool any = false;
    while (i < (int)json.length() && json[i] >= '0' && json[i] <= '9') {
        any = true;
        value = value * 10 + (json[i++] - '0');
        if (value > INT32_MAX) return fallback;
    }
    if (!any) return fallback;
    return neg ? -(int)value : (int)value;
}

static bool jsonFileSeekFieldValue(File& file, const char* key) {
    if (!file.seek(0, SeekSet)) return false;
    String needle = "\"" + String(key) + "\"";
    int matched = 0;
    while (file.available()) {
        char ch = (char)file.read();
        if (ch == needle[matched]) {
            matched++;
            if (matched == (int)needle.length()) {
                do {
                    if (!file.available()) return false;
                    ch = (char)file.read();
                } while (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t');
                if (ch != ':') {
                    matched = 0;
                    continue;
                }
                do {
                    if (!file.available()) return false;
                    ch = (char)file.peek();
                    if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') file.read();
                } while (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t');
                return true;
            }
        } else {
            matched = (ch == needle[0]) ? 1 : 0;
        }
    }
    return false;
}

static bool jsonExtractStringFieldFromFile(const char* path, const char* key, String& out, size_t maxLen = 1024) {
    File file = SPIFFS.open(path, FILE_READ);
    if (!file) return false;
    if (!jsonFileSeekFieldValue(file, key) || file.read() != '"') {
        file.close();
        return false;
    }
    out = "";

    while (file.available()) {
        char ch = (char)file.read();
        if (ch == '"') {
            file.close();
            return true;
        }
        if (ch != '\\') {
            if (out.length() < maxLen) out += ch;
            continue;
        }
        if (!file.available()) break;
        char esc = (char)file.read();
        char decoded = 0;
        bool hasDecoded = true;
        switch (esc) {
            case '"': decoded = '"'; break;
            case '\\': decoded = '\\'; break;
            case '/': decoded = '/'; break;
            case 'b': decoded = '\b'; break;
            case 'f': decoded = '\f'; break;
            case 'n': decoded = '\n'; break;
            case 'r': decoded = '\r'; break;
            case 't': decoded = '\t'; break;
            case 'u': {
                uint32_t cp = 0;
                for (int n = 0; n < 4; n++) {
                    if (!file.available()) {
                        file.close();
                        return false;
                    }
                    int v = jsonHexNibble((char)file.read());
                    if (v < 0) {
                        file.close();
                        return false;
                    }
                    cp = (cp << 4) | (uint32_t)v;
                }
                if (out.length() < maxLen) appendUtf8Codepoint(out, cp);
                hasDecoded = false;
                break;
            }
            default:
                file.close();
                return false;
        }
        if (hasDecoded && out.length() < maxLen) out += decoded;
    }
    file.close();
    return false;
}

static int jsonExtractIntFieldFromFile(const char* path, const char* key, int fallback = 0) {
    File file = SPIFFS.open(path, FILE_READ);
    if (!file) return fallback;
    if (!jsonFileSeekFieldValue(file, key)) {
        file.close();
        return fallback;
    }
    bool neg = false;
    int ch = file.peek();
    if (ch == '-') {
        neg = true;
        file.read();
    }
    long value = 0;
    bool any = false;
    while (file.available()) {
        ch = file.peek();
        if (ch < '0' || ch > '9') break;
        any = true;
        value = value * 10 + (file.read() - '0');
        if (value > INT32_MAX) {
            file.close();
            return fallback;
        }
    }
    file.close();
    if (!any) return fallback;
    return neg ? -(int)value : (int)value;
}

static bool jsonExtractStringFieldToFile(const char* jsonPath, const char* key, const char* outPath, size_t& written) {
    written = 0;
    File in = SPIFFS.open(jsonPath, FILE_READ);
    if (!in) return false;
    if (!jsonFileSeekFieldValue(in, key) || in.read() != '"') {
        in.close();
        return false;
    }

    SPIFFS.remove(outPath);
    File out = SPIFFS.open(outPath, FILE_WRITE);
    if (!out) {
        in.close();
        return false;
    }

    while (in.available()) {
        char ch = (char)in.read();
        if (ch == '"') {
            out.close();
            in.close();
            return true;
        }
        if (ch != '\\') {
            if (out.write((const uint8_t*)&ch, 1) != 1) break;
            written++;
            continue;
        }
        if (!in.available()) break;
        char esc = (char)in.read();
        char decoded = 0;
        bool writeChar = true;
        switch (esc) {
            case '"': decoded = '"'; break;
            case '\\': decoded = '\\'; break;
            case '/': decoded = '/'; break;
            case 'b': decoded = '\b'; break;
            case 'f': decoded = '\f'; break;
            case 'n': decoded = '\n'; break;
            case 'r': decoded = '\r'; break;
            case 't': decoded = '\t'; break;
            case 'u': {
                uint32_t cp = 0;
                for (int n = 0; n < 4; n++) {
                    if (!in.available()) {
                        out.close();
                        in.close();
                        SPIFFS.remove(outPath);
                        return false;
                    }
                    int v = jsonHexNibble((char)in.read());
                    if (v < 0) {
                        out.close();
                        in.close();
                        SPIFFS.remove(outPath);
                        return false;
                    }
                    cp = (cp << 4) | (uint32_t)v;
                }
                if (!writeUtf8Codepoint(out, cp, written)) {
                    out.close();
                    in.close();
                    SPIFFS.remove(outPath);
                    return false;
                }
                writeChar = false;
                break;
            }
            default:
                out.close();
                in.close();
                SPIFFS.remove(outPath);
                return false;
        }
        if (writeChar) {
            if (out.write((const uint8_t*)&decoded, 1) != 1) break;
            written++;
        }
    }

    out.close();
    in.close();
    SPIFFS.remove(outPath);
    return false;
}

static uint64_t jsonUint64(cJSON* obj, const char* key, uint64_t fallback = 0) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) return (uint64_t)item->valuedouble;
    if (cJSON_IsString(item) && item->valuestring) {
        char* end = nullptr;
        uint64_t value = strtoull(item->valuestring, &end, 10);
        return (end && *end == '\0') ? value : fallback;
    }
    return fallback;
}

static bool jsonBool(cJSON* obj, const char* key, bool fallback = false) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}

static void persistBadgeState() {
    g_prefs.putULong64("onion_id", g_identity.onionId);
    g_prefs.putString("status", g_identity.status);
    g_prefs.putString("username", g_identity.username);
    g_prefs.putString("onions", g_identity.onionCount);
    g_prefs.putBool("linked", g_identity.linked);
}

static bool refreshPublicProfile(bool quiet = false);

static bool updateProfileFromObject(cJSON* obj) {
    if (!cJSON_IsObject(obj)) return false;

    bool changed = false;
    String value;
    const char* usernameKeys[] = {"username", "userName", "linkedUsername", "displayName", "name"};
    for (const char* key : usernameKeys) {
        if (jsonValueString(obj, key, value) && value.length()) {
            if (g_identity.username != value) {
                g_identity.username = value;
                changed = true;
            }
            break;
        }
    }

    // Onion balance. The server sends several balance fields at once — the
    // resolved display value plus its point/token components. Pick the one the
    // portal shows, in priority order, instead of a blind first-match: a linked
    // badge's profile carries BOTH a residual "pointBalance" (e.g. 10) and the
    // real "currentOnionBalance"/token balance (e.g. 2500), and a naive scan
    // that hit pointBalance first displayed the wrong number.
    bool gotBalance = false;
    if (jsonValueString(obj, "currentOnionBalance", value) && value.length()) {
        gotBalance = true;  // server's already-resolved display balance
    } else {
        String balanceType;
        if (jsonValueString(obj, "balanceType", balanceType)) {
            const char* sel = balanceType == "points" ? "currentOnionPoints" : "currentOnionTokens";
            if (jsonValueString(obj, sel, value) && value.length()) gotBalance = true;
        }
    }
    if (!gotBalance) {
        // Legacy / alternate shapes. Token fields precede point fields, and the
        // ambiguous pointBalance sub-component is intentionally excluded.
        const char* onionKeys[] = {
            "onionCount", "onions", "currentOnionTokens", "tokenBalance",
            "onionTokens", "currentOnionPoints", "onionPoints", "points", "balance"
        };
        for (const char* key : onionKeys) {
            if (jsonValueString(obj, key, value) && value.length()) { gotBalance = true; break; }
        }
    }
    if (gotBalance && g_identity.onionCount != value) {
        g_identity.onionCount = value;
        changed = true;
    }

    return changed;
}

static void updateProfileFromJson(cJSON* root) {
    bool changed = updateProfileFromObject(root);
    const char* objectKeys[] = {"profile", "user", "account"};
    for (const char* key : objectKeys) {
        changed = updateProfileFromObject(cJSON_GetObjectItemCaseSensitive(root, key)) || changed;
    }
    if (changed) persistBadgeState();
}

static void subscribeBadgeTopics();

static bool mqttHandshakeResponseMatchesBadge(cJSON* root, const String& incomingTopic, uint64_t onionId) {
    if (!incomingTopic.length()) return true;

    String hardwareId = jsonString(root, "hardwareId");
    if (hardwareId.length()) return hardwareId == g_identity.hardwareId;

    uint64_t topicOnionId = handshakeOnionIdFromTopic(incomingTopic);
    if (g_identity.onionId) {
        return (!topicOnionId || topicOnionId == g_identity.onionId) &&
            (!onionId || onionId == g_identity.onionId);
    }

    bool recentHandshake = g_mqttHandshakePending &&
        millis() - g_mqttHandshakeSentAt <= MQTT_HANDSHAKE_ACCEPT_WINDOW_MS;
    return recentHandshake && topicOnionId && onionId == topicOnionId;
}

static bool handleHandshakeResponse(const String& response, const String& incomingTopic = String()) {
    cJSON* root = cJSON_Parse(response.c_str());
    if (!root) return false;
    uint64_t onionId = jsonUint64(root, "onionId", 0);
    String status = jsonString(root, "status");
    if (!onionId) {
        cJSON_Delete(root);
        return false;
    }
    if (!mqttHandshakeResponseMatchesBadge(root, incomingTopic, onionId)) {
        cJSON_Delete(root);
        return false;
    }

    g_identity.onionId = onionId;
    g_mqttHandshakePending = false;
    if (status.length()) g_identity.status = status;
    g_identity.linked = g_identity.status == "linked";
    updateProfileFromJson(root);
    cJSON_Delete(root);
    persistBadgeState();
    subscribeBadgeTopics();
    setLog("Handshake accepted");
    return true;
}

static void doHttpHandshake() {
    String body = "{\"hardwareId\":\"" + g_identity.hardwareId + "\",\"firmware\":\"onion-os\",\"transport\":\"http\"}";
    String response;
    int code = httpPostJson("/api/badge/handshake", body, &response);
    if (code >= 200 && code < 300 && handleHandshakeResponse(response)) return;
    setLog("HTTP handshake failed " + String(code));
}

static bool publishMqtt(const String& mqttTopic, const String& payload) {
    if (!g_mqtt || !g_mqttConnected) return false;
    return esp_mqtt_client_publish(g_mqtt, mqttTopic.c_str(), payload.c_str(), 0, 1, 0) >= 0;
}

static void doMqttHandshake() {
    String replyTo = topic(String("badge/hardware/") + g_identity.hardwareId + "/handshake/accepted");
    String body = "{\"hardwareId\":\"" + jsonEscape(g_identity.hardwareId) +
        "\",\"firmware\":\"onion-os\",\"transport\":\"mqtt\",\"replyTo\":\"" + jsonEscape(replyTo) + "\"}";
    if (publishMqtt(topic("badge/handshake"), body)) {
        g_mqttHandshakePending = true;
        g_mqttHandshakeSentAt = millis();
    }
}

static void subscribeBadgeTopics() {
    if (!g_mqtt || !g_mqttConnected) return;
    esp_mqtt_client_subscribe(g_mqtt, topic("badge/+/handshake/accepted").c_str(), 1);
    esp_mqtt_client_subscribe(g_mqtt, topic(String("badge/hardware/") + g_identity.hardwareId + "/handshake/accepted").c_str(), 1);

    if (!g_identity.onionId) return;
    String base = "badge/" + String((unsigned long long)g_identity.onionId) + "/";
    esp_mqtt_client_subscribe(g_mqtt, topic(base + "handshake/accepted").c_str(), 1);
    esp_mqtt_client_subscribe(g_mqtt, topic(base + "link/request").c_str(), 1);
    esp_mqtt_client_subscribe(g_mqtt, topic(base + "transaction/request").c_str(), 1);
    esp_mqtt_client_subscribe(g_mqtt, topic(base + "lua/request").c_str(), 1);
}

static void handleLinkRequest(cJSON* root) {
    g_luaDisplayActive = false;
    g_forceFullRefresh = true;
    g_linkPrompt.requestId = jsonString(root, "requestId");
    g_linkPrompt.username = jsonString(root, "username");
    g_linkPrompt.active = true;
    g_screen = SCREEN_LINK_PROMPT;
    setLog("Link request received");
}

static void handleTransactionRequest(cJSON* root) {
    g_luaDisplayActive = false;
    g_forceFullRefresh = true;
    g_txPrompt.operationId = jsonString(root, "operationId");
    g_txPrompt.requestId = jsonString(root, "requestId");
    g_txPrompt.type = jsonString(root, "type");
    g_txPrompt.amount = jsonInt(root, "amount", 0);
    g_txPrompt.transactionBase64 = jsonString(root, "transaction");
    g_txPrompt.active = true;
    g_screen = SCREEN_TX_PROMPT;
    setLog("Transaction request received");
}

static bool handleLuaRequestJson(const String& payload) {
    g_luaDisplayActive = false;
    g_forceFullRefresh = true;

    g_luaPrompt.requestId = "";
    g_luaPrompt.scriptId = "";
    g_luaPrompt.title = "";
    g_luaPrompt.fileName = "";
    g_luaPrompt.description = "";
    g_luaPrompt.authorUsername = "";
    g_luaPrompt.downloadUrl = "";
    g_luaPrompt.code = "";
    g_luaPrompt.codePath = "";
    g_luaPrompt.sizeBytes = jsonExtractIntField(payload, "sizeBytes", 0);
    if (g_luaPrompt.sizeBytes > 0) g_luaPrompt.code.reserve((unsigned int)g_luaPrompt.sizeBytes + 1);

    jsonExtractStringField(payload, "requestId", g_luaPrompt.requestId);
    jsonExtractStringField(payload, "scriptId", g_luaPrompt.scriptId);
    jsonExtractStringField(payload, "title", g_luaPrompt.title);
    jsonExtractStringField(payload, "fileName", g_luaPrompt.fileName);
    jsonExtractStringField(payload, "description", g_luaPrompt.description);
    jsonExtractStringField(payload, "authorUsername", g_luaPrompt.authorUsername);
    jsonExtractStringField(payload, "downloadUrl", g_luaPrompt.downloadUrl);
    jsonExtractStringField(payload, "url", g_luaPrompt.downloadUrl);
    if (!jsonExtractStringField(payload, "code", g_luaPrompt.code) && !g_luaPrompt.downloadUrl.length()) return false;
    if (g_luaPrompt.sizeBytes <= 0) g_luaPrompt.sizeBytes = g_luaPrompt.code.length();

    g_luaPrompt.active = true;
    g_screen = SCREEN_LUA_PROMPT;
    setLog("Lua push received");
    Serial.printf("[onion-os] Lua push: fileName=%s scriptId=%s codeLen=%d jsonLen=%d\n",
        g_luaPrompt.fileName.c_str(), g_luaPrompt.scriptId.c_str(),
        g_luaPrompt.code.length(), payload.length());
    return true;
}

static bool handleLuaRequestJsonFile(const char* payloadPath) {
    g_luaDisplayActive = false;
    g_forceFullRefresh = true;

    g_luaPrompt.requestId = "";
    g_luaPrompt.scriptId = "";
    g_luaPrompt.title = "";
    g_luaPrompt.fileName = "";
    g_luaPrompt.description = "";
    g_luaPrompt.authorUsername = "";
    g_luaPrompt.downloadUrl = "";
    g_luaPrompt.code = "";
    g_luaPrompt.codePath = "";
    g_luaPrompt.sizeBytes = jsonExtractIntFieldFromFile(payloadPath, "sizeBytes", 0);

    jsonExtractStringFieldFromFile(payloadPath, "requestId", g_luaPrompt.requestId, 128);
    jsonExtractStringFieldFromFile(payloadPath, "scriptId", g_luaPrompt.scriptId, 128);
    jsonExtractStringFieldFromFile(payloadPath, "title", g_luaPrompt.title, 96);
    jsonExtractStringFieldFromFile(payloadPath, "fileName", g_luaPrompt.fileName, 96);
    jsonExtractStringFieldFromFile(payloadPath, "description", g_luaPrompt.description, 500);
    jsonExtractStringFieldFromFile(payloadPath, "authorUsername", g_luaPrompt.authorUsername, 64);
    jsonExtractStringFieldFromFile(payloadPath, "downloadUrl", g_luaPrompt.downloadUrl, 256);
    if (!g_luaPrompt.downloadUrl.length()) jsonExtractStringFieldFromFile(payloadPath, "url", g_luaPrompt.downloadUrl, 256);

    size_t codeBytes = 0;
    bool hasCode = jsonExtractStringFieldToFile(payloadPath, "code", LUA_PUSH_TEMP_PATH, codeBytes);
    if (!hasCode && !g_luaPrompt.downloadUrl.length()) return false;
    if (g_luaPrompt.sizeBytes <= 0) g_luaPrompt.sizeBytes = (int)codeBytes;
    g_luaPrompt.codePath = hasCode ? LUA_PUSH_TEMP_PATH : "";
    g_luaPrompt.active = true;
    g_screen = SCREEN_LUA_PROMPT;
    setLog("Lua push received");
    File payloadFile = SPIFFS.open(payloadPath, FILE_READ);
    int payloadLen = payloadFile ? payloadFile.size() : 0;
    if (payloadFile) payloadFile.close();
    Serial.printf("[onion-os] Lua push file-backed: fileName=%s scriptId=%s codeLen=%d jsonLen=%d\n",
        g_luaPrompt.fileName.c_str(), g_luaPrompt.scriptId.c_str(),
        (int)codeBytes, payloadLen);
    return true;
}

static void handleMqttPayload(const String& incomingTopic, const String& payload) {
    bool isControlTopic = incomingTopic.endsWith("/handshake/accepted") ||
        incomingTopic.endsWith("/link/request") ||
        incomingTopic.endsWith("/transaction/request") ||
        incomingTopic.endsWith("/lua/request");
    if (!isControlTopic) return;

    if (incomingTopic.endsWith("/lua/request")) {
        if (!handleLuaRequestJson(payload)) setLog("Bad MQTT JSON");
        return;
    }

    cJSON* root = cJSON_Parse(payload.c_str());
    if (!root) {
        setLog("Bad MQTT JSON");
        return;
    }

    if (incomingTopic.endsWith("/handshake/accepted")) {
        if (!handleHandshakeResponse(payload, incomingTopic)) {
            Serial.printf("[onion-os] ignored MQTT handshake on %s\n", incomingTopic.c_str());
        }
    } else if (incomingTopic.endsWith("/link/request")) {
        handleLinkRequest(root);
    } else if (incomingTopic.endsWith("/transaction/request")) {
        handleTransactionRequest(root);
    }

    cJSON_Delete(root);
}

// Standard MQTT topic-filter matching with '+' (single level) and '#'
// (multi-level, trailing) wildcards. Does not match a '#' filter against the
// parent level (e.g. "a/#" does not match "a"), which is fine for Lua use.
static bool mqttTopicMatches(const char* filter, const char* topic) {
    if (*filter == '#') return true;
    if (*filter == '+') {
        const char* f = filter + 1;
        const char* t = topic;
        while (*t && *t != '/') t++;
        if (*f == '\0') return *t == '\0';
        if (*f != '/' || *t != '/') return false;
        return mqttTopicMatches(f + 1, t + 1);
    }
    if (*topic == '\0') return *filter == '\0';
    if (*filter != *topic) return false;
    return mqttTopicMatches(filter + 1, topic + 1);
}

// Drop every Lua MQTT subscription/queued message and unsubscribe from the
// broker. Called when a Lua script ends so a fresh script starts clean and the
// bounded subscription list cannot leak across runs.
static void luaMqttResetSubs() {
    char subs[ONION_LUA_MQTT_MAX_SUBS][ONION_LUA_MQTT_MAX_TOPIC + 1];
    uint8_t count;
    portENTER_CRITICAL(&g_luaMqttMux);
    count = g_luaMqttSubCount;
    for (uint8_t i = 0; i < count; i++) strcpy(subs[i], g_luaMqttSubs[i]);
    g_luaMqttSubCount = 0;
    g_luaMqttQueueHead = 0;
    g_luaMqttQueueCount = 0;
    portEXIT_CRITICAL(&g_luaMqttMux);

    if (g_mqtt && g_mqttConnected) {
        for (uint8_t i = 0; i < count; i++) esp_mqtt_client_unsubscribe(g_mqtt, subs[i]);
    }
}

static bool luaMqttQueuePop(MqttQueuedMessage& out) {
    bool hasMessage = false;
    portENTER_CRITICAL(&g_luaMqttMux);
    if (g_luaMqttQueueCount > 0) {
        out = g_luaMqttQueue[g_luaMqttQueueHead];
        g_luaMqttQueueHead = (g_luaMqttQueueHead + 1) % ONION_LUA_MQTT_QUEUE_LEN;
        g_luaMqttQueueCount--;
        hasMessage = true;
    }
    portEXIT_CRITICAL(&g_luaMqttMux);
    return hasMessage;
}

// Called from the MQTT task for every inbound message. Enqueues a copy only if
// the topic matches a Lua subscription, dropping the oldest entry when full.
static void luaMqttMaybeQueue(const char* topic, uint16_t topicLen, const char* payload, uint16_t payloadLen) {
    if (topicLen > ONION_LUA_MQTT_MAX_TOPIC) return;
    if (payloadLen > ONION_LUA_MQTT_MAX_PAYLOAD) payloadLen = ONION_LUA_MQTT_MAX_PAYLOAD;

    char topicCopy[ONION_LUA_MQTT_MAX_TOPIC + 1];
    memcpy(topicCopy, topic, topicLen);
    topicCopy[topicLen] = '\0';

    portENTER_CRITICAL(&g_luaMqttMux);
    bool matched = false;
    for (uint8_t i = 0; i < g_luaMqttSubCount; i++) {
        if (mqttTopicMatches(g_luaMqttSubs[i], topicCopy)) { matched = true; break; }
    }
    if (matched) {
        uint8_t slot = (g_luaMqttQueueHead + g_luaMqttQueueCount) % ONION_LUA_MQTT_QUEUE_LEN;
        if (g_luaMqttQueueCount == ONION_LUA_MQTT_QUEUE_LEN) {
            slot = g_luaMqttQueueHead;
            g_luaMqttQueueHead = (g_luaMqttQueueHead + 1) % ONION_LUA_MQTT_QUEUE_LEN;
        } else {
            g_luaMqttQueueCount++;
        }
        MqttQueuedMessage& msg = g_luaMqttQueue[slot];
        memcpy(msg.topic, topicCopy, topicLen);
        msg.topic[topicLen] = '\0';
        msg.topicLen = topicLen;
        memcpy(msg.payload, payload, payloadLen);
        msg.payload[payloadLen] = '\0';
        msg.payloadLen = payloadLen;
        msg.receivedAt = millis();
    }
    portEXIT_CRITICAL(&g_luaMqttMux);
}

static void processMqttMessage(const String& incomingTopic, const String& payload) {
    uint16_t topicLen = incomingTopic.length() > 65535 ? 65535 : (uint16_t)incomingTopic.length();
    uint16_t payloadLen = payload.length() > 65535 ? 65535 : (uint16_t)payload.length();
    luaMqttMaybeQueue(incomingTopic.c_str(), topicLen, payload.c_str(), payloadLen);
    handleMqttPayload(incomingTopic, payload);
}

static void processMqttMessageFile(const String& incomingTopic, const char* payloadPath) {
    if (incomingTopic.endsWith("/lua/request")) {
        if (!handleLuaRequestJsonFile(payloadPath)) setLog("Bad MQTT JSON");
        return;
    }

    File file = SPIFFS.open(payloadPath, FILE_READ);
    if (!file) {
        setLog("Bad MQTT JSON");
        return;
    }
    String payload;
    if (!payload.reserve(file.size() + 1)) {
        file.close();
        setLog("MQTT alloc failed");
        return;
    }
    while (file.available()) payload += (char)file.read();
    file.close();
    processMqttMessage(incomingTopic, payload);
}

static void resetMqttFragmentBuffer() {
    g_mqttRxTopic = "";
    if (g_mqttRxFile) g_mqttRxFile.close();
    if (g_mqttRxPresent) {
        free(g_mqttRxPresent);
        g_mqttRxPresent = nullptr;
    }
    SPIFFS.remove(MQTT_RX_TEMP_JSON_PATH);
    g_mqttRxExpectedLen = 0;
    g_mqttRxReceivedLen = 0;
    g_mqttRxPresentBytes = 0;
    g_mqttRxMsgId = 0;
}

static bool mqttFragmentBytePresent(int idx) {
    return (g_mqttRxPresent[(size_t)idx >> 3] & (1 << (idx & 7))) != 0;
}

static void markMqttFragmentBytePresent(int idx) {
    g_mqttRxPresent[(size_t)idx >> 3] |= (1 << (idx & 7));
}

static bool beginMqttFragmentBuffer(const String& incomingTopic, int totalLen, int msgId) {
    resetMqttFragmentBuffer();
    g_mqttRxTopic = incomingTopic;
    g_mqttRxExpectedLen = totalLen;
    g_mqttRxReceivedLen = 0;
    g_mqttRxMsgId = msgId;

    g_mqttRxPresentBytes = ((size_t)totalLen + 7) / 8;
    g_mqttRxPresent = (uint8_t*)heap_caps_calloc(g_mqttRxPresentBytes, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_mqttRxPresent) g_mqttRxPresent = (uint8_t*)heap_caps_calloc(g_mqttRxPresentBytes, 1, MALLOC_CAP_8BIT);
    SPIFFS.remove(MQTT_RX_TEMP_JSON_PATH);
    g_mqttRxFile = SPIFFS.open(MQTT_RX_TEMP_JSON_PATH, FILE_WRITE);

    if (!g_mqttRxFile || !g_mqttRxPresent) {
        resetMqttFragmentBuffer();
        setLog("MQTT alloc failed");
        return false;
    }
    return true;
}

static bool ensureMqttRxFileSize(size_t size) {
    if (!g_mqttRxFile) return false;
    size_t current = g_mqttRxFile.size();
    if (current >= size) return true;
    if (!g_mqttRxFile.seek(current, SeekSet)) return false;

    uint8_t zeros[64] = {};
    while (current < size) {
        size_t n = size - current;
        if (n > sizeof(zeros)) n = sizeof(zeros);
        if (g_mqttRxFile.write(zeros, n) != n) return false;
        current += n;
    }
    return true;
}

static void processMqttEventData(esp_mqtt_event_handle_t event) {
    String incomingTopic = (event->topic && event->topic_len > 0)
        ? String(event->topic, event->topic_len)
        : String();
    String payloadChunk = (event->data && event->data_len > 0)
        ? String(event->data, event->data_len)
        : String();

    int totalLen = event->total_data_len > 0 ? event->total_data_len : event->data_len;
    int offset = event->current_data_offset;
    if (offset < 0 || event->data_len < 0 || totalLen < event->data_len) {
        resetMqttFragmentBuffer();
        setLog("Bad MQTT fragment");
        Serial.printf("[onion-os] Bad MQTT fragment: msg=%d off=%d len=%d total=%d got=%d\n",
            event->msg_id, offset, event->data_len, totalLen, g_mqttRxReceivedLen);
        return;
    }
    if (totalLen > MQTT_RX_MAX_BYTES) {
        resetMqttFragmentBuffer();
        setLog("MQTT payload too large");
        return;
    }

    bool fragmented = totalLen > event->data_len || offset > 0;
    if (!fragmented) {
        processMqttMessage(incomingTopic, payloadChunk);
        return;
    }

    if (offset == 0) {
        if (!beginMqttFragmentBuffer(incomingTopic, totalLen, event->msg_id)) return;
    } else if (!g_mqttRxExpectedLen || (g_mqttRxMsgId && event->msg_id && event->msg_id != g_mqttRxMsgId)) {
        resetMqttFragmentBuffer();
        setLog("Bad MQTT fragment");
        Serial.printf("[onion-os] Bad MQTT fragment: msg=%d off=%d len=%d total=%d got=%d\n",
            event->msg_id, offset, event->data_len, totalLen, g_mqttRxReceivedLen);
        return;
    } else if (incomingTopic.length() && g_mqttRxTopic.length() && incomingTopic != g_mqttRxTopic) {
        resetMqttFragmentBuffer();
        setLog("Bad MQTT fragment");
        Serial.printf("[onion-os] Bad MQTT fragment topic: msg=%d off=%d len=%d total=%d got=%d\n",
            event->msg_id, offset, event->data_len, totalLen, g_mqttRxReceivedLen);
        return;
    }

    if (offset + event->data_len > g_mqttRxExpectedLen) {
        resetMqttFragmentBuffer();
        setLog("Bad MQTT fragment");
        Serial.printf("[onion-os] Bad MQTT fragment overflow: msg=%d off=%d len=%d total=%d got=%d\n",
            event->msg_id, offset, event->data_len, totalLen, g_mqttRxReceivedLen);
        return;
    }

    if (!ensureMqttRxFileSize((size_t)offset) ||
        !g_mqttRxFile.seek(offset, SeekSet) ||
        g_mqttRxFile.write((const uint8_t*)event->data, event->data_len) != (size_t)event->data_len) {
        resetMqttFragmentBuffer();
        setLog("MQTT write failed");
        return;
    }
    for (int i = 0; i < event->data_len; i++) {
        int idx = offset + i;
        if (!mqttFragmentBytePresent(idx)) {
            markMqttFragmentBytePresent(idx);
            g_mqttRxReceivedLen++;
        }
    }

    if (g_mqttRxReceivedLen < g_mqttRxExpectedLen) return;

    if (g_mqttRxFile) g_mqttRxFile.close();
    processMqttMessageFile(g_mqttRxTopic, MQTT_RX_TEMP_JSON_PATH);
    resetMqttFragmentBuffer();
}

static void mqttEventHandler(void*, esp_event_base_t, int32_t eventId, void* eventData) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)eventData;
    switch ((esp_mqtt_event_id_t)eventId) {
    case MQTT_EVENT_CONNECTED:
        g_mqttConnected = true;
        setLog("MQTT connected");
        subscribeBadgeTopics();
        doMqttHandshake();
        break;
    case MQTT_EVENT_DISCONNECTED:
        g_mqttConnected = false;
        setLog("MQTT disconnected");
        break;
    case MQTT_EVENT_DATA: {
        processMqttEventData(event);
        break;
    }
    default:
        break;
    }
}

static void ensureMqtt() {
    if (g_mqttConnected || !g_config.mqttUri.length() || WiFi.status() != WL_CONNECTED) return;
    if (millis() - g_lastMqttAttempt < MQTT_RECONNECT_INTERVAL_MS) return;
    g_lastMqttAttempt = millis();

    if (!g_mqtt) {
        esp_mqtt_client_config_t cfg = {};
        cfg.broker.address.uri = g_config.mqttUri.c_str();
        cfg.credentials.username = g_config.mqttUsername.length() ? g_config.mqttUsername.c_str() : nullptr;
        cfg.credentials.authentication.password = g_config.mqttPassword.length() ? g_config.mqttPassword.c_str() : nullptr;
        cfg.buffer.size = MQTT_CLIENT_BUFFER_BYTES;
        cfg.buffer.out_size = MQTT_CLIENT_OUT_BUFFER_BYTES;
        g_mqtt = esp_mqtt_client_init(&cfg);
        esp_mqtt_client_register_event(g_mqtt, MQTT_EVENT_ANY, mqttEventHandler, nullptr);
        esp_mqtt_client_start(g_mqtt);
    } else {
        esp_mqtt_client_reconnect(g_mqtt);
    }
}

static bool sendLinkResponse(bool approved) {
    if (!g_identity.onionId) {
        setLog("No Onion ID yet");
        return false;
    }

    String attestation;
    if (approved) {
        String keyError;
        if (!loadOrCreateSolanaKey(false, keyError)) {
            setLog(keyError);
            return false;
        }
        String subject = String((unsigned long long)g_identity.onionId) + ":" + g_linkPrompt.requestId + ":" + g_linkPrompt.username;
        if (!createAteccAttestation("link", subject, attestation, keyError)) {
            setLog(keyError);
            return false;
        }
    }

    String body = "{\"onionId\":" + String((unsigned long long)g_identity.onionId) +
        ",\"approved\":" + String(approved ? "true" : "false") +
        ",\"solanaPublicKey\":\"" + jsonEscape(g_identity.solanaPublicKey) + "\"";
    if (g_linkPrompt.requestId.length()) {
        body += ",\"requestId\":\"" + jsonEscape(g_linkPrompt.requestId) + "\"";
    }
    if (attestation.length()) body += ",\"attestation\":" + attestation;
    body += "}";

    bool sentOverMqtt = publishMqtt(topic("badge/" + String((unsigned long long)g_identity.onionId) + "/link/response"), body);
    String response;
    int code = sentOverMqtt ? 200 : httpPostJson("/api/badge/link-response", body, &response);
    if (code >= 200 && code < 300) {
        g_identity.linked = approved;
        g_identity.status = approved ? "linked" : "seen";
        if (approved && g_linkPrompt.username.length()) g_identity.username = g_linkPrompt.username;
        persistBadgeState();
        setLog(approved ? "Link approved" : "Link denied");
        if (approved) refreshPublicProfile();
    } else {
        setLog("Link response HTTP " + String(code));
    }

    g_linkPrompt.active = false;
    g_screen = SCREEN_STATUS;
    return code >= 200 && code < 300;
}

static bool signSolanaTransaction(const String& transactionBase64, String& signedTransaction, String& error) {
    if (!loadOrCreateSolanaKey(false, error)) return false;

    uint8_t seed[SOLANA_SEED_LEN];
    uint8_t pubkey[SOLANA_PUBKEY_LEN];
    uint8_t secret[SOLANA_SECRET_KEY_LEN];
    if (!decryptSolanaSeed(seed, error)) return false;
    crypto_sign_seed_keypair(pubkey, secret, seed);
    sodium_memzero(seed, sizeof(seed));

    std::vector<uint8_t> tx;
    if (!base64Decode(transactionBase64, tx)) {
        sodium_memzero(secret, sizeof(secret));
        error = "Bad transaction base64";
        return false;
    }

    size_t offset = 0;
    size_t sigCount = 0;
    if (!readSolanaShortVec(tx, offset, sigCount) || sigCount == 0) {
        sodium_memzero(secret, sizeof(secret));
        error = "Bad Solana signatures";
        return false;
    }
    size_t sigStart = offset;
    size_t messageStart = sigStart + sigCount * SOLANA_SIGNATURE_LEN;
    if (messageStart + 3 > tx.size()) {
        sodium_memzero(secret, sizeof(secret));
        error = "Solana tx too short";
        return false;
    }

    uint8_t requiredSigners = tx[messageStart];
    if (requiredSigners == 0 || requiredSigners > sigCount) {
        sodium_memzero(secret, sizeof(secret));
        error = "Badge is not required signer";
        return false;
    }

    size_t messageOffset = messageStart + 3;
    size_t accountCount = 0;
    if (!readSolanaShortVec(tx, messageOffset, accountCount)) {
        sodium_memzero(secret, sizeof(secret));
        error = "Bad account vector";
        return false;
    }
    if (messageOffset + accountCount * SOLANA_PUBKEY_LEN > tx.size() || accountCount < requiredSigners) {
        sodium_memzero(secret, sizeof(secret));
        error = "Bad account keys";
        return false;
    }

    int signerIndex = -1;
    for (size_t i = 0; i < requiredSigners; ++i) {
        const uint8_t* accountKey = tx.data() + messageOffset + i * SOLANA_PUBKEY_LEN;
        if (memcmp(accountKey, pubkey, SOLANA_PUBKEY_LEN) == 0) {
            signerIndex = (int)i;
            break;
        }
    }
    if (signerIndex < 0) {
        sodium_memzero(secret, sizeof(secret));
        error = "Wrong signer wallet";
        return false;
    }

    uint8_t* signatureOut = tx.data() + sigStart + signerIndex * SOLANA_SIGNATURE_LEN;
    if (crypto_sign_detached(signatureOut, nullptr, tx.data() + messageStart, tx.size() - messageStart, secret) != 0) {
        sodium_memzero(secret, sizeof(secret));
        error = "Ed25519 signing failed";
        return false;
    }
    sodium_memzero(secret, sizeof(secret));

    signedTransaction = base64Encode(tx.data(), tx.size());
    if (!signedTransaction.length()) {
        error = "Signed tx encode failed";
        return false;
    }
    return true;
}

static bool sendTransactionResponse(bool approved) {
    if (!g_identity.onionId || !g_txPrompt.operationId.length()) {
        setLog("No transaction active");
        return false;
    }

    String signedTx;
    String signError;
    String attestation;
    if (approved && !signSolanaTransaction(g_txPrompt.transactionBase64, signedTx, signError)) {
        setLog(signError);
        return false;
    }
    if (approved) {
        String subject = g_txPrompt.operationId + ":" + g_txPrompt.requestId + ":" + g_txPrompt.type + ":" + String(g_txPrompt.amount);
        if (!createAteccAttestation("transaction", subject, attestation, signError)) {
            setLog(signError);
            return false;
        }
    }

    String body = "{\"onionId\":" + String((unsigned long long)g_identity.onionId) +
        ",\"operationId\":\"" + jsonEscape(g_txPrompt.operationId) +
        "\",\"approved\":" + String(approved ? "true" : "false") +
        ",\"signedTransaction\":\"" + jsonEscape(signedTx) + "\"";
    if (g_txPrompt.requestId.length()) {
        body += ",\"requestId\":\"" + jsonEscape(g_txPrompt.requestId) + "\"";
    }
    if (attestation.length()) body += ",\"attestation\":" + attestation;
    body += "}";

    bool sentOverMqtt = publishMqtt(topic("badge/" + String((unsigned long long)g_identity.onionId) + "/transaction/response"), body);
    String response;
    int code = sentOverMqtt ? 200 : httpPostJson("/api/badge/transaction-response", body, &response);
    if (code >= 200 && code < 300) {
        setLog(approved ? "Transaction approved" : "Transaction denied");
        g_txPrompt.active = false;
        g_screen = SCREEN_STATUS;
    } else {
        setLog("Txn response HTTP " + String(code));
    }
    return code >= 200 && code < 300;
}

static int httpGetString(const String& url, String* response) {
    if (!ensureWifi()) return false;
    String responseBuffer;
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.event_handler = httpCaptureEvent;
    cfg.user_data = &responseBuffer;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;
    if (g_config.badgeApiKey.length()) {
        String auth = "Bearer " + g_config.badgeApiKey;
        esp_http_client_set_header(client, "Authorization", auth.c_str());
    }
    esp_err_t err = esp_http_client_perform(client);
    int code = err == ESP_OK ? esp_http_client_get_status_code(client) : -1;
    if (response) *response = responseBuffer;
    esp_http_client_cleanup(client);
    return code;
}

static String urlEncode(const String& value) {
    static const char* hex = "0123456789ABCDEF";
    String out;
    out.reserve(value.length() + 8);
    for (size_t i = 0; i < value.length(); ++i) {
        uint8_t ch = (uint8_t)value[i];
        bool safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~';
        if (safe) {
            out += (char)ch;
        } else {
            out += '%';
            out += hex[ch >> 4];
            out += hex[ch & 0x0F];
        }
    }
    return out;
}

// Fetches the attendee profile (Onion balance, wallet) and updates the badge.
// quiet=true is for the periodic background refresh: it skips the status-line
// log on every path and only forces a redraw when the balance actually
// changed, so an idle badge doesn't flicker its e-paper every interval.
static bool refreshPublicProfile(bool quiet) {
    if (!g_identity.onionId && !g_identity.username.length()) {
        if (!quiet) setLog("No profile identity");
        return false;
    }

    String base = g_config.serverBaseUrl;
    if (base.endsWith("/")) base.remove(base.length() - 1);
    String response;
    int code = g_identity.onionId
        ? httpGetString(base + "/api/badge/profile/" + String((unsigned long long)g_identity.onionId), &response)
        : -1;
    if ((code < 200 || code >= 300) && g_identity.username.length()) {
        code = httpGetString(base + "/api/public/profile/" + urlEncode(g_identity.username), &response);
    }
    if (code < 200 || code >= 300) {
        if (!quiet) setLog("Profile GET failed " + String(code));
        return false;
    }

    cJSON* root = cJSON_Parse(response.c_str());
    if (!root) {
        if (!quiet) setLog("Bad profile JSON");
        return false;
    }
    String beforeCount = g_identity.onionCount;
    updateProfileFromJson(root);
    String wallet = jsonString(root, "solanaWalletAddress");
    if (wallet.length() && wallet != g_identity.solanaPublicKey) {
        g_identity.solanaPublicKey = wallet;
        g_prefs.putString("sol_pub", g_identity.solanaPublicKey);
    }
    cJSON_Delete(root);
    if (quiet) {
        if (g_identity.onionCount != beforeCount && !g_luaDisplayActive) g_needsRedraw = true;
    } else {
        setLog("Profile refreshed");
    }
    return true;
}

static bool downloadFile(const String& url, const String& path, size_t maxBytes, const String& label) {
    if (!ensureWifi()) return false;
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;
    if (g_config.badgeApiKey.length()) {
        String auth = "Bearer " + g_config.badgeApiKey;
        esp_http_client_set_header(client, "Authorization", auth.c_str());
    }
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        setLog(label + " open failed");
        return false;
    }
    int contentLength = esp_http_client_fetch_headers(client);
    int code = esp_http_client_get_status_code(client);
    if (code < 200 || code >= 300) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        setLog(label + " GET failed " + String(code));
        return false;
    }
    if (contentLength > 0 && (size_t)contentLength > maxBytes) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        setLog(label + " too large");
        return false;
    }

    File file = SPIFFS.open(path, FILE_WRITE);
    if (!file) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        setLog(label + " open failed");
        return false;
    }

    uint8_t buf[256];
    size_t written = 0;
    while (true) {
        int read = esp_http_client_read(client, reinterpret_cast<char*>(buf), sizeof(buf));
        if (read < 0) {
            file.close();
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            SPIFFS.remove(path);
            setLog(label + " read failed");
            return false;
        }
        if (read == 0) break;
        file.write(buf, (size_t)read);
        written += read;
        if (written > maxBytes) {
            file.close();
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            SPIFFS.remove(path);
            setLog(label + " too large");
            return false;
        }
    }

    file.close();
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return true;
}

static bool downloadScriptFile(const String& url, const String& path) {
    return downloadFile(url, path, MAX_SCRIPT_BYTES, "Script");
}

static bool downloadImageFile(const String& url, const String& path) {
    return downloadFile(url, path, MAX_IMAGE_BYTES, "Image");
}

static String resolveServerUrl(const String& url) {
    if (url.startsWith("http://") || url.startsWith("https://")) return url;
    String base = g_config.serverBaseUrl;
    if (base.endsWith("/")) base.remove(base.length() - 1);
    if (url.startsWith("/")) return base + url;
    return base + "/" + url;
}

struct LoadedBitmap {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bits;
};

static void setBitmapPixel(LoadedBitmap& bitmap, int x, int y, bool black) {
    if (!black || x < 0 || y < 0 || x >= bitmap.width || y >= bitmap.height) return;
    size_t rowBytes = (bitmap.width + 7) / 8;
    bitmap.bits[y * rowBytes + x / 8] |= 0x80 >> (x & 7);
}

static uint16_t readLe16(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + 2 > data.size()) return 0;
    return (uint16_t)data[offset] | ((uint16_t)data[offset + 1] << 8);
}

static uint32_t readLe32(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + 4 > data.size()) return 0;
    return (uint32_t)data[offset] | ((uint32_t)data[offset + 1] << 8) |
        ((uint32_t)data[offset + 2] << 16) | ((uint32_t)data[offset + 3] << 24);
}

static int32_t readLeS32(const std::vector<uint8_t>& data, size_t offset) {
    return (int32_t)readLe32(data, offset);
}

static bool isBlackRgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint16_t)r * 30 + (uint16_t)g * 59 + (uint16_t)b * 11) < 12800;
}

static bool loadFileBytes(const String& path, std::vector<uint8_t>& bytes, size_t maxBytes, String& error) {
    File file = SPIFFS.open(path, FILE_READ);
    if (!file) {
        error = "Image missing";
        return false;
    }
    size_t size = file.size();
    if (!size || size > maxBytes) {
        file.close();
        error = "Image too large";
        return false;
    }
    bytes.assign(size, 0);
    size_t read = file.read(bytes.data(), bytes.size());
    file.close();
    if (read != size) {
        error = "Image read failed";
        return false;
    }
    return true;
}

static bool readPbmToken(const std::vector<uint8_t>& data, size_t& offset, String& token) {
    token = "";
    while (offset < data.size()) {
        char ch = (char)data[offset];
        if (ch == '#') {
            while (offset < data.size() && data[offset] != '\n') offset++;
        } else if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            offset++;
        } else {
            break;
        }
    }
    while (offset < data.size()) {
        char ch = (char)data[offset];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '#') break;
        token += ch;
        offset++;
    }
    return token.length() > 0;
}

static bool loadPbmBitmap(const std::vector<uint8_t>& data, LoadedBitmap& bitmap, String& error) {
    size_t offset = 0;
    String magic;
    String widthToken;
    String heightToken;
    if (!readPbmToken(data, offset, magic) || !readPbmToken(data, offset, widthToken) ||
        !readPbmToken(data, offset, heightToken)) {
        error = "Bad PBM header";
        return false;
    }

    bitmap.width = widthToken.toInt();
    bitmap.height = heightToken.toInt();
    if (bitmap.width <= 0 || bitmap.height <= 0 ||
        bitmap.width > display.width() || bitmap.height > display.height()) {
        error = "PBM size unsupported";
        return false;
    }

    size_t rowBytes = (bitmap.width + 7) / 8;
    bitmap.bits.assign(rowBytes * bitmap.height, 0);

    if (magic == "P4") {
        if (offset < data.size() &&
            (data[offset] == ' ' || data[offset] == '\t' || data[offset] == '\r' || data[offset] == '\n')) offset++;
        size_t needed = rowBytes * bitmap.height;
        if (offset + needed > data.size()) {
            error = "PBM data short";
            return false;
        }
        memcpy(bitmap.bits.data(), data.data() + offset, needed);
        return true;
    }

    if (magic == "P1") {
        String pixel;
        for (int y = 0; y < bitmap.height; ++y) {
            for (int x = 0; x < bitmap.width; ++x) {
                if (!readPbmToken(data, offset, pixel)) {
                    error = "PBM data short";
                    return false;
                }
                setBitmapPixel(bitmap, x, y, pixel == "1");
            }
        }
        return true;
    }

    error = "Unsupported PBM";
    return false;
}

static bool loadBmpBitmap(const std::vector<uint8_t>& data, LoadedBitmap& bitmap, String& error) {
    if (data.size() < 54 || data[0] != 'B' || data[1] != 'M') {
        error = "Bad BMP header";
        return false;
    }

    uint32_t pixelOffset = readLe32(data, 10);
    uint32_t dibSize = readLe32(data, 14);
    int32_t width = readLeS32(data, 18);
    int32_t rawHeight = readLeS32(data, 22);
    uint16_t planes = readLe16(data, 26);
    uint16_t bpp = readLe16(data, 28);
    uint32_t compression = readLe32(data, 30);
    uint32_t colorsUsed = readLe32(data, 46);
    if (dibSize < 40 || width <= 0 || rawHeight == 0 || planes != 1 || compression != 0 ||
        (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32)) {
        error = "Unsupported BMP";
        return false;
    }

    int height = rawHeight < 0 ? -rawHeight : rawHeight;
    bool topDown = rawHeight < 0;
    if (width > display.width() || height > display.height()) {
        error = "BMP size unsupported";
        return false;
    }

    bitmap.width = width;
    bitmap.height = height;
    size_t outRowBytes = (bitmap.width + 7) / 8;
    bitmap.bits.assign(outRowBytes * bitmap.height, 0);

    uint32_t paletteEntries = bpp <= 8 ? (colorsUsed ? colorsUsed : (1UL << bpp)) : 0;
    size_t paletteOffset = 14 + dibSize;
    if (paletteEntries && paletteOffset + paletteEntries * 4 > data.size()) {
        error = "BMP palette bad";
        return false;
    }

    uint32_t rowBytes = ((uint32_t)width * bpp + 31) / 32 * 4;
    if (pixelOffset + rowBytes * height > data.size()) {
        error = "BMP data short";
        return false;
    }

    for (int y = 0; y < height; ++y) {
        int srcY = topDown ? y : (height - 1 - y);
        size_t rowOffset = pixelOffset + rowBytes * srcY;
        for (int x = 0; x < width; ++x) {
            uint8_t r = 255;
            uint8_t g = 255;
            uint8_t b = 255;
            if (bpp == 24 || bpp == 32) {
                size_t pixel = rowOffset + x * (bpp / 8);
                b = data[pixel];
                g = data[pixel + 1];
                r = data[pixel + 2];
            } else {
                uint8_t index = 0;
                if (bpp == 8) {
                    index = data[rowOffset + x];
                } else if (bpp == 4) {
                    uint8_t packed = data[rowOffset + x / 2];
                    index = (x & 1) ? (packed & 0x0F) : (packed >> 4);
                } else {
                    uint8_t packed = data[rowOffset + x / 8];
                    index = (packed >> (7 - (x & 7))) & 0x01;
                }
                if (index >= paletteEntries) {
                    error = "BMP palette index";
                    return false;
                }
                size_t color = paletteOffset + index * 4;
                b = data[color];
                g = data[color + 1];
                r = data[color + 2];
            }
            setBitmapPixel(bitmap, x, y, isBlackRgb(r, g, b));
        }
    }
    return true;
}

static bool loadStoredBitmap(const String& name, LoadedBitmap& bitmap, String& error) {
    String path = imagePathForName(name);
    if (!path.length()) {
        error = "Bad image name";
        return false;
    }

    std::vector<uint8_t> bytes;
    if (!loadFileBytes(path, bytes, MAX_IMAGE_BYTES, error)) return false;
    if (name.endsWith(".pbm")) return loadPbmBitmap(bytes, bitmap, error);
    if (name.endsWith(".bmp")) return loadBmpBitmap(bytes, bitmap, error);
    error = "Unsupported image";
    return false;
}

static uint16_t canvasColor(uint16_t displayColor) {
    return displayColor == GxEPD_BLACK ? 1 : 0;
}

static void refreshLuaCanvas() {
    // Copy lua canvas into the UI framebuffer with bit inversion so flushFrame()
    // can apply the same partial-refresh logic to Lua screens.
    // Conventions differ: g_luaCanvas bit=1=black; g_frame bit=0=black.
    const uint8_t* src = g_luaCanvas.getBuffer();
    uint8_t*       dst = g_frame.getBuffer();
    for (int i = 0; i < FRAME_BYTES; ++i) dst[i] = ~src[i];
    g_luaDisplayActive = true;
    g_needsRedraw = false;
    flushFrame();
}

static void luaFlushOrDefer() {
    if (g_luaDeferFlush) { g_luaFramePending = true; return; }
    refreshLuaCanvas();
}

static void renderBitmap(const LoadedBitmap& bitmap, int x, int y, bool clearScreen) {
    if (clearScreen) g_luaCanvas.fillScreen(0);
    g_luaCanvas.drawBitmap(x, y, bitmap.bits.data(), bitmap.width, bitmap.height, 1);
    luaFlushOrDefer();
}

static bool luaTableBool(lua_State* L, int index, const char* key, bool fallback) {
    if (!lua_istable(L, index)) return fallback;
    lua_getfield(L, index, key);
    bool value = lua_isnil(L, -1) ? fallback : lua_toboolean(L, -1);
    lua_pop(L, 1);
    return value;
}

static int luaTableInt(lua_State* L, int index, const char* key, int fallback) {
    if (!lua_istable(L, index)) return fallback;
    lua_getfield(L, index, key);
    int value = lua_isnumber(L, -1) ? (int)lua_tointeger(L, -1) : fallback;
    lua_pop(L, 1);
    return value;
}

static const char* luaTableString(lua_State* L, int index, const char* key, const char* fallback) {
    if (!lua_istable(L, index)) return fallback;
    lua_getfield(L, index, key);
    const char* value = lua_isstring(L, -1) ? lua_tostring(L, -1) : fallback;
    lua_pop(L, 1);
    return value;
}

static uint16_t displayColorFromName(const char* value, uint16_t fallback) {
    if (!value) return fallback;
    if (strcmp(value, "white") == 0 || strcmp(value, "bg") == 0 || strcmp(value, "background") == 0) {
        return GxEPD_WHITE;
    }
    return GxEPD_BLACK;
}

static const GFXfont* displayFontFromName(const char* value) {
    if (!value || strcmp(value, "small") == 0 || strcmp(value, "regular") == 0) return &FreeMono9pt7b;
    if (strcmp(value, "bold") == 0) return &FreeMonoBold9pt7b;
    if (strcmp(value, "large") == 0 || strcmp(value, "title") == 0) return &FreeMonoBold18pt7b;
    return &FreeMono9pt7b;
}

static bool luaDisplayClearArg(lua_State* L, int index, bool fallback = true) {
    if (lua_gettop(L) < index) return fallback;
    if (lua_istable(L, index)) return luaTableBool(L, index, "clear", fallback);
    return lua_toboolean(L, index);
}

static String macToString(const uint8_t mac[6]) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

static bool parseMacString(const char* value, uint8_t mac[6]) {
    if (!value) return false;
    unsigned int parts[6];
    if (sscanf(value, "%x:%x:%x:%x:%x:%x",
        &parts[0], &parts[1], &parts[2], &parts[3], &parts[4], &parts[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        if (parts[i] > 0xff) return false;
        mac[i] = (uint8_t)parts[i];
    }
    return true;
}

static bool espnowQueuePop(EspNowQueuedMessage& out) {
    bool hasMessage = false;
    portENTER_CRITICAL(&g_espnowMux);
    if (g_espnowQueueCount > 0) {
        out = g_espnowQueue[g_espnowQueueHead];
        g_espnowQueueHead = (g_espnowQueueHead + 1) % ONION_ESPNOW_QUEUE_LEN;
        g_espnowQueueCount--;
        hasMessage = true;
    }
    portEXIT_CRITICAL(&g_espnowMux);
    return hasMessage;
}

static void copyBounded(char* dst, size_t dstLen, const char* src, size_t srcLen) {
    if (!dst || dstLen == 0) return;
    size_t n = 0;
    if (src && srcLen) {
        while (n + 1 < dstLen && n < srcLen && src[n] != '\0') {
            dst[n] = src[n];
            n++;
        }
    }
    dst[n] = '\0';
}

static void copyStringToField(char* dst, size_t dstLen, const String& value) {
    if (!dst || dstLen == 0) return;
    size_t n = value.length();
    if (n >= dstLen) n = dstLen - 1;
    memcpy(dst, value.c_str(), n);
    dst[n] = '\0';
    if (n + 1 < dstLen) memset(dst + n + 1, 0, dstLen - n - 1);
}

static bool checkInHeaderMatches(const uint8_t* data, int len, uint8_t type) {
    if (!data || len < (int)sizeof(CheckInPacketHeader)) return false;
    const CheckInPacketHeader* header = reinterpret_cast<const CheckInPacketHeader*>(data);
    return memcmp(header->magic, kCheckInMagic, sizeof(kCheckInMagic)) == 0 &&
        header->version == kCheckInVersion && header->type == type;
}

static void checkInMaybeCapturePacket(const esp_now_recv_info_t* info, const uint8_t* data, int len, int8_t rssi) {
    if (!info || !info->src_addr || !data) return;

    if (len == (int)sizeof(CheckInAdvertisePacket) &&
        checkInHeaderMatches(data, len, CHECKIN_PACKET_ADVERTISE)) {
        CheckInAdvertisePacket packet;
        memcpy(&packet, data, sizeof(packet));
        int8_t minRssi = packet.minRssi ? packet.minRssi : ONION_CHECKIN_DEFAULT_MIN_RSSI;
        if (rssi < minRssi) return;

        portENTER_CRITICAL(&g_checkinMux);
        memcpy(g_checkinPendingOffer.beaconMac, info->src_addr, 6);
        copyBounded(g_checkinPendingOffer.beaconId, sizeof(g_checkinPendingOffer.beaconId),
            packet.beaconId, sizeof(packet.beaconId));
        copyBounded(g_checkinPendingOffer.room, sizeof(g_checkinPendingOffer.room),
            packet.room, sizeof(packet.room));
        copyBounded(g_checkinPendingOffer.label, sizeof(g_checkinPendingOffer.label),
            packet.label, sizeof(packet.label));
        memcpy(g_checkinPendingOffer.nonce, packet.nonce, sizeof(packet.nonce));
        g_checkinPendingOffer.rssi = rssi;
        g_checkinPendingOffer.minRssi = minRssi;
        g_checkinPendingOffer.seenAt = millis();
        g_checkinOfferPending = true;
        portEXIT_CRITICAL(&g_checkinMux);
    } else if (len == (int)sizeof(CheckInResultPacket) &&
        checkInHeaderMatches(data, len, CHECKIN_PACKET_RESULT)) {
        CheckInResultPacket packet;
        memcpy(&packet, data, sizeof(packet));
        portENTER_CRITICAL(&g_checkinMux);
        copyBounded(g_checkinPendingResult.beaconId, sizeof(g_checkinPendingResult.beaconId),
            packet.beaconId, sizeof(packet.beaconId));
        memcpy(g_checkinPendingResult.nonce, packet.nonce, sizeof(packet.nonce));
        g_checkinPendingResult.awarded = packet.awarded != 0;
        g_checkinPendingResult.points = packet.points;
        copyBounded(g_checkinPendingResult.message, sizeof(g_checkinPendingResult.message),
            packet.message, sizeof(packet.message));
        g_checkinResultPending = true;
        portEXIT_CRITICAL(&g_checkinMux);
    }
}

static void espnowQueuePush(const uint8_t mac[6], const uint8_t* data, int len, int8_t rssi) {
    if (!mac || !data || len <= 0) return;
    if (len > ONION_ESPNOW_MAX_PAYLOAD) len = ONION_ESPNOW_MAX_PAYLOAD;

    portENTER_CRITICAL(&g_espnowMux);
    uint8_t slot = (g_espnowQueueHead + g_espnowQueueCount) % ONION_ESPNOW_QUEUE_LEN;
    if (g_espnowQueueCount == ONION_ESPNOW_QUEUE_LEN) {
        slot = g_espnowQueueHead;
        g_espnowQueueHead = (g_espnowQueueHead + 1) % ONION_ESPNOW_QUEUE_LEN;
    } else {
        g_espnowQueueCount++;
    }

    EspNowQueuedMessage& msg = g_espnowQueue[slot];
    memcpy(msg.mac, mac, sizeof(msg.mac));
    msg.len = (uint8_t)len;
    memcpy(msg.payload, data, len);
    msg.payload[len] = '\0';
    msg.rssi = rssi;
    msg.receivedAt = millis();
    g_espnowReceived++;
    portEXIT_CRITICAL(&g_espnowMux);
}

static void onEspNowReceive(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    int8_t rssi = 0;
    if (info && info->rx_ctrl) rssi = info->rx_ctrl->rssi;
    checkInMaybeCapturePacket(info, data, len, rssi);
    if (info && info->src_addr) espnowQueuePush(info->src_addr, data, len, rssi);
}

static void onEspNowSend(const esp_now_send_info_t*, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) g_espnowSent++;
}

static bool espnowAddPeer(const uint8_t mac[6], uint8_t channel, String& error) {
    if (esp_now_is_peer_exist(mac)) return true;

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = channel;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    esp_err_t rc = esp_now_add_peer(&peer);
    if (rc != ESP_OK) {
        error = "ESP-NOW add peer failed " + String((int)rc);
        return false;
    }
    return true;
}

static bool ensureEspNow(int requestedChannel, String& error) {
    if (requestedChannel < 0 || requestedChannel > 14) {
        error = "Bad ESP-NOW channel";
        return false;
    }

    WiFi.mode(WIFI_STA);

    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&primary, &second);
    if (requestedChannel > 0 && WiFi.status() == WL_CONNECTED && requestedChannel != primary) {
        error = "WiFi connected; ESP-NOW uses AP channel " + String(primary);
        return false;
    }
    if (requestedChannel > 0 && WiFi.status() != WL_CONNECTED) {
        esp_wifi_set_promiscuous(true);
        esp_err_t channelRc = esp_wifi_set_channel((uint8_t)requestedChannel, WIFI_SECOND_CHAN_NONE);
        esp_wifi_set_promiscuous(false);
        if (channelRc != ESP_OK) {
            error = "ESP-NOW channel failed " + String((int)channelRc);
            return false;
        }
    }

    if (!g_espnowStarted) {
        esp_err_t rc = esp_now_init();
        if (rc != ESP_OK) {
            error = "ESP-NOW init failed " + String((int)rc);
            return false;
        }
        esp_now_register_recv_cb(onEspNowReceive);
        esp_now_register_send_cb(onEspNowSend);
        // Fixed PMK so per-peer LMKs (onion.espnow_set_peer_key) interoperate
        // across badges. The PMK only wraps LMKs; broadcast stays plaintext.
        static const uint8_t kEspNowPmk[ESP_NOW_KEY_LEN] = {
            'o', 'n', 'i', 'o', 'n', '-', 'o', 's', '-', 'p', 'm', 'k', '-', '0', '0', '1'
        };
        esp_err_t pmkRc = esp_now_set_pmk(kEspNowPmk);
        if (pmkRc != ESP_OK) {
            Serial.printf("[onion-os] ESP-NOW set_pmk failed %d\n", (int)pmkRc);
        }
        g_espnowStarted = true;
        // Internal-heap visibility: the 128-deep RX queue costs ~32 KB of
        // static internal RAM; watch this line if the queue grows again.
        Serial.printf("[onion-os] ESP-NOW started queue=%d free_internal=%u\n",
                      ONION_ESPNOW_QUEUE_LEN,
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }

    return espnowAddPeer(kEspNowBroadcastMac, 0, error);
}

static bool screenAllowsCheckInPrompt() {
    return g_screen == SCREEN_STATUS && !g_luaDisplayActive &&
        !g_linkPrompt.active && !g_txPrompt.active && !g_luaPrompt.active;
}

static void showCheckInResult(const String& message, bool awarded = false, int points = 0) {
    g_checkinResult.message = message;
    g_checkinResult.awarded = awarded;
    g_checkinResult.points = points;
    g_checkinResult.shownAt = millis();
    g_screen = SCREEN_CHECKIN_RESULT;
    g_forceFullRefresh = true;
    g_needsRedraw = true;
}

static void sendCheckInApproval(bool approve) {
    g_checkinPrompt.active = false;
    g_lastCheckInBeaconId = g_checkinPrompt.beaconId;
    g_lastCheckInPromptAt = millis();

    if (!approve) {
        setLog("Check-in skipped");
        g_screen = SCREEN_STATUS;
        return;
    }

    CheckInApprovePacket packet = {};
    memcpy(packet.header.magic, kCheckInMagic, sizeof(kCheckInMagic));
    packet.header.version = kCheckInVersion;
    packet.header.type = CHECKIN_PACKET_APPROVE;
    copyStringToField(packet.beaconId, sizeof(packet.beaconId), g_checkinPrompt.beaconId);
    memcpy(packet.nonce, g_checkinPrompt.nonce, sizeof(packet.nonce));
    copyStringToField(packet.hardwareId, sizeof(packet.hardwareId), g_identity.hardwareId);
    packet.onionId = g_identity.onionId;
    copyStringToField(packet.username, sizeof(packet.username), g_identity.username);
    copyStringToField(packet.wallet, sizeof(packet.wallet), g_identity.solanaPublicKey);
    packet.rssi = g_checkinPrompt.rssi;
    packet.approvedAt = millis();
    esp_wifi_get_mac(WIFI_IF_STA, packet.badgeMac);

    String error;
    if (!ensureEspNow(0, error) || !espnowAddPeer(g_checkinPrompt.beaconMac, 0, error)) {
        showCheckInResult("Radio failed: " + clipped(error, 15));
        return;
    }

    esp_err_t rc = esp_now_send(g_checkinPrompt.beaconMac,
        reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
    if (rc != ESP_OK) {
        showCheckInResult("Send failed " + String((int)rc));
        return;
    }

    setLog("Check-in sent");
    showCheckInResult("Waiting for server...");
}

static void processCheckInService() {
    uint32_t now = millis();
    if (WiFi.status() == WL_CONNECTED &&
        g_wifiWorkerResult.load() != WIFI_WORKER_RUNNING &&
        now - g_lastCheckInScan >= ONION_CHECKIN_SCAN_INTERVAL_MS) {
        g_lastCheckInScan = now;
        String error;
        if (!ensureEspNow(0, error)) {
            Serial.printf("[onion-os] check-in ESP-NOW unavailable: %s\n", error.c_str());
        }
    }

    CheckInPendingResult pendingResult;
    bool hasResult = false;
    portENTER_CRITICAL(&g_checkinMux);
    if (g_checkinResultPending) {
        pendingResult = g_checkinPendingResult;
        g_checkinResultPending = false;
        hasResult = true;
    }
    portEXIT_CRITICAL(&g_checkinMux);
    if (hasResult) {
        String msg = String(pendingResult.message);
        if (!msg.length()) msg = pendingResult.awarded ? "Attendance recorded" : "No active workshop";
        showCheckInResult(msg, pendingResult.awarded, pendingResult.points);
        if (pendingResult.awarded) refreshPublicProfile(true);
    }

    if (g_screen == SCREEN_CHECKIN_RESULT &&
        g_checkinResult.shownAt &&
        now - g_checkinResult.shownAt > ONION_CHECKIN_RESULT_MS) {
        g_screen = SCREEN_STATUS;
        g_needsRedraw = true;
    }

    CheckInPendingOffer offer;
    bool hasOffer = false;
    portENTER_CRITICAL(&g_checkinMux);
    if (g_checkinOfferPending) {
        offer = g_checkinPendingOffer;
        g_checkinOfferPending = false;
        hasOffer = true;
    }
    portEXIT_CRITICAL(&g_checkinMux);
    if (!hasOffer || !screenAllowsCheckInPrompt()) return;

    String beaconId = String(offer.beaconId);
    if (!beaconId.length()) beaconId = macToString(offer.beaconMac);
    if (beaconId == g_lastCheckInBeaconId &&
        now - g_lastCheckInPromptAt < ONION_CHECKIN_PROMPT_COOLDOWN_MS) {
        return;
    }

    g_checkinPrompt.beaconId = beaconId;
    g_checkinPrompt.room = String(offer.room);
    g_checkinPrompt.label = String(offer.label);
    memcpy(g_checkinPrompt.beaconMac, offer.beaconMac, sizeof(g_checkinPrompt.beaconMac));
    memcpy(g_checkinPrompt.nonce, offer.nonce, sizeof(g_checkinPrompt.nonce));
    g_checkinPrompt.rssi = offer.rssi;
    g_checkinPrompt.minRssi = offer.minRssi;
    g_checkinPrompt.active = true;
    g_screen = SCREEN_CHECKIN_PROMPT;
    g_forceFullRefresh = true;
    setLog("Check-in beacon nearby");
}

static bool luaCanReadGpio(int pin) {
    for (int allowed : kLuaReadableGpios) {
        if (allowed == pin) return true;
    }
    return false;
}

static bool luaConfigureInputGpio(lua_State* L, int pin, int modeArg) {
    if (!luaCanReadGpio(pin)) {
        lua_pushboolean(L, false);
        lua_pushfstring(L, "GPIO %d is not readable by Lua", pin);
        return false;
    }

    const char* mode = luaL_optstring(L, modeArg, "input");
    if (strcmp(mode, "input") == 0 || strcmp(mode, "floating") == 0) {
        pinMode(pin, INPUT);
    } else if (strcmp(mode, "pullup") == 0 || strcmp(mode, "up") == 0) {
        pinMode(pin, INPUT_PULLUP);
    } else if (strcmp(mode, "pulldown") == 0 || strcmp(mode, "down") == 0) {
        pinMode(pin, INPUT_PULLDOWN);
    } else {
        lua_pushboolean(L, false);
        lua_pushfstring(L, "Bad GPIO mode: %s", mode);
        return false;
    }
    return true;
}

// ===========================================================================
// Swappable side-port modules (docs/MODULES.md): CC1101 Sub-GHz radio and the
// Sound module (NS4168 I2S amp + SPH0641 PDM mic). Both modules land on the
// same five physical pins, so only one may be active at a time. The pins differ
// per board variant (L1/L2/R) selectable via NVS ("module <variant>") and
// overridable per begin() call.
// ===========================================================================

struct ModuleVariantPins {
    const char* name;
    // [0]=MOSI/MicData [1]=CLK/BitClk [2]=CS/WS [3]=MISO/AudioOut [4]=GDO0/CTRL
    int line[5];
};

static const ModuleVariantPins kModuleVariants[] = {
    {"L1", {48, 47, 19, 42, 41}},
    {"L2", {40, 41, 42, 19, 47}},
    {"R",  {38, 39, 16, 15, 7}},
};

static const ModuleVariantPins& resolveModuleVariant() {
    for (const ModuleVariantPins& v : kModuleVariants) {
        if (g_config.moduleVariant.equalsIgnoreCase(v.name)) return v;
    }
    return kModuleVariants[0];
}

enum ActiveModule { MODULE_NONE, MODULE_SUBGHZ, MODULE_SOUND_SPK, MODULE_SOUND_MIC };
static ActiveModule g_activeModule = MODULE_NONE;
static int g_modulePowerPin = -1;

// Asserts the peripheral power rail HIGH (PINOUT.md: GPIO18 enables module VCC).
static void modulePowerOn(int powerPin) {
    g_modulePowerPin = powerPin;
    if (powerPin >= 0) {
        pinMode(powerPin, OUTPUT);
        digitalWrite(powerPin, HIGH);
        delay(10);
    }
}

static void modulePowerOff() {
    if (g_modulePowerPin >= 0) {
        digitalWrite(g_modulePowerPin, LOW);
        g_modulePowerPin = -1;
    }
}

// Reads an integer pin override from an options table, else returns fallback.
static int luaModulePin(lua_State* L, int idx, const char* key, int fallback) {
    if (idx) {
        lua_getfield(L, idx, key);
        if (lua_isnumber(L, -1)) {
            int v = (int)lua_tointeger(L, -1);
            lua_pop(L, 1);
            return v;
        }
        lua_pop(L, 1);
    }
    return fallback;
}

// --- CC1101 Sub-GHz radio --------------------------------------------------

#define CC1101_SRES     0x30
#define CC1101_SRX      0x34
#define CC1101_STX      0x35
#define CC1101_SIDLE    0x36
#define CC1101_SPWD     0x39
#define CC1101_SFRX     0x3A
#define CC1101_SFTX     0x3B
#define CC1101_PARTNUM  0x30
#define CC1101_VERSION  0x31
#define CC1101_MARCSTATE 0x35
#define CC1101_RSSI     0x34   // status-register read (same addr as SRX strobe)
#define CC1101_RXBYTES  0x3B
#define CC1101_PATABLE  0x3E
#define CC1101_FIFO     0x3F

static SPIClass g_subghzSpi(HSPI);
static int g_subghzCs = -1;
static int g_subghzMiso = -1;
static int g_subghzGdo0 = -1;
static double g_subghzFreq = 0.0;

static inline void cc1101Select() {
    digitalWrite(g_subghzCs, LOW);
    if (g_subghzMiso >= 0) {
        uint32_t start = micros();
        while (digitalRead(g_subghzMiso) && (micros() - start) < 5000) {}
    }
}

static inline void cc1101Deselect() {
    digitalWrite(g_subghzCs, HIGH);
}

static uint8_t cc1101Strobe(uint8_t cmd) {
    g_subghzSpi.beginTransaction(SPISettings(CC1101_SPI_HZ, MSBFIRST, SPI_MODE0));
    cc1101Select();
    uint8_t status = g_subghzSpi.transfer(cmd);
    cc1101Deselect();
    g_subghzSpi.endTransaction();
    return status;
}

static void cc1101WriteReg(uint8_t addr, uint8_t value) {
    g_subghzSpi.beginTransaction(SPISettings(CC1101_SPI_HZ, MSBFIRST, SPI_MODE0));
    cc1101Select();
    g_subghzSpi.transfer(addr & 0x3F);
    g_subghzSpi.transfer(value);
    cc1101Deselect();
    g_subghzSpi.endTransaction();
}

static uint8_t cc1101ReadReg(uint8_t addr) {
    g_subghzSpi.beginTransaction(SPISettings(CC1101_SPI_HZ, MSBFIRST, SPI_MODE0));
    cc1101Select();
    g_subghzSpi.transfer((addr & 0x3F) | 0x80);
    uint8_t value = g_subghzSpi.transfer(0x00);
    cc1101Deselect();
    g_subghzSpi.endTransaction();
    return value;
}

// Status registers (0x30-0x3D) must be read with the burst bit set, otherwise
// the same address byte is interpreted as a command strobe.
static uint8_t cc1101ReadStatus(uint8_t addr) {
    g_subghzSpi.beginTransaction(SPISettings(CC1101_SPI_HZ, MSBFIRST, SPI_MODE0));
    cc1101Select();
    g_subghzSpi.transfer((addr & 0x3F) | 0xC0);
    uint8_t value = g_subghzSpi.transfer(0x00);
    cc1101Deselect();
    g_subghzSpi.endTransaction();
    return value;
}

static void cc1101WriteBurst(uint8_t addr, const uint8_t* data, size_t len) {
    g_subghzSpi.beginTransaction(SPISettings(CC1101_SPI_HZ, MSBFIRST, SPI_MODE0));
    cc1101Select();
    g_subghzSpi.transfer((addr & 0x3F) | 0x40);
    for (size_t i = 0; i < len; i++) g_subghzSpi.transfer(data[i]);
    cc1101Deselect();
    g_subghzSpi.endTransaction();
}

static void cc1101ReadBurst(uint8_t addr, uint8_t* data, size_t len) {
    g_subghzSpi.beginTransaction(SPISettings(CC1101_SPI_HZ, MSBFIRST, SPI_MODE0));
    cc1101Select();
    g_subghzSpi.transfer((addr & 0x3F) | 0xC0);
    for (size_t i = 0; i < len; i++) data[i] = g_subghzSpi.transfer(0x00);
    cc1101Deselect();
    g_subghzSpi.endTransaction();
}

static void cc1101Reset() {
    cc1101Deselect();
    delayMicroseconds(5);
    digitalWrite(g_subghzCs, LOW);
    delayMicroseconds(10);
    cc1101Deselect();
    delayMicroseconds(45);
    cc1101Strobe(CC1101_SRES);
    delay(1);
}

// Common register block: variable-length packets, CRC on, GDO0 asserts while a
// packet is being received. Modulation/frequency are layered on top.
static void cc1101ApplyBaseConfig() {
    static const uint8_t kBase[][2] = {
        {0x02, 0x06}, {0x03, 0x47}, {0x04, 0xD3}, {0x05, 0x91}, {0x06, 0x3D},
        {0x07, 0x04}, {0x08, 0x05}, {0x09, 0x00}, {0x0A, 0x00}, {0x0B, 0x06},
        {0x0C, 0x00}, {0x10, 0xF6}, {0x11, 0x83}, {0x13, 0x22}, {0x14, 0xF8},
        {0x16, 0x07}, {0x17, 0x30}, {0x18, 0x18}, {0x19, 0x16}, {0x1A, 0x6C},
        {0x1B, 0x03}, {0x1C, 0x40}, {0x1D, 0x91}, {0x20, 0xFB}, {0x21, 0x56},
        {0x23, 0xE9}, {0x24, 0x2A}, {0x25, 0x00}, {0x26, 0x1F}, {0x29, 0x59},
        {0x2C, 0x81}, {0x2D, 0x35}, {0x2E, 0x09},
    };
    for (const auto& reg : kBase) cc1101WriteReg(reg[0], reg[1]);
}

static void cc1101SetModulation(const char* mod) {
    uint8_t mdmcfg2 = 0x13; // GFSK + 30/32 sync (default)
    uint8_t frend0 = 0x10;
    uint8_t deviatn = 0x47;
    bool ook = false;
    if (mod) {
        if (strcasecmp(mod, "ook") == 0 || strcasecmp(mod, "ask") == 0) {
            mdmcfg2 = 0x33; frend0 = 0x11; ook = true;
        } else if (strcasecmp(mod, "2fsk") == 0 || strcasecmp(mod, "fsk") == 0) {
            mdmcfg2 = 0x03;
        } else if (strcasecmp(mod, "msk") == 0) {
            mdmcfg2 = 0x73;
        }
    }
    cc1101WriteReg(0x12, mdmcfg2);
    cc1101WriteReg(0x15, deviatn);
    cc1101WriteReg(0x22, frend0);
    if (ook) {
        uint8_t pa[2] = {0x00, 0xC0}; // OOK: index0 = off, index1 = on
        cc1101WriteBurst(CC1101_PATABLE, pa, 2);
    } else {
        uint8_t pa = 0xC0;
        cc1101WriteBurst(CC1101_PATABLE, &pa, 1);
    }
}

static void cc1101SetFrequency(double mhz) {
    uint32_t freqWord = (uint32_t)((mhz * 65536.0) / CC1101_XOSC_MHZ + 0.5);
    cc1101WriteReg(0x0D, (freqWord >> 16) & 0xFF);
    cc1101WriteReg(0x0E, (freqWord >> 8) & 0xFF);
    cc1101WriteReg(0x0F, freqWord & 0xFF);
    g_subghzFreq = mhz;
}

static bool cc1101Transmit(const uint8_t* data, size_t len) {
    if (len == 0 || len > SUBGHZ_MAX_PACKET) return false;
    cc1101Strobe(CC1101_SIDLE);
    cc1101Strobe(CC1101_SFTX);
    uint8_t fifo[SUBGHZ_MAX_PACKET + 1];
    fifo[0] = (uint8_t)len; // variable-length mode: first FIFO byte is length
    memcpy(fifo + 1, data, len);
    cc1101WriteBurst(CC1101_FIFO, fifo, len + 1);
    cc1101Strobe(CC1101_STX);
    uint32_t start = millis();
    while (millis() - start < 1000) {
        if ((cc1101ReadStatus(CC1101_MARCSTATE) & 0x1F) == 0x01) break; // back to IDLE
        delay(1);
    }
    cc1101Strobe(CC1101_SFTX);
    return true;
}

static int cc1101Receive(uint8_t* out, size_t maxLen, int* rssiRaw, uint32_t timeoutMs) {
    cc1101Strobe(CC1101_SIDLE);
    cc1101Strobe(CC1101_SFRX);
    cc1101Strobe(CC1101_SRX);
    uint32_t start = millis();
    while ((uint32_t)(millis() - start) < timeoutMs) {
        uint8_t rxBytes = cc1101ReadStatus(CC1101_RXBYTES);
        if (rxBytes & 0x80) { // RX FIFO overflow
            cc1101Strobe(CC1101_SIDLE);
            cc1101Strobe(CC1101_SFRX);
            cc1101Strobe(CC1101_SRX);
            delay(2);
            continue;
        }
        if ((rxBytes & 0x7F) > 0) {
            uint8_t len = cc1101ReadReg(CC1101_FIFO);
            if (len == 0 || len > maxLen) {
                cc1101Strobe(CC1101_SIDLE);
                cc1101Strobe(CC1101_SFRX);
                cc1101Strobe(CC1101_SRX);
                delay(2);
                continue;
            }
            cc1101ReadBurst(CC1101_FIFO, out, len);
            uint8_t status[2];
            cc1101ReadBurst(CC1101_FIFO, status, 2); // appended RSSI + LQI/CRC
            if (rssiRaw) *rssiRaw = status[0];
            cc1101Strobe(CC1101_SIDLE);
            cc1101Strobe(CC1101_SFRX);
            return (int)len;
        }
        delay(2);
    }
    cc1101Strobe(CC1101_SIDLE);
    cc1101Strobe(CC1101_SFRX);
    return 0;
}

static bool subghzBegin(double freq, const char* mod, int sck, int miso, int mosi,
                        int cs, int gdo0, int powerPin, String& error) {
    if (g_activeModule != MODULE_NONE && g_activeModule != MODULE_SUBGHZ) {
        error = "module busy; end it first";
        return false;
    }
    modulePowerOn(powerPin);
    g_subghzCs = cs;
    g_subghzMiso = miso;
    g_subghzGdo0 = gdo0;
    pinMode(cs, OUTPUT);
    digitalWrite(cs, HIGH);
    if (gdo0 >= 0) pinMode(gdo0, INPUT);
    g_subghzSpi.end();
    g_subghzSpi.begin(sck, miso, mosi, -1);

    cc1101Reset();
    uint8_t version = cc1101ReadStatus(CC1101_VERSION);
    if (version == 0x00 || version == 0xFF) {
        error = "CC1101 not detected (version 0x" + String(version, HEX) + ")";
        g_subghzSpi.end();
        modulePowerOff();
        g_activeModule = MODULE_NONE;
        return false;
    }
    cc1101ApplyBaseConfig();
    cc1101SetModulation(mod);
    cc1101SetFrequency(freq);
    cc1101Strobe(CC1101_SIDLE);
    g_activeModule = MODULE_SUBGHZ;
    return true;
}

static void subghzEnd() {
    if (g_activeModule != MODULE_SUBGHZ) return;
    cc1101Strobe(CC1101_SIDLE);
    cc1101Strobe(CC1101_SPWD);
    g_subghzSpi.end();
    modulePowerOff();
    g_activeModule = MODULE_NONE;
}

// --- CC1101 raw OOK capture / replay (record & replay fixed-code remotes) -----
// Garage/gate/car remotes send OOK/ASK with arbitrary pulse timing and no packet
// framing, so the packet engine can't see them. These helpers switch the radio
// to asynchronous transparent mode: in RX the demodulated envelope appears on
// GDO0 (we time the edges); in TX we bit-bang GDO0 and the radio keys the
// carrier to match. Replay works only on FIXED-code remotes — rolling-code
// systems (most modern garage openers) reject a replayed capture by design.
// Use only on equipment you own.
#define RAW_MAX_EDGES 4096
static uint16_t g_rawDur[RAW_MAX_EDGES];
static int g_rawCount = 0;
static int g_rawInitialLevel = 0;
static double g_rawFreq = 0.0;

static void cc1101ConfigOokAsync() {
    cc1101Strobe(CC1101_SIDLE);
    cc1101WriteReg(0x02, 0x0D); // IOCFG0: asynchronous serial data output (RX)
    cc1101WriteReg(0x08, 0x32); // PKTCTRL0: async serial mode, infinite length
    cc1101WriteReg(0x07, 0x04); // PKTCTRL1
    cc1101WriteReg(0x12, 0x30); // MDMCFG2: ASK/OOK, no Manchester, no sync
    cc1101WriteReg(0x13, 0x22); // MDMCFG1
    cc1101WriteReg(0x10, 0x67); // MDMCFG4: RX filter BW
    cc1101WriteReg(0x11, 0x32); // MDMCFG3: data rate (~3.8 kBaud slicer)
    cc1101WriteReg(0x15, 0x00); // DEVIATN: 0 for OOK
    cc1101WriteReg(0x18, 0x18); // MCSM0: auto-cal on IDLE->RX/TX
    cc1101WriteReg(0x19, 0x18); // FOCCFG
    cc1101WriteReg(0x1B, 0x07); // AGCCTRL2
    cc1101WriteReg(0x1C, 0x00); // AGCCTRL1
    cc1101WriteReg(0x1D, 0x91); // AGCCTRL0
    cc1101WriteReg(0x21, 0x11); // FREND0: OOK uses PA index 0/1
    uint8_t pa[2] = {0x00, 0xC0}; // OOK PATABLE: off, on
    cc1101WriteBurst(CC1101_PATABLE, pa, 2);
}

// Waits (up to timeoutMs) for a carrier above rssiThresh, then records GDO0 edge
// durations until a long idle gap or a ~1.5 s cap. Returns edge count, 0 if no
// signal arrived, -1 if no GDO0 pin is wired.
static int cc1101RawRecord(uint32_t timeoutMs, int rssiThresh) {
    if (g_subghzGdo0 < 0) return -1;
    cc1101ConfigOokAsync();
    pinMode(g_subghzGdo0, INPUT);
    cc1101Strobe(CC1101_SIDLE);
    cc1101Strobe(CC1101_SFRX);
    cc1101Strobe(CC1101_SRX);
    delay(2);

    uint32_t tstart = millis();
    bool carrier = false;
    while ((uint32_t)(millis() - tstart) < timeoutMs) {
        uint8_t raw = cc1101ReadStatus(CC1101_RSSI);
        int dbm = (raw >= 128 ? (raw - 256) : raw) / 2 - 74;
        if (dbm > rssiThresh) { carrier = true; break; }
        delay(2);
    }
    if (!carrier) { cc1101Strobe(CC1101_SIDLE); return 0; }

    g_rawCount = 0;
    int level = digitalRead(g_subghzGdo0);
    g_rawInitialLevel = level;
    uint32_t last = micros();
    uint32_t deadline = millis() + 1500;   // hard cap (well under the task WDT)
    const uint32_t GAP_END_US = 12000;      // 12 ms idle after a burst = done
    while (g_rawCount < RAW_MAX_EDGES) {
        int cur = digitalRead(g_subghzGdo0);
        uint32_t now = micros();
        if (cur != level) {
            uint32_t dur = now - last;
            if (dur > 65535) dur = 65535;
            g_rawDur[g_rawCount++] = (uint16_t)dur;
            last = now;
            level = cur;
        } else if ((uint32_t)(now - last) > GAP_END_US && g_rawCount > 16) {
            break;
        }
        if (millis() > deadline) break;
    }
    cc1101Strobe(CC1101_SIDLE);
    g_rawFreq = g_subghzFreq;
    return g_rawCount;
}

static bool cc1101RawReplay(int repeats, int gapMs) {
    if (g_rawCount < 2 || g_subghzGdo0 < 0) return false;
    cc1101ConfigOokAsync();
    pinMode(g_subghzGdo0, OUTPUT);   // GDO0 is the TX data input in async mode
    digitalWrite(g_subghzGdo0, LOW);
    cc1101Strobe(CC1101_SIDLE);
    cc1101Strobe(CC1101_SFTX);
    cc1101Strobe(CC1101_STX);
    delayMicroseconds(400);          // let the PLL settle into TX
    for (int r = 0; r < repeats; r++) {
        int level = g_rawInitialLevel;
        for (int i = 0; i < g_rawCount; i++) {
            digitalWrite(g_subghzGdo0, level);
            delayMicroseconds(g_rawDur[i]);
            level = !level;
        }
        digitalWrite(g_subghzGdo0, LOW);
        if (gapMs > 0) delay(gapMs);
    }
    cc1101Strobe(CC1101_SIDLE);
    pinMode(g_subghzGdo0, INPUT);
    return true;
}

static int luaOnionSubghzRawRecord(lua_State* L) {
    if (g_activeModule != MODULE_SUBGHZ) {
        lua_pushnil(L); lua_pushstring(L, "subghz not started"); return 2;
    }
    uint32_t timeoutMs = (uint32_t)luaL_optinteger(L, 1, 5000);
    if (timeoutMs > 20000) timeoutMs = 20000;
    int thresh = (int)luaL_optinteger(L, 2, -78);
    int n = cc1101RawRecord(timeoutMs, thresh);
    if (n <= 0) {
        lua_pushnil(L);
        lua_pushstring(L, n == 0 ? "no signal" : "no gdo0 pin");
        return 2;
    }
    uint32_t total = 0; uint16_t mn = 65535, mx = 0;
    for (int i = 0; i < g_rawCount; i++) {
        total += g_rawDur[i];
        if (g_rawDur[i] < mn) mn = g_rawDur[i];
        if (g_rawDur[i] > mx) mx = g_rawDur[i];
    }
    lua_newtable(L);
    lua_pushinteger(L, g_rawCount);        lua_setfield(L, -2, "edges");
    lua_pushinteger(L, (lua_Integer)total); lua_setfield(L, -2, "duration_us");
    lua_pushinteger(L, mn);                 lua_setfield(L, -2, "min_us");
    lua_pushinteger(L, mx);                 lua_setfield(L, -2, "max_us");
    lua_pushnumber(L, g_rawFreq);           lua_setfield(L, -2, "frequency");
    return 1;
}

static int luaOnionSubghzRawReplay(lua_State* L) {
    if (g_activeModule != MODULE_SUBGHZ) {
        lua_pushnil(L); lua_pushstring(L, "subghz not started"); return 2;
    }
    int repeats = (int)luaL_optinteger(L, 1, 5);
    if (repeats < 1) repeats = 1;
    if (repeats > 50) repeats = 50;
    int gapMs = (int)luaL_optinteger(L, 2, 20);
    if (gapMs < 0) gapMs = 0;
    if (gapMs > 1000) gapMs = 1000;
    if (!cc1101RawReplay(repeats, gapMs)) {
        lua_pushnil(L); lua_pushstring(L, "nothing recorded"); return 2;
    }
    lua_pushboolean(L, true);
    return 1;
}

static int luaOnionSubghzRawInfo(lua_State* L) {
    lua_newtable(L);
    lua_pushinteger(L, g_rawCount); lua_setfield(L, -2, "edges");
    lua_pushnumber(L, g_rawFreq);   lua_setfield(L, -2, "frequency");
    return 1;
}

static int luaOnionSubghzRawSave(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    if (g_rawCount < 2) { lua_pushnil(L); lua_pushstring(L, "nothing recorded"); return 2; }
    String path = String("/raw_") + name + ".bin";
    File f = SPIFFS.open(path, FILE_WRITE);
    if (!f) { lua_pushnil(L); lua_pushstring(L, "open failed"); return 2; }
    int32_t hdr[2] = { g_rawCount, g_rawInitialLevel };
    f.write((const uint8_t*)hdr, sizeof(hdr));
    f.write((const uint8_t*)&g_rawFreq, sizeof(double));
    f.write((const uint8_t*)g_rawDur, sizeof(uint16_t) * g_rawCount);
    f.close();
    lua_pushboolean(L, true);
    return 1;
}

static int luaOnionSubghzRawLoad(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    String path = String("/raw_") + name + ".bin";
    File f = SPIFFS.open(path, FILE_READ);
    if (!f) { lua_pushnil(L); lua_pushstring(L, "not found"); return 2; }
    int32_t hdr[2];
    if (f.read((uint8_t*)hdr, sizeof(hdr)) != (int)sizeof(hdr)) {
        f.close(); lua_pushnil(L); lua_pushstring(L, "bad file"); return 2;
    }
    int n = hdr[0];
    if (n < 2 || n > RAW_MAX_EDGES) {
        f.close(); lua_pushnil(L); lua_pushstring(L, "bad count"); return 2;
    }
    g_rawCount = n;
    g_rawInitialLevel = hdr[1];
    f.read((uint8_t*)&g_rawFreq, sizeof(double));
    f.read((uint8_t*)g_rawDur, sizeof(uint16_t) * n);
    f.close();
    lua_pushboolean(L, true);
    lua_pushinteger(L, n);
    return 2;
}

// --- Sound module: NS4168 amp (I2S std TX) + SPH0641 mic (PDM RX) -----------

static i2s_chan_handle_t g_i2sTx = nullptr;
static i2s_chan_handle_t g_i2sRx = nullptr;
static int g_soundSampleRate = SOUND_SPK_SAMPLE_RATE;
static int g_soundCtrlPin = -1;
// Mic capture counters (reset on sound_mic_begin) so a script pacing a
// real-time capture loop can see elapsed time and dropped-read health.
static uint32_t g_soundMicStartedAt = 0;
static uint32_t g_soundMicSamples = 0;
static uint32_t g_soundMicBytes = 0;
static uint32_t g_soundMicTimeouts = 0;

// Writes `frames` frames of speaker silence. Used to prime the TX DMA before
// real samples (otherwise the first chunk's head is clipped) and to drain the
// pipeline before mute (otherwise the tail is).
static void soundWriteSilence(int frames) {
    if (!g_i2sTx) return;
    static const int16_t kZeros[256] = {};
    while (frames > 0) {
        int n = frames > 256 ? 256 : frames;
        size_t written = 0;
        if (i2s_channel_write(g_i2sTx, kZeros, n * sizeof(int16_t), &written, 500) != ESP_OK) return;
        if (written == 0) return;
        frames -= n;
    }
}

static void soundStop() {
    if (g_i2sTx) {
        soundWriteSilence(g_soundSampleRate / 8); // ~125 ms drain so the tail plays out
        i2s_channel_disable(g_i2sTx);
        i2s_del_channel(g_i2sTx);
        g_i2sTx = nullptr;
    }
    if (g_i2sRx) {
        i2s_channel_disable(g_i2sRx);
        i2s_del_channel(g_i2sRx);
        g_i2sRx = nullptr;
    }
    if (g_soundCtrlPin >= 0) {
        digitalWrite(g_soundCtrlPin, LOW);
        g_soundCtrlPin = -1;
    }
    if (g_activeModule == MODULE_SOUND_SPK || g_activeModule == MODULE_SOUND_MIC) {
        modulePowerOff();
        g_activeModule = MODULE_NONE;
    }
}

static bool soundSpeakerBegin(int bclk, int ws, int dout, int ctrl, int sampleRate,
                              int powerPin, String& error) {
    if (g_activeModule != MODULE_NONE) {
        error = "module busy; end it first";
        return false;
    }
    modulePowerOn(powerPin);
    g_soundCtrlPin = ctrl;
    if (ctrl >= 0) {
        pinMode(ctrl, OUTPUT);
        digitalWrite(ctrl, HIGH); // NS4168 CTRL high = amp enabled
        delay(SOUND_AMP_UNMUTE_MS); // amp soft-start; samples written sooner get clipped
    }

    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    if (i2s_new_channel(&chanCfg, &g_i2sTx, nullptr) != ESP_OK) {
        error = "i2s alloc failed";
        modulePowerOff();
        return false;
    }
    i2s_std_config_t stdCfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)sampleRate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)bclk,
            .ws = (gpio_num_t)ws,
            .dout = (gpio_num_t)dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {0, 0, 0},
        },
    };
    if (i2s_channel_init_std_mode(g_i2sTx, &stdCfg) != ESP_OK) {
        error = "i2s std init failed";
        i2s_del_channel(g_i2sTx);
        g_i2sTx = nullptr;
        modulePowerOff();
        return false;
    }
    i2s_channel_enable(g_i2sTx);
    g_soundSampleRate = sampleRate;
    g_activeModule = MODULE_SOUND_SPK;
    soundWriteSilence(sampleRate / 32); // ~31 ms DMA priming
    return true;
}

static void soundPlayTone(double freq, int durationMs, double volume) {
    const int sr = g_soundSampleRate;
    int64_t total = (int64_t)sr * durationMs / 1000;
    if (volume < 0) volume = 0;
    if (volume > 1) volume = 1;
    const double twoPi = 6.283185307179586;
    double step = freq > 0 ? twoPi * freq / sr : 0.0;
    double phase = 0.0;
    int16_t buf[256];
    int64_t produced = 0;
    while (produced < total) {
        int n = (int)((total - produced) > 256 ? 256 : (total - produced));
        for (int i = 0; i < n; i++) {
            buf[i] = (int16_t)(sin(phase) * 32000.0 * volume);
            phase += step;
            if (phase >= twoPi) phase -= twoPi;
        }
        size_t written = 0;
        i2s_channel_write(g_i2sTx, buf, n * sizeof(int16_t), &written, 1000);
        produced += n;
    }
}

// dmaDesc/dmaFrame of 0 keep the I2S driver defaults (~90 ms of buffering).
// Streaming scripts that process audio between reads (DSP/codec) should pass
// larger values — e.g. 16 descriptors x 512 frames buffers ~512 ms at 16 kHz,
// enough to ride out an e-ink refresh without the PDM ring overflowing.
// discardMs drops the mic's start-up transient (DC-settle pop) before any
// samples reach the script.
static bool soundMicBegin(int clk, int din, int ws, int ctrl, int sampleRate,
                          int dmaDesc, int dmaFrame, int discardMs,
                          int powerPin, String& error) {
    if (g_activeModule != MODULE_NONE) {
        error = "module busy; end it first";
        return false;
    }
    modulePowerOn(powerPin);
    // Mic mode select: mute the NS4168 (CTRL low) and hold the shared WS line
    // low so the SPH0641 drives the left PDM phase before the clock starts.
    if (ctrl >= 0) {
        pinMode(ctrl, OUTPUT);
        digitalWrite(ctrl, LOW);
    }
    if (ws >= 0) {
        pinMode(ws, OUTPUT);
        digitalWrite(ws, LOW);
    }
    delay(10);

    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    if (dmaDesc > 0) {
        if (dmaDesc > 64) dmaDesc = 64;
        chanCfg.dma_desc_num = dmaDesc;
    }
    if (dmaFrame > 0) {
        if (dmaFrame > 2046) dmaFrame = 2046; // 16-bit mono: 4092-byte DMA buffer cap
        chanCfg.dma_frame_num = dmaFrame;
    }
    if (i2s_new_channel(&chanCfg, nullptr, &g_i2sRx) != ESP_OK) {
        error = "i2s alloc failed";
        modulePowerOff();
        return false;
    }
    i2s_pdm_rx_config_t pdmCfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG((uint32_t)sampleRate),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = (gpio_num_t)clk,
            .din = (gpio_num_t)din,
            .invert_flags = {0},
        },
    };
    // 16x PDM downsampling keeps the SPH0641's clock comfortably inside its
    // normal-mode range at 16 kHz and measurably improves SNR over the default.
    pdmCfg.clk_cfg.dn_sample_mode = I2S_PDM_DSR_16S;
    pdmCfg.slot_cfg.slot_mask = I2S_PDM_SLOT_LEFT;
    if (i2s_channel_init_pdm_rx_mode(g_i2sRx, &pdmCfg) != ESP_OK) {
        error = "i2s pdm init failed";
        i2s_del_channel(g_i2sRx);
        g_i2sRx = nullptr;
        modulePowerOff();
        return false;
    }
    i2s_channel_enable(g_i2sRx);

    if (discardMs > SOUND_MIC_MAX_DISCARD_MS) discardMs = SOUND_MIC_MAX_DISCARD_MS;
    if (discardMs > 0) {
        int remaining = sampleRate * discardMs / 1000;
        int16_t scratch[256];
        uint32_t deadline = millis() + discardMs + 80;
        while (remaining > 0 && (int32_t)(deadline - millis()) > 0) {
            size_t toRead = remaining >= 256 ? sizeof(scratch) : remaining * sizeof(scratch[0]);
            size_t bytesRead = 0;
            esp_err_t rc = i2s_channel_read(g_i2sRx, scratch, toRead, &bytesRead, 120);
            if (rc != ESP_OK && rc != ESP_ERR_TIMEOUT) break;
            remaining -= (int)(bytesRead / sizeof(scratch[0]));
        }
    }

    g_soundSampleRate = sampleRate;
    g_soundMicStartedAt = millis();
    g_soundMicSamples = 0;
    g_soundMicBytes = 0;
    g_soundMicTimeouts = 0;
    g_activeModule = MODULE_SOUND_MIC;
    return true;
}

// Powers down whichever module a script left running. Called when a Lua script
// finishes so the radio/amp never stays energised between runs.
static void moduleShutdownActive() {
    if (g_activeModule == MODULE_SUBGHZ) subghzEnd();
    else if (g_activeModule != MODULE_NONE) soundStop();
}

// --- Lua bindings ----------------------------------------------------------

static int luaOnionSubghzBegin(lua_State* L) {
    int opt = lua_istable(L, 1) ? 1 : 0;
    const ModuleVariantPins& v = resolveModuleVariant();
    double freq = 433.92;
    const char* mod = "gfsk";
    if (opt) {
        lua_getfield(L, opt, "freq");
        if (lua_isnumber(L, -1)) freq = lua_tonumber(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, opt, "modulation");
        if (lua_type(L, -1) == LUA_TSTRING) mod = lua_tostring(L, -1);
        lua_pop(L, 1);
    }
    int mosi = luaModulePin(L, opt, "mosi", v.line[0]);
    int sck = luaModulePin(L, opt, "sck", v.line[1]);
    int cs = luaModulePin(L, opt, "cs", v.line[2]);
    int miso = luaModulePin(L, opt, "miso", v.line[3]);
    int gdo0 = luaModulePin(L, opt, "gdo0", v.line[4]);
    int powerPin = luaModulePin(L, opt, "power_pin", PIN_PWR);

    String error;
    if (!subghzBegin(freq, mod, sck, miso, mosi, cs, gdo0, powerPin, error)) {
        lua_pushnil(L);
        lua_pushstring(L, error.c_str());
        return 2;
    }
    lua_pushboolean(L, true);
    return 1;
}

static int luaOnionSubghzTransmit(lua_State* L) {
    if (g_activeModule != MODULE_SUBGHZ) {
        lua_pushnil(L);
        lua_pushstring(L, "subghz not started");
        return 2;
    }
    size_t len = 0;
    const char* data = luaL_checklstring(L, 1, &len);
    if (len == 0 || len > SUBGHZ_MAX_PACKET) {
        lua_pushnil(L);
        lua_pushstring(L, "payload must be 1-61 bytes");
        return 2;
    }
    if (!cc1101Transmit((const uint8_t*)data, len)) {
        lua_pushnil(L);
        lua_pushstring(L, "transmit failed");
        return 2;
    }
    lua_pushboolean(L, true);
    return 1;
}

static int luaOnionSubghzReceive(lua_State* L) {
    if (g_activeModule != MODULE_SUBGHZ) {
        lua_pushnil(L);
        lua_pushstring(L, "subghz not started");
        return 2;
    }
    uint32_t timeoutMs = (uint32_t)luaL_optinteger(L, 1, 0);
    if (timeoutMs > SUBGHZ_RX_MAX_MS) timeoutMs = SUBGHZ_RX_MAX_MS;

    uint8_t buf[SUBGHZ_MAX_PACKET];
    int rssiRaw = 0;
    int n = cc1101Receive(buf, sizeof(buf), &rssiRaw, timeoutMs);
    if (n <= 0) {
        lua_pushnil(L);
        return 1;
    }
    int rssiDbm = (rssiRaw >= 128 ? (rssiRaw - 256) : rssiRaw) / 2 - 74;
    lua_newtable(L);
    lua_pushlstring(L, (const char*)buf, n);
    lua_setfield(L, -2, "payload");
    lua_pushlstring(L, (const char*)buf, n);
    lua_setfield(L, -2, "message");
    lua_pushinteger(L, n);
    lua_setfield(L, -2, "len");
    lua_pushinteger(L, rssiRaw);
    lua_setfield(L, -2, "rssi");
    lua_pushinteger(L, rssiDbm);
    lua_setfield(L, -2, "rssi_dbm");
    return 1;
}

static int luaOnionSubghzSetFrequency(lua_State* L) {
    if (g_activeModule != MODULE_SUBGHZ) {
        lua_pushnil(L);
        lua_pushstring(L, "subghz not started");
        return 2;
    }
    double mhz = luaL_checknumber(L, 1);
    cc1101Strobe(CC1101_SIDLE);
    cc1101SetFrequency(mhz);
    lua_pushboolean(L, true);
    return 1;
}

// Reads the CC1101 carrier RSSI on the current frequency, independent of any
// packet — enables spectrum sweeps / a frequency analyzer. Enters RX, waits for
// the demodulator to settle, samples the RSSI status register, then idles.
// Returns rssi_dbm (integer) and the raw register byte. Optional arg 1 is the
// settle time in ms (default 3, clamped 1..50).
static int luaOnionSubghzRssi(lua_State* L) {
    if (g_activeModule != MODULE_SUBGHZ) {
        lua_pushnil(L);
        lua_pushstring(L, "subghz not started");
        return 2;
    }
    int settleMs = (int)luaL_optinteger(L, 1, 3);
    if (settleMs < 1) settleMs = 1;
    if (settleMs > 50) settleMs = 50;
    cc1101Strobe(CC1101_SIDLE);
    cc1101Strobe(CC1101_SRX);
    uint32_t start = millis();
    while ((cc1101ReadStatus(CC1101_MARCSTATE) & 0x1F) != 0x0D &&
           (uint32_t)(millis() - start) < 10) {
        delay(1);  // wait until the radio reaches the RX state (MARCSTATE 0x0D)
    }
    delay(settleMs);  // let the AGC/RSSI estimate settle after entering RX
    uint8_t raw = cc1101ReadStatus(CC1101_RSSI);
    cc1101Strobe(CC1101_SIDLE);
    int dbm = (raw >= 128 ? (raw - 256) : raw) / 2 - 74;  // CC1101 RSSI_offset = 74
    lua_pushinteger(L, dbm);
    lua_pushinteger(L, raw);
    return 2;
}

static int luaOnionSubghzInfo(lua_State* L) {
    lua_newtable(L);
    lua_pushstring(L, resolveModuleVariant().name);
    lua_setfield(L, -2, "variant");
    lua_pushboolean(L, g_activeModule == MODULE_SUBGHZ);
    lua_setfield(L, -2, "active");
    if (g_activeModule == MODULE_SUBGHZ) {
        lua_pushnumber(L, g_subghzFreq);
        lua_setfield(L, -2, "frequency");
        lua_pushinteger(L, cc1101ReadStatus(CC1101_VERSION));
        lua_setfield(L, -2, "version");
        lua_pushinteger(L, cc1101ReadStatus(CC1101_PARTNUM));
        lua_setfield(L, -2, "partnum");
    }
    return 1;
}

static int luaOnionSubghzEnd(lua_State* L) {
    subghzEnd();
    lua_pushboolean(L, true);
    return 1;
}

static int luaOnionSoundSpeakerBegin(lua_State* L) {
    int opt = lua_istable(L, 1) ? 1 : 0;
    const ModuleVariantPins& v = resolveModuleVariant();
    int bclk = luaModulePin(L, opt, "bclk", v.line[1]);
    int ws = luaModulePin(L, opt, "ws", v.line[2]);
    int dout = luaModulePin(L, opt, "dout", v.line[3]);
    int ctrl = luaModulePin(L, opt, "ctrl", v.line[4]);
    int powerPin = luaModulePin(L, opt, "power_pin", PIN_PWR);
    int sampleRate = (int)luaModulePin(L, opt, "sample_rate", SOUND_SPK_SAMPLE_RATE);

    String error;
    if (!soundSpeakerBegin(bclk, ws, dout, ctrl, sampleRate, powerPin, error)) {
        lua_pushnil(L);
        lua_pushstring(L, error.c_str());
        return 2;
    }
    lua_pushboolean(L, true);
    return 1;
}

static int luaOnionSoundPlayTone(lua_State* L) {
    if (g_activeModule != MODULE_SOUND_SPK) {
        lua_pushnil(L);
        lua_pushstring(L, "speaker not started");
        return 2;
    }
    double freq = luaL_checknumber(L, 1);
    int durationMs = (int)luaL_optinteger(L, 2, 200);
    double volume = (double)luaL_optnumber(L, 3, 0.6);
    if (durationMs < 0) durationMs = 0;
    if (durationMs > SOUND_TONE_MAX_MS) durationMs = SOUND_TONE_MAX_MS;
    soundPlayTone(freq, durationMs, volume);
    lua_pushboolean(L, true);
    return 1;
}

static int luaOnionSoundPlay(lua_State* L) {
    if (g_activeModule != MODULE_SOUND_SPK) {
        lua_pushnil(L);
        lua_pushstring(L, "speaker not started");
        return 2;
    }
    size_t len = 0;
    const char* pcm = luaL_checklstring(L, 1, &len);
    if (len > SOUND_PLAY_MAX_BYTES) len = SOUND_PLAY_MAX_BYTES;
    size_t off = 0;
    while (off < len) {
        size_t written = 0;
        if (i2s_channel_write(g_i2sTx, pcm + off, len - off, &written, 1000) != ESP_OK) break;
        if (written == 0) break;
        off += written;
    }
    lua_pushinteger(L, (lua_Integer)off);
    return 1;
}

static int luaOnionSoundSpeakerEnd(lua_State* L) {
    if (g_activeModule == MODULE_SOUND_SPK) soundStop();
    lua_pushboolean(L, true);
    return 1;
}

static int luaOnionSoundMicBegin(lua_State* L) {
    int opt = lua_istable(L, 1) ? 1 : 0;
    const ModuleVariantPins& v = resolveModuleVariant();
    int clk = luaModulePin(L, opt, "clk", v.line[1]);
    int din = luaModulePin(L, opt, "din", v.line[0]);
    int ws = luaModulePin(L, opt, "ws", v.line[2]);
    int ctrl = luaModulePin(L, opt, "ctrl", v.line[4]);
    int powerPin = luaModulePin(L, opt, "power_pin", PIN_PWR);
    int sampleRate = (int)luaModulePin(L, opt, "sample_rate", SOUND_MIC_SAMPLE_RATE);
    int dmaDesc = luaTableInt(L, opt, "dma_desc", 0);
    int dmaFrame = luaTableInt(L, opt, "dma_frame", 0);
    int discardMs = luaTableInt(L, opt, "discard_ms", 0);

    String error;
    if (!soundMicBegin(clk, din, ws, ctrl, sampleRate, dmaDesc, dmaFrame,
                       discardMs, powerPin, error)) {
        lua_pushnil(L);
        lua_pushstring(L, error.c_str());
        return 2;
    }
    lua_pushboolean(L, true);
    return 1;
}

static int luaOnionSoundMicRead(lua_State* L) {
    if (g_activeModule != MODULE_SOUND_MIC) {
        lua_pushnil(L);
        lua_pushstring(L, "mic not started");
        return 2;
    }
    int numSamples = (int)luaL_optinteger(L, 1, 256);
    if (numSamples < 1) numSamples = 1;
    if (numSamples > SOUND_MIC_MAX_SAMPLES) numSamples = SOUND_MIC_MAX_SAMPLES;
    int timeoutMs = (int)luaL_optinteger(L, 2, 1000);
    if (timeoutMs < 0) timeoutMs = 0;
    if (timeoutMs > SOUND_MIC_READ_MAX_TIMEOUT_MS) timeoutMs = SOUND_MIC_READ_MAX_TIMEOUT_MS;
    static int16_t buf[SOUND_MIC_MAX_SAMPLES];
    size_t bytesRead = 0;
    esp_err_t rc = i2s_channel_read(g_i2sRx, buf, numSamples * sizeof(int16_t),
                                    &bytesRead, timeoutMs);
    bool timedOut = rc == ESP_ERR_TIMEOUT; // a timeout still returns what arrived
    if (timedOut) g_soundMicTimeouts++;
    if (rc != ESP_OK && !timedOut) {
        lua_pushnil(L);
        lua_pushstring(L, "mic read failed");
        return 2;
    }
    g_soundMicSamples += bytesRead / sizeof(int16_t);
    g_soundMicBytes += bytesRead;

    lua_pushlstring(L, (const char*)buf, bytesRead);
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)(bytesRead / sizeof(int16_t)));
    lua_setfield(L, -2, "samples");
    lua_pushinteger(L, (lua_Integer)bytesRead);
    lua_setfield(L, -2, "bytes");
    lua_pushinteger(L, g_soundSampleRate);
    lua_setfield(L, -2, "sample_rate");
    lua_pushboolean(L, timedOut);
    lua_setfield(L, -2, "timeout");
    lua_pushinteger(L, g_soundMicSamples);
    lua_setfield(L, -2, "total_samples");
    lua_pushinteger(L, g_soundMicBytes);
    lua_setfield(L, -2, "total_bytes");
    lua_pushinteger(L, g_soundMicTimeouts);
    lua_setfield(L, -2, "timeouts");
    lua_pushinteger(L, (lua_Integer)(millis() - g_soundMicStartedAt));
    lua_setfield(L, -2, "elapsed_ms");
    return 2;
}

static int luaOnionSoundMicLevel(lua_State* L) {
    if (g_activeModule != MODULE_SOUND_MIC) {
        lua_pushnil(L);
        lua_pushstring(L, "mic not started");
        return 2;
    }
    int durationMs = (int)luaL_optinteger(L, 1, 100);
    if (durationMs < 1) durationMs = 1;
    if (durationMs > 1000) durationMs = 1000;
    int wantSamples = g_soundSampleRate * durationMs / 1000;
    static int16_t buf[512];
    double sumSq = 0.0;
    int counted = 0;
    int peak = 0;
    while (counted < wantSamples) {
        int n = wantSamples - counted;
        if (n > 512) n = 512;
        size_t bytesRead = 0;
        if (i2s_channel_read(g_i2sRx, buf, n * sizeof(int16_t), &bytesRead, 1000) != ESP_OK) break;
        int got = (int)(bytesRead / sizeof(int16_t));
        if (got == 0) break;
        for (int i = 0; i < got; i++) {
            int s = buf[i];
            sumSq += (double)s * s;
            int a = s < 0 ? -s : s;
            if (a > peak) peak = a;
        }
        counted += got;
    }
    double rms = counted ? sqrt(sumSq / counted) : 0.0;
    lua_newtable(L);
    lua_pushnumber(L, rms);
    lua_setfield(L, -2, "rms");
    lua_pushinteger(L, peak);
    lua_setfield(L, -2, "peak");
    lua_pushinteger(L, counted);
    lua_setfield(L, -2, "samples");
    return 1;
}

static int luaOnionSoundMicEnd(lua_State* L) {
    if (g_activeModule != MODULE_SOUND_MIC) {
        lua_pushboolean(L, true);
        return 1;
    }
    uint32_t durationMs = millis() - g_soundMicStartedAt;
    uint32_t samples = g_soundMicSamples;
    uint32_t bytes = g_soundMicBytes;
    uint32_t timeouts = g_soundMicTimeouts;
    int sampleRate = g_soundSampleRate;
    soundStop();
    Serial.printf("[onion-os] sound mic stop samples=%u bytes=%u duration_ms=%u timeouts=%u\n",
                  (unsigned)samples, (unsigned)bytes, (unsigned)durationMs,
                  (unsigned)timeouts);

    lua_pushboolean(L, true);
    lua_newtable(L);
    lua_pushinteger(L, samples);
    lua_setfield(L, -2, "samples");
    lua_pushinteger(L, bytes);
    lua_setfield(L, -2, "bytes");
    lua_pushinteger(L, durationMs);
    lua_setfield(L, -2, "duration_ms");
    lua_pushinteger(L, timeouts);
    lua_setfield(L, -2, "timeouts");
    lua_pushinteger(L, sampleRate);
    lua_setfield(L, -2, "sample_rate");
    return 2;
}

static int luaOnionLog(lua_State* L) {
    const char* message = luaL_checkstring(L, 1);
    setLog("Lua: " + String(message));
    return 0;
}

static int luaOnionHardwareId(lua_State* L) {
    lua_pushstring(L, g_identity.hardwareId.c_str());
    return 1;
}

static int luaOnionOnionId(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)g_identity.onionId);
    return 1;
}

static int luaOnionWallet(lua_State* L) {
    lua_pushstring(L, g_identity.solanaPublicKey.c_str());
    return 1;
}

static int luaOnionUsername(lua_State* L) {
    lua_pushstring(L, g_identity.username.c_str());
    return 1;
}

// onion.secure_random([count]) -> string of `count` random bytes from the
// ATECC608A hardware RNG (default 32, max 256). Returns nil + error on failure.
static int luaOnionSecureRandom(lua_State* L) {
    lua_Integer count = luaL_optinteger(L, 1, 32);
    if (count < 1) count = 1;
    if (count > 256) count = 256;

    uint8_t buf[256];
    String error;
    if (!ateccRandom(buf, (size_t)count, error)) {
        lua_pushnil(L);
        lua_pushstring(L, error.c_str());
        return 2;
    }

    lua_pushlstring(L, reinterpret_cast<const char*>(buf), (size_t)count);
    return 1;
}

// Shared worker for onion.http_get / onion.http_post. `optionsIdx` is the Lua
// stack index of an options table ({ headers = {...}, content_type = ...,
// timeout_ms = ... }) or 0 when none was passed. On success returns one table
// { status, body }; on failure returns nil plus an error string.
static int luaHttpDo(lua_State* L, esp_http_client_method_t method, const char* url,
                     const char* body, size_t bodyLen, int optionsIdx) {
    if (!ensureWifi()) {
        lua_pushnil(L);
        lua_pushstring(L, "wifi unavailable");
        return 2;
    }

    String responseBuffer;
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = LUA_HTTP_DEFAULT_TIMEOUT_MS;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.event_handler = httpCaptureEvent;
    cfg.user_data = &responseBuffer;

    const char* contentType = "application/json";
    if (optionsIdx) {
        lua_getfield(L, optionsIdx, "timeout_ms");
        if (lua_isnumber(L, -1)) {
            int t = (int)lua_tointeger(L, -1);
            if (t > 0) cfg.timeout_ms = t > LUA_HTTP_MAX_TIMEOUT_MS ? LUA_HTTP_MAX_TIMEOUT_MS : t;
        }
        lua_pop(L, 1);
        lua_getfield(L, optionsIdx, "content_type");
        if (lua_type(L, -1) == LUA_TSTRING) contentType = lua_tostring(L, -1);
        lua_pop(L, 1);
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        lua_pushnil(L);
        lua_pushstring(L, "http init failed");
        return 2;
    }

    esp_http_client_set_method(client, method);
    if (body) {
        esp_http_client_set_header(client, "Content-Type", contentType);
        esp_http_client_set_post_field(client, body, bodyLen);
    }

    if (optionsIdx) {
        lua_getfield(L, optionsIdx, "headers");
        if (lua_istable(L, -1)) {
            int headersTable = lua_gettop(L);
            lua_pushnil(L);
            while (lua_next(L, headersTable) != 0) {
                if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TSTRING) {
                    esp_http_client_set_header(client, lua_tostring(L, -2), lua_tostring(L, -1));
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }

    esp_err_t err = esp_http_client_perform(client);
    int code = err == ESP_OK ? esp_http_client_get_status_code(client) : -1;
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }

    lua_newtable(L);
    lua_pushinteger(L, code);
    lua_setfield(L, -2, "status");
    lua_pushlstring(L, responseBuffer.c_str(), responseBuffer.length());
    lua_setfield(L, -2, "body");
    return 1;
}

// onion.http_get(url [, options]) -> { status, body } | nil, err
static int luaOnionHttpGet(lua_State* L) {
    const char* url = luaL_checkstring(L, 1);
    int optionsIdx = lua_istable(L, 2) ? 2 : 0;
    return luaHttpDo(L, HTTP_METHOD_GET, url, nullptr, 0, optionsIdx);
}

// onion.http_post(url, body [, options]) -> { status, body } | nil, err
static int luaOnionHttpPost(lua_State* L) {
    const char* url = luaL_checkstring(L, 1);
    size_t bodyLen = 0;
    const char* body = luaL_optlstring(L, 2, "", &bodyLen);
    int optionsIdx = lua_istable(L, 3) ? 3 : 0;
    return luaHttpDo(L, HTTP_METHOD_POST, url, body, bodyLen, optionsIdx);
}

// onion.mqtt_connected() -> bool
static int luaOnionMqttConnected(lua_State* L) {
    lua_pushboolean(L, g_mqttConnected);
    return 1;
}

// onion.mqtt_subscribe(topic [, qos]) -> true | nil, err
static int luaOnionMqttSubscribe(lua_State* L) {
    const char* sub = luaL_checkstring(L, 1);
    int qos = (int)luaL_optinteger(L, 2, 1);
    if (qos < 0) qos = 0;
    if (qos > 2) qos = 2;
    if (strlen(sub) > ONION_LUA_MQTT_MAX_TOPIC) {
        lua_pushnil(L);
        lua_pushstring(L, "topic too long");
        return 2;
    }

    ensureMqtt();
    if (!g_mqtt || !g_mqttConnected) {
        lua_pushnil(L);
        lua_pushstring(L, "mqtt not connected");
        return 2;
    }

    if (esp_mqtt_client_subscribe(g_mqtt, sub, qos) < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "subscribe failed");
        return 2;
    }

    // Track the filter so inbound matches are queued for onion.mqtt_receive().
    portENTER_CRITICAL(&g_luaMqttMux);
    bool known = false;
    for (uint8_t i = 0; i < g_luaMqttSubCount; i++) {
        if (strcmp(g_luaMqttSubs[i], sub) == 0) { known = true; break; }
    }
    if (!known && g_luaMqttSubCount < ONION_LUA_MQTT_MAX_SUBS) {
        strncpy(g_luaMqttSubs[g_luaMqttSubCount], sub, ONION_LUA_MQTT_MAX_TOPIC);
        g_luaMqttSubs[g_luaMqttSubCount][ONION_LUA_MQTT_MAX_TOPIC] = '\0';
        g_luaMqttSubCount++;
    }
    portEXIT_CRITICAL(&g_luaMqttMux);

    lua_pushboolean(L, true);
    return 1;
}

// onion.mqtt_unsubscribe(topic) -> true | nil, err
static int luaOnionMqttUnsubscribe(lua_State* L) {
    const char* sub = luaL_checkstring(L, 1);
    if (g_mqtt && g_mqttConnected) esp_mqtt_client_unsubscribe(g_mqtt, sub);

    portENTER_CRITICAL(&g_luaMqttMux);
    for (uint8_t i = 0; i < g_luaMqttSubCount; i++) {
        if (strcmp(g_luaMqttSubs[i], sub) == 0) {
            for (uint8_t j = i + 1; j < g_luaMqttSubCount; j++) {
                strcpy(g_luaMqttSubs[j - 1], g_luaMqttSubs[j]);
            }
            g_luaMqttSubCount--;
            break;
        }
    }
    portEXIT_CRITICAL(&g_luaMqttMux);

    lua_pushboolean(L, true);
    return 1;
}

// onion.mqtt_publish(topic, payload [, qos [, retain]]) -> true | nil, err
static int luaOnionMqttPublish(lua_State* L) {
    const char* mqttTopic = luaL_checkstring(L, 1);
    size_t payloadLen = 0;
    const char* payload = luaL_checklstring(L, 2, &payloadLen);
    int qos = (int)luaL_optinteger(L, 3, 1);
    if (qos < 0) qos = 0;
    if (qos > 2) qos = 2;
    int retain = lua_toboolean(L, 4) ? 1 : 0;

    ensureMqtt();
    if (!g_mqtt || !g_mqttConnected) {
        lua_pushnil(L);
        lua_pushstring(L, "mqtt not connected");
        return 2;
    }

    if (esp_mqtt_client_publish(g_mqtt, mqttTopic, payload, (int)payloadLen, qos, retain) < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "publish failed");
        return 2;
    }
    lua_pushboolean(L, true);
    return 1;
}

// onion.mqtt_receive([timeout_ms]) -> { topic, payload, message, len, received_at } | nil
static int luaOnionMqttReceive(lua_State* L) {
    uint32_t timeoutMs = (uint32_t)luaL_optinteger(L, 1, 0);
    if (timeoutMs > LUA_MQTT_RECV_MAX_MS) timeoutMs = LUA_MQTT_RECV_MAX_MS;

    uint32_t start = millis();
    MqttQueuedMessage msg;
    while (!luaMqttQueuePop(msg)) {
        if ((uint32_t)(millis() - start) >= timeoutMs) {
            lua_pushnil(L);
            return 1;
        }
        delay(10);
    }

    lua_newtable(L);
    lua_pushlstring(L, msg.topic, msg.topicLen);
    lua_setfield(L, -2, "topic");
    lua_pushlstring(L, msg.payload, msg.payloadLen);
    lua_setfield(L, -2, "payload");
    lua_pushlstring(L, msg.payload, msg.payloadLen);
    lua_setfield(L, -2, "message");
    lua_pushinteger(L, msg.payloadLen);
    lua_setfield(L, -2, "len");
    lua_pushinteger(L, (lua_Integer)msg.receivedAt);
    lua_setfield(L, -2, "received_at");
    return 1;
}

// onion.mqtt_info() -> { connected, uri, prefix, subscriptions, queued }
static int luaOnionMqttInfo(lua_State* L) {
    lua_newtable(L);
    lua_pushboolean(L, g_mqttConnected);
    lua_setfield(L, -2, "connected");
    lua_pushstring(L, g_config.mqttUri.c_str());
    lua_setfield(L, -2, "uri");
    lua_pushstring(L, (g_config.mqttTopicPrefix.length() ? g_config.mqttTopicPrefix : String("oniondao")).c_str());
    lua_setfield(L, -2, "prefix");
    portENTER_CRITICAL(&g_luaMqttMux);
    uint8_t subCount = g_luaMqttSubCount;
    uint8_t queued = g_luaMqttQueueCount;
    portEXIT_CRITICAL(&g_luaMqttMux);
    lua_pushinteger(L, subCount);
    lua_setfield(L, -2, "subscriptions");
    lua_pushinteger(L, queued);
    lua_setfield(L, -2, "queued");
    return 1;
}

static int luaOnionDisplaySize(lua_State* L) {
    lua_newtable(L);
    lua_pushinteger(L, display.width());
    lua_setfield(L, -2, "width");
    lua_pushinteger(L, display.height());
    lua_setfield(L, -2, "height");
    return 1;
}

static int luaOnionClearDisplay(lua_State*) {
    g_luaCanvas.fillScreen(0);
    // Scripts use clear_display as the explicit ghost-clear, so always full.
    g_forceFullRefresh = true;
    luaFlushOrDefer();
    return 0;
}

static int luaOnionReleaseDisplay(lua_State*) {
    g_luaDisplayActive = false;
    g_forceFullRefresh = true;  // guarantee clean full refresh over Lua content
    g_needsRedraw = true;
    return 0;
}

static int luaOnionDisplayText(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    int x = (int)luaL_optinteger(L, 2, 6);
    int y = (int)luaL_optinteger(L, 3, 22);
    bool clearScreen = luaDisplayClearArg(L, 4, true);
    const GFXfont* font = &FreeMono9pt7b;
    uint16_t color = GxEPD_BLACK;
    uint16_t bg = GxEPD_WHITE;

    if (lua_istable(L, 4)) {
        font = displayFontFromName(luaTableString(L, 4, "font", "small"));
        color = displayColorFromName(luaTableString(L, 4, "color", "black"), GxEPD_BLACK);
        bg = displayColorFromName(luaTableString(L, 4, "background", "white"), GxEPD_WHITE);
    } else if (lua_gettop(L) >= 5) {
        font = displayFontFromName(luaL_optstring(L, 5, "small"));
    }

    if (clearScreen) g_luaCanvas.fillScreen(canvasColor(bg));
    g_luaCanvas.setFont(font);
    g_luaCanvas.setTextColor(canvasColor(color));
    g_luaCanvas.setCursor(x, y);
    g_luaCanvas.print(text);
    luaFlushOrDefer();
    return 0;
}

static int luaOnionDisplayLines(lua_State* L) {
    int x = (int)luaL_optinteger(L, 2, 6);
    int y = (int)luaL_optinteger(L, 3, 22);
    int lineHeight = (int)luaL_optinteger(L, 4, 18);
    bool clearScreen = luaDisplayClearArg(L, 5, true);
    const GFXfont* font = &FreeMono9pt7b;
    uint16_t color = GxEPD_BLACK;
    uint16_t bg = GxEPD_WHITE;
    if (lua_istable(L, 5)) {
        font = displayFontFromName(luaTableString(L, 5, "font", "small"));
        color = displayColorFromName(luaTableString(L, 5, "color", "black"), GxEPD_BLACK);
        bg = displayColorFromName(luaTableString(L, 5, "background", "white"), GxEPD_WHITE);
        lineHeight = luaTableInt(L, 5, "line_height", lineHeight);
    }
    if (lineHeight < 8) lineHeight = 8;
    if (lineHeight > 64) lineHeight = 64;

    if (clearScreen) g_luaCanvas.fillScreen(canvasColor(bg));
    g_luaCanvas.setFont(font);
    g_luaCanvas.setTextColor(canvasColor(color));
    if (lua_istable(L, 1)) {
        int count = (int)lua_rawlen(L, 1);
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(L, 1, i);
            const char* line = lua_tostring(L, -1);
            if (line) {
                g_luaCanvas.setCursor(x, y + (i - 1) * lineHeight);
                g_luaCanvas.print(line);
            }
            lua_pop(L, 1);
        }
    } else {
        String text = luaL_checkstring(L, 1);
        int line = 0;
        int start = 0;
        while (start <= (int)text.length()) {
            int end = text.indexOf('\n', start);
            if (end < 0) end = text.length();
            g_luaCanvas.setCursor(x, y + line * lineHeight);
            g_luaCanvas.print(text.substring(start, end));
            if (end == (int)text.length()) break;
            start = end + 1;
            line++;
        }
    }
    luaFlushOrDefer();
    return 0;
}

static int luaOnionDisplayLine(lua_State* L) {
    int x0 = (int)luaL_checkinteger(L, 1);
    int y0 = (int)luaL_checkinteger(L, 2);
    int x1 = (int)luaL_checkinteger(L, 3);
    int y1 = (int)luaL_checkinteger(L, 4);
    bool clearScreen = luaDisplayClearArg(L, 5, false);
    uint16_t color = GxEPD_BLACK;
    if (lua_istable(L, 5)) color = displayColorFromName(luaTableString(L, 5, "color", "black"), GxEPD_BLACK);

    if (clearScreen) g_luaCanvas.fillScreen(0);
    g_luaCanvas.drawLine(x0, y0, x1, y1, canvasColor(color));
    luaFlushOrDefer();
    return 0;
}

static int luaOnionDisplayRect(lua_State* L) {
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int w = (int)luaL_checkinteger(L, 3);
    int h = (int)luaL_checkinteger(L, 4);
    bool clearScreen = luaDisplayClearArg(L, 5, false);
    bool fill = lua_istable(L, 5) ? luaTableBool(L, 5, "fill", false) : false;
    uint16_t color = lua_istable(L, 5)
        ? displayColorFromName(luaTableString(L, 5, "color", "black"), GxEPD_BLACK)
        : GxEPD_BLACK;

    if (clearScreen) g_luaCanvas.fillScreen(0);
    if (fill) g_luaCanvas.fillRect(x, y, w, h, canvasColor(color));
    else g_luaCanvas.drawRect(x, y, w, h, canvasColor(color));
    luaFlushOrDefer();
    return 0;
}

// onion.display_buffer() -> string (5808 raw bytes, 264×176 1-bpp, bit=1=black)
// Returns the current Lua canvas so scripts can upload the display state to a server.
static int luaOnionDisplayBuffer(lua_State* L) {
    lua_pushlstring(L, (const char*)g_luaCanvas.getBuffer(), FRAME_BYTES);
    return 1;
}

// ── Deferred flush (display_begin / display_commit / display_flush) ────────────

static int luaOnionDisplayBegin(lua_State*) {
    g_luaDeferFlush = true;
    return 0;
}

static int luaOnionDisplayCommit(lua_State*) {
    g_luaDeferFlush = false;
    if (g_luaFramePending) { g_luaFramePending = false; refreshLuaCanvas(); }
    return 0;
}

static int luaOnionDisplayFlush(lua_State*) {
    if (g_luaFramePending) { g_luaFramePending = false; refreshLuaCanvas(); }
    return 0;
}

// ── Vector drawing primitives ─────────────────────────────────────────────────

static int luaOnionDisplayCircle(lua_State* L) {
    int cx = (int)luaL_checkinteger(L, 1);
    int cy = (int)luaL_checkinteger(L, 2);
    int r  = (int)luaL_checkinteger(L, 3);
    bool clearScreen = luaDisplayClearArg(L, 4, false);
    bool fill  = lua_istable(L, 4) ? luaTableBool(L, 4, "fill",  false) : false;
    uint16_t color = lua_istable(L, 4)
        ? displayColorFromName(luaTableString(L, 4, "color", "black"), GxEPD_BLACK)
        : GxEPD_BLACK;
    if (clearScreen) g_luaCanvas.fillScreen(0);
    if (fill) g_luaCanvas.fillCircle(cx, cy, r, canvasColor(color));
    else      g_luaCanvas.drawCircle(cx, cy, r, canvasColor(color));
    luaFlushOrDefer();
    return 0;
}

static int luaOnionDisplayTriangle(lua_State* L) {
    int x0 = (int)luaL_checkinteger(L, 1), y0 = (int)luaL_checkinteger(L, 2);
    int x1 = (int)luaL_checkinteger(L, 3), y1 = (int)luaL_checkinteger(L, 4);
    int x2 = (int)luaL_checkinteger(L, 5), y2 = (int)luaL_checkinteger(L, 6);
    bool clearScreen = luaDisplayClearArg(L, 7, false);
    bool fill  = lua_istable(L, 7) ? luaTableBool(L, 7, "fill",  false) : false;
    uint16_t color = lua_istable(L, 7)
        ? displayColorFromName(luaTableString(L, 7, "color", "black"), GxEPD_BLACK)
        : GxEPD_BLACK;
    if (clearScreen) g_luaCanvas.fillScreen(0);
    if (fill) g_luaCanvas.fillTriangle(x0, y0, x1, y1, x2, y2, canvasColor(color));
    else      g_luaCanvas.drawTriangle(x0, y0, x1, y1, x2, y2, canvasColor(color));
    luaFlushOrDefer();
    return 0;
}

static int luaOnionDisplayRoundRect(lua_State* L) {
    int x = (int)luaL_checkinteger(L, 1), y = (int)luaL_checkinteger(L, 2);
    int w = (int)luaL_checkinteger(L, 3), h = (int)luaL_checkinteger(L, 4);
    int r = (int)luaL_checkinteger(L, 5);
    bool clearScreen = luaDisplayClearArg(L, 6, false);
    bool fill  = lua_istable(L, 6) ? luaTableBool(L, 6, "fill",  false) : false;
    uint16_t color = lua_istable(L, 6)
        ? displayColorFromName(luaTableString(L, 6, "color", "black"), GxEPD_BLACK)
        : GxEPD_BLACK;
    if (clearScreen) g_luaCanvas.fillScreen(0);
    if (fill) g_luaCanvas.fillRoundRect(x, y, w, h, r, canvasColor(color));
    else      g_luaCanvas.drawRoundRect(x, y, w, h, r, canvasColor(color));
    luaFlushOrDefer();
    return 0;
}

static int luaOnionDisplayPixel(lua_State* L) {
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    bool clearScreen = luaDisplayClearArg(L, 3, false);
    uint16_t color = lua_istable(L, 3)
        ? displayColorFromName(luaTableString(L, 3, "color", "black"), GxEPD_BLACK)
        : GxEPD_BLACK;
    if (clearScreen) g_luaCanvas.fillScreen(0);
    g_luaCanvas.drawPixel(x, y, canvasColor(color));
    luaFlushOrDefer();
    return 0;
}

static int luaOnionKvList(lua_State* L) {
    lua_newtable(L);
    return 1;
}

// ── Timing helpers ────────────────────────────────────────────────────────────

static int luaOnionMillis(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)millis());
    return 1;
}

static int luaOnionTime(lua_State* L) {
    time_t t = time(nullptr);
    if (t < 1000000000L) t = 0;
    lua_pushinteger(L, (lua_Integer)t);
    return 1;
}

static int luaOnionTimeSynced(lua_State* L) {
    lua_pushboolean(L, sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED);
    return 1;
}

static int luaOnionDisplayBitmap(lua_State* L) {
    String name = luaL_checkstring(L, 1);
    int x = (int)luaL_optinteger(L, 2, 0);
    int y = (int)luaL_optinteger(L, 3, 0);
    bool clearScreen = lua_gettop(L) < 4 || lua_toboolean(L, 4);

    LoadedBitmap bitmap;
    String error;
    if (!loadStoredBitmap(name, bitmap, error)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, error.c_str());
        setLog(error);
        return 2;
    }

    if (x < 0) x = (display.width() - bitmap.width) / 2;
    if (y < 0) y = (display.height() - bitmap.height) / 2;
    renderBitmap(bitmap, x, y, clearScreen);
    lua_pushboolean(L, true);
    return 1;
}

static int luaOnionImages(lua_State* L) {
    lua_newtable(L);

    File root = SPIFFS.open("/");
    if (!root) return 1;

    int index = 1;
    File file = root.openNextFile();
    while (file) {
        String name = normalizedSpiffsPath(file.name());
        if (!file.isDirectory() && name.startsWith("/images_")) {
            name.remove(0, 8);
            if (validImageFileName(name)) {
                lua_pushstring(L, name.c_str());
                lua_rawseti(L, -2, index++);
            }
        }
        file = root.openNextFile();
    }

    return 1;
}

static bool luaButtonMaskForName(const char* name, uint8_t& mask) {
    for (const LuaButton& button : kLuaButtons) {
        if (strcmp(name, button.name) == 0) {
            mask = button.mask;
            return true;
        }
    }
    return false;
}

static int luaOnionButtonMask(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    uint8_t mask = 0;
    if (!luaButtonMaskForName(name, mask)) {
        lua_pushnil(L);
        lua_pushfstring(L, "Bad button name: %s", name);
        return 2;
    }
    lua_pushinteger(L, mask);
    return 1;
}

static int luaOnionButtons(lua_State* L) {
    uint8_t buttons = readButtons();
    lua_newtable(L);

    lua_pushinteger(L, buttons);
    lua_setfield(L, -2, "mask");

    for (const LuaButton& button : kLuaButtons) {
        lua_pushboolean(L, (buttons & button.mask) != 0);
        lua_setfield(L, -2, button.name);
    }

    return 1;
}

static int luaOnionSleep(lua_State* L) {
    uint32_t ms = (uint32_t)luaL_optinteger(L, 1, 0);
    if (ms > LUA_SLEEP_MAX_MS) ms = LUA_SLEEP_MAX_MS;
    delay(ms);
    return 0;
}

static int luaOnionGpioRead(lua_State* L) {
    int pin = (int)luaL_checkinteger(L, 1);
    if (!luaConfigureInputGpio(L, pin, 2)) return 2;

    lua_pushinteger(L, digitalRead(pin) == HIGH ? 1 : 0);
    return 1;
}

static int luaOnionGpioPoll(lua_State* L) {
    int pin = (int)luaL_checkinteger(L, 1);
    int target = (int)luaL_checkinteger(L, 2) ? 1 : 0;
    uint32_t timeoutMs = (uint32_t)luaL_optinteger(L, 3, 1000);
    uint32_t intervalMs = (uint32_t)luaL_optinteger(L, 4, 25);
    if (timeoutMs > LUA_GPIO_POLL_MAX_MS) timeoutMs = LUA_GPIO_POLL_MAX_MS;
    if (intervalMs < 1) intervalMs = 1;
    if (intervalMs > 1000) intervalMs = 1000;
    if (!luaConfigureInputGpio(L, pin, 5)) return 2;

    uint32_t start = millis();
    int value = digitalRead(pin) == HIGH ? 1 : 0;
    while ((uint32_t)(millis() - start) <= timeoutMs) {
        value = digitalRead(pin) == HIGH ? 1 : 0;
        if (value == target) {
            lua_pushboolean(L, true);
            lua_pushinteger(L, value);
            lua_pushinteger(L, (lua_Integer)(millis() - start));
            return 3;
        }
        delay(intervalMs);
    }

    lua_pushboolean(L, false);
    lua_pushinteger(L, value);
    lua_pushinteger(L, (lua_Integer)(millis() - start));
    return 3;
}

static int luaOnionEspNowStart(lua_State* L) {
    int channel = (int)luaL_optinteger(L, 1, 0);
    String error;
    if (!ensureEspNow(channel, error)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, error.c_str());
        return 2;
    }
    setLog("ESP-NOW ready");
    lua_pushboolean(L, true);
    return 1;
}

static int luaOnionEspNowStop(lua_State* L) {
    if (g_espnowStarted) {
        esp_now_unregister_recv_cb();
        esp_now_unregister_send_cb();
        esp_now_deinit();
        g_espnowStarted = false;
    }
    portENTER_CRITICAL(&g_espnowMux);
    g_espnowQueueHead = 0;
    g_espnowQueueCount = 0;
    portEXIT_CRITICAL(&g_espnowMux);
    lua_pushboolean(L, true);
    return 1;
}

static int luaOnionEspNowMac(lua_State* L) {
    uint8_t mac[6] = {};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    String macText = macToString(mac);
    lua_pushstring(L, macText.c_str());
    return 1;
}

static int luaOnionEspNowInfo(lua_State* L) {
    uint8_t mac[6] = {};
    uint8_t channel = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    esp_wifi_get_channel(&channel, &second);

    lua_newtable(L);
    lua_pushboolean(L, g_espnowStarted);
    lua_setfield(L, -2, "started");
    String macText = macToString(mac);
    lua_pushstring(L, macText.c_str());
    lua_setfield(L, -2, "mac");
    lua_pushinteger(L, channel);
    lua_setfield(L, -2, "channel");
    lua_pushinteger(L, (lua_Integer)g_espnowSent);
    lua_setfield(L, -2, "sent");
    lua_pushinteger(L, (lua_Integer)g_espnowReceived);
    lua_setfield(L, -2, "received");
    portENTER_CRITICAL(&g_espnowMux);
    lua_pushinteger(L, g_espnowQueueCount);
    portEXIT_CRITICAL(&g_espnowMux);
    lua_setfield(L, -2, "queued");
    return 1;
}

static int luaOnionEspNowSend(lua_State* L) {
    size_t len = 0;
    const char* payload = luaL_checklstring(L, 1, &len);
    if (len == 0 || len > ONION_ESPNOW_MAX_PAYLOAD) {
        lua_pushboolean(L, false);
        lua_pushfstring(L, "ESP-NOW payload must be 1-%d bytes", ONION_ESPNOW_MAX_PAYLOAD);
        return 2;
    }

    uint8_t mac[6];
    memcpy(mac, kEspNowBroadcastMac, sizeof(mac));
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
        if (!parseMacString(luaL_checkstring(L, 2), mac)) {
            lua_pushboolean(L, false);
            lua_pushstring(L, "Bad ESP-NOW MAC");
            return 2;
        }
    }

    String error;
    if (!ensureEspNow(0, error) || !espnowAddPeer(mac, 0, error)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, error.c_str());
        return 2;
    }

    esp_err_t rc = esp_now_send(mac, reinterpret_cast<const uint8_t*>(payload), len);
    if (rc != ESP_OK) {
        lua_pushboolean(L, false);
        lua_pushfstring(L, "ESP-NOW send failed %d", (int)rc);
        return 2;
    }
    lua_pushboolean(L, true);
    return 1;
}

static int luaOnionEspNowReceive(lua_State* L) {
    uint32_t timeoutMs = (uint32_t)luaL_optinteger(L, 1, 0);
    if (timeoutMs > LUA_ESPNOW_RECV_MAX_MS) timeoutMs = LUA_ESPNOW_RECV_MAX_MS;

    String error;
    if (!ensureEspNow(0, error)) {
        lua_pushnil(L);
        lua_pushstring(L, error.c_str());
        return 2;
    }

    uint32_t start = millis();
    EspNowQueuedMessage msg;
    while (!espnowQueuePop(msg)) {
        if ((uint32_t)(millis() - start) >= timeoutMs) {
            lua_pushnil(L);
            return 1;
        }
        delay(10);
    }

    lua_newtable(L);
    String macText = macToString(msg.mac);
    lua_pushstring(L, macText.c_str());
    lua_setfield(L, -2, "mac");
    lua_pushlstring(L, msg.payload, msg.len);
    lua_setfield(L, -2, "payload");
    lua_pushlstring(L, msg.payload, msg.len);
    lua_setfield(L, -2, "message");
    lua_pushinteger(L, msg.len);
    lua_setfield(L, -2, "len");
    lua_pushinteger(L, msg.rssi);
    lua_setfield(L, -2, "rssi");
    lua_pushinteger(L, (lua_Integer)msg.receivedAt);
    lua_setfield(L, -2, "received_at");
    return 1;
}

// onion.espnow_set_peer_key(mac, key16) enables radio AES-128 (LMK) for one
// unicast peer; key=nil reverts the peer to plaintext. Both sides must set the
// same key or unicast frames are silently dropped by the radio, so scripts
// should confirm the encrypted path still answers and fall back if it goes
// quiet.
static int luaOnionEspNowSetPeerKey(lua_State* L) {
    uint8_t mac[6];
    if (!parseMacString(luaL_checkstring(L, 1), mac)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "Bad ESP-NOW MAC");
        return 2;
    }
    if (memcmp(mac, kEspNowBroadcastMac, sizeof(mac)) == 0) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "Broadcast peer cannot be encrypted");
        return 2;
    }
    bool clear = lua_isnoneornil(L, 2);
    const char* key = nullptr;
    if (!clear) {
        size_t keyLen = 0;
        key = luaL_checklstring(L, 2, &keyLen);
        if (keyLen != ESP_NOW_KEY_LEN) {
            lua_pushboolean(L, false);
            lua_pushfstring(L, "ESP-NOW key must be %d bytes", ESP_NOW_KEY_LEN);
            return 2;
        }
    }

    String error;
    if (!ensureEspNow(0, error) || !espnowAddPeer(mac, 0, error)) {
        lua_pushboolean(L, false);
        lua_pushstring(L, error.c_str());
        return 2;
    }

    esp_now_peer_info_t peer = {};
    esp_err_t rc = esp_now_get_peer(mac, &peer);
    if (rc != ESP_OK) {
        lua_pushboolean(L, false);
        lua_pushfstring(L, "ESP-NOW get peer failed %d", (int)rc);
        return 2;
    }
    if (clear) {
        peer.encrypt = false;
        memset(peer.lmk, 0, sizeof(peer.lmk));
    } else {
        memcpy(peer.lmk, key, ESP_NOW_KEY_LEN);
        peer.encrypt = true;
    }
    rc = esp_now_mod_peer(&peer);
    if (rc != ESP_OK) {
        lua_pushboolean(L, false);
        lua_pushfstring(L, "ESP-NOW mod peer failed %d", (int)rc);
        return 2;
    }
    Serial.printf("[onion-os] ESP-NOW peer %s encrypt=%d\n",
                  macToString(peer.peer_addr).c_str(), peer.encrypt ? 1 : 0);
    lua_pushboolean(L, true);
    return 1;
}

// --- Lua KV store / crypto --------------------------------------------------
// Small script-facing primitives so downloadable Lua apps can keep state and
// do authenticated pairing without custom firmware: NVS key/value blobs in a
// namespace separate from the protected onion-os config, and SHA-256
// (libsodium, already linked for the wallet).

static Preferences g_luaPrefs;
static bool g_luaPrefsOpen = false;

static bool ensureLuaPrefs() {
    if (!g_luaPrefsOpen) g_luaPrefsOpen = g_luaPrefs.begin("lua-kv", false);
    return g_luaPrefsOpen;
}

// NVS limits keys to 15 chars; values are stored as binary-safe blobs.
static bool luaKvCheckKey(lua_State* L, const char** outKey) {
    size_t klen = 0;
    const char* key = luaL_checklstring(L, 1, &klen);
    if (klen == 0 || klen > 15) {
        lua_pushnil(L);
        lua_pushstring(L, "kv key must be 1-15 chars");
        return false;
    }
    *outKey = key;
    return true;
}

static int luaOnionKvSet(lua_State* L) {
    const char* key = nullptr;
    if (!luaKvCheckKey(L, &key)) return 2;
    if (!ensureLuaPrefs()) {
        lua_pushnil(L);
        lua_pushstring(L, "kv storage unavailable");
        return 2;
    }
    if (lua_isnoneornil(L, 2)) {
        g_luaPrefs.remove(key);
        lua_pushboolean(L, true);
        return 1;
    }
    size_t vlen = 0;
    const char* value = luaL_checklstring(L, 2, &vlen);
    if (vlen == 0 || vlen > LUA_KV_MAX_VALUE) {
        lua_pushnil(L);
        lua_pushfstring(L, "kv value must be 1-%d bytes", (int)LUA_KV_MAX_VALUE);
        return 2;
    }
    if (g_luaPrefs.putBytes(key, value, vlen) != vlen) {
        lua_pushnil(L);
        lua_pushstring(L, "kv write failed");
        return 2;
    }
    lua_pushboolean(L, true);
    return 1;
}

static int luaOnionKvGet(lua_State* L) {
    const char* key = nullptr;
    if (!luaKvCheckKey(L, &key)) return 2;
    if (!ensureLuaPrefs() || !g_luaPrefs.isKey(key)) {
        lua_pushnil(L);
        return 1;
    }
    size_t len = g_luaPrefs.getBytesLength(key);
    if (len == 0 || len > LUA_KV_MAX_VALUE) {
        lua_pushnil(L);
        return 1;
    }
    char buf[LUA_KV_MAX_VALUE];
    g_luaPrefs.getBytes(key, buf, len);
    lua_pushlstring(L, buf, len);
    return 1;
}

static int luaOnionKvDel(lua_State* L) {
    const char* key = nullptr;
    if (!luaKvCheckKey(L, &key)) return 2;
    if (!ensureLuaPrefs()) {
        lua_pushnil(L);
        lua_pushstring(L, "kv storage unavailable");
        return 2;
    }
    g_luaPrefs.remove(key);
    lua_pushboolean(L, true);
    return 1;
}

// onion.sha256(data) -> 32 raw bytes. Binary-safe input.
static int luaOnionSha256(lua_State* L) {
    size_t len = 0;
    const char* data = luaL_checklstring(L, 1, &len);
    unsigned char hash[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(hash, reinterpret_cast<const unsigned char*>(data), len);
    lua_pushlstring(L, reinterpret_cast<const char*>(hash), sizeof(hash));
    return 1;
}

// --- Lua WiFi association control (fixed-channel ESP-NOW) -------------------
// ESP-NOW shares the one radio with the WiFi STA. While associated to an AP
// the radio is pinned to that AP's channel, so badges that roamed to different
// APs (e.g. a multi-AP venue) land on different channels and cannot hear each
// other. onion.wifi_disconnect() frees the radio from the AP WITHOUT turning
// it off; the script can then onion.espnow_start(channel) to pin every badge
// to one agreed channel. Only the AP association (internet/MQTT) is dropped.
// onion.wifi_reconnect() restores normal connectivity (ensureWifi also undoes
// the LR PHY automatically once the script exits and the main loop resumes).

static int luaOnionWifiDisconnect(lua_State* L) {
    WiFi.setAutoReconnect(false); // stop the Arduino layer racing us back on
    esp_wifi_disconnect();        // leave the AP; radio stays up in STA mode
    // 802.11 LR (Espressif long-range PHY, ~250 kbps) gives ~10 dB better RX
    // sensitivity, roughly 2-4x ESP-NOW range badge-to-badge. It is only legal
    // while not associated (LR cannot talk to a normal AP); every reconnect
    // path restores B/G/N first via restoreWifiProtocol().
    esp_err_t lrRc = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR);
    if (lrRc != ESP_OK) {
        Serial.printf("[onion-os] LR protocol set failed %d; default PHY\n", (int)lrRc);
    }
    lua_pushboolean(L, true);
    return 1;
}

static int luaOnionWifiReconnect(lua_State* L) {
    restoreWifiProtocol();
    WiFi.setAutoReconnect(true);
    if (g_config.wifiSsid.length()) {
        WiFi.begin(g_config.wifiSsid.c_str(), g_config.wifiPassword.c_str());
    }
    lua_pushboolean(L, true);
    return 1;
}

static void registerOnionLua(lua_State* L) {
    lua_newtable(L);
    lua_pushcfunction(L, luaOnionLog);
    lua_setfield(L, -2, "log");
    lua_pushcfunction(L, luaOnionHardwareId);
    lua_setfield(L, -2, "hardware_id");
    lua_pushcfunction(L, luaOnionOnionId);
    lua_setfield(L, -2, "onion_id");
    lua_pushcfunction(L, luaOnionWallet);
    lua_setfield(L, -2, "wallet");
    lua_pushcfunction(L, luaOnionUsername);
    lua_setfield(L, -2, "username");
    lua_pushcfunction(L, luaOnionSecureRandom);
    lua_setfield(L, -2, "secure_random");
    lua_pushcfunction(L, luaOnionSha256);
    lua_setfield(L, -2, "sha256");
    lua_pushcfunction(L, luaOnionKvSet);
    lua_setfield(L, -2, "kv_set");
    lua_pushcfunction(L, luaOnionKvGet);
    lua_setfield(L, -2, "kv_get");
    lua_pushcfunction(L, luaOnionKvDel);
    lua_setfield(L, -2, "kv_del");
    lua_pushcfunction(L, luaOnionHttpGet);
    lua_setfield(L, -2, "http_get");
    lua_pushcfunction(L, luaOnionHttpPost);
    lua_setfield(L, -2, "http_post");
    lua_pushcfunction(L, luaOnionMqttConnected);
    lua_setfield(L, -2, "mqtt_connected");
    lua_pushcfunction(L, luaOnionMqttSubscribe);
    lua_setfield(L, -2, "mqtt_subscribe");
    lua_pushcfunction(L, luaOnionMqttUnsubscribe);
    lua_setfield(L, -2, "mqtt_unsubscribe");
    lua_pushcfunction(L, luaOnionMqttPublish);
    lua_setfield(L, -2, "mqtt_publish");
    lua_pushcfunction(L, luaOnionMqttReceive);
    lua_setfield(L, -2, "mqtt_receive");
    lua_pushcfunction(L, luaOnionMqttInfo);
    lua_setfield(L, -2, "mqtt_info");
    lua_pushcfunction(L, luaOnionSubghzBegin);
    lua_setfield(L, -2, "subghz_begin");
    lua_pushcfunction(L, luaOnionSubghzTransmit);
    lua_setfield(L, -2, "subghz_transmit");
    lua_pushcfunction(L, luaOnionSubghzReceive);
    lua_setfield(L, -2, "subghz_receive");
    lua_pushcfunction(L, luaOnionSubghzSetFrequency);
    lua_setfield(L, -2, "subghz_set_frequency");
    lua_pushcfunction(L, luaOnionSubghzRssi);
    lua_setfield(L, -2, "subghz_rssi");
    lua_pushcfunction(L, luaOnionSubghzRawRecord);
    lua_setfield(L, -2, "subghz_raw_record");
    lua_pushcfunction(L, luaOnionSubghzRawReplay);
    lua_setfield(L, -2, "subghz_raw_replay");
    lua_pushcfunction(L, luaOnionSubghzRawInfo);
    lua_setfield(L, -2, "subghz_raw_info");
    lua_pushcfunction(L, luaOnionSubghzRawSave);
    lua_setfield(L, -2, "subghz_raw_save");
    lua_pushcfunction(L, luaOnionSubghzRawLoad);
    lua_setfield(L, -2, "subghz_raw_load");
    lua_pushcfunction(L, luaOnionSubghzInfo);
    lua_setfield(L, -2, "subghz_info");
    lua_pushcfunction(L, luaOnionSubghzEnd);
    lua_setfield(L, -2, "subghz_end");
    lua_pushcfunction(L, luaOnionSoundSpeakerBegin);
    lua_setfield(L, -2, "sound_speaker_begin");
    lua_pushcfunction(L, luaOnionSoundPlayTone);
    lua_setfield(L, -2, "sound_play_tone");
    lua_pushcfunction(L, luaOnionSoundPlay);
    lua_setfield(L, -2, "sound_play");
    lua_pushcfunction(L, luaOnionSoundSpeakerEnd);
    lua_setfield(L, -2, "sound_speaker_end");
    lua_pushcfunction(L, luaOnionSoundMicBegin);
    lua_setfield(L, -2, "sound_mic_begin");
    lua_pushcfunction(L, luaOnionSoundMicRead);
    lua_setfield(L, -2, "sound_mic_read");
    lua_pushcfunction(L, luaOnionSoundMicLevel);
    lua_setfield(L, -2, "sound_mic_level");
    lua_pushcfunction(L, luaOnionSoundMicEnd);
    lua_setfield(L, -2, "sound_mic_end");
    lua_pushcfunction(L, luaOnionDisplaySize);
    lua_setfield(L, -2, "display_size");
    lua_pushcfunction(L, luaOnionClearDisplay);
    lua_setfield(L, -2, "clear_display");
    lua_pushcfunction(L, luaOnionReleaseDisplay);
    lua_setfield(L, -2, "release_display");
    lua_pushcfunction(L, luaOnionDisplayText);
    lua_setfield(L, -2, "display_text");
    lua_pushcfunction(L, luaOnionDisplayLines);
    lua_setfield(L, -2, "display_lines");
    lua_pushcfunction(L, luaOnionDisplayLine);
    lua_setfield(L, -2, "display_line");
    lua_pushcfunction(L, luaOnionDisplayRect);
    lua_setfield(L, -2, "display_rect");
    lua_pushcfunction(L, luaOnionDisplayBuffer);
    lua_setfield(L, -2, "display_buffer");
    lua_pushcfunction(L, luaOnionDisplayBegin);
    lua_setfield(L, -2, "display_begin");
    lua_pushcfunction(L, luaOnionDisplayCommit);
    lua_setfield(L, -2, "display_commit");
    lua_pushcfunction(L, luaOnionDisplayFlush);
    lua_setfield(L, -2, "display_flush");
    lua_pushcfunction(L, luaOnionDisplayCircle);
    lua_setfield(L, -2, "display_circle");
    lua_pushcfunction(L, luaOnionDisplayTriangle);
    lua_setfield(L, -2, "display_triangle");
    lua_pushcfunction(L, luaOnionDisplayRoundRect);
    lua_setfield(L, -2, "display_round_rect");
    lua_pushcfunction(L, luaOnionDisplayPixel);
    lua_setfield(L, -2, "display_pixel");
    lua_pushcfunction(L, luaOnionDisplayBitmap);
    lua_setfield(L, -2, "display_bitmap");
    lua_pushcfunction(L, luaOnionKvDel);
    lua_setfield(L, -2, "kv_delete");
    lua_pushcfunction(L, luaOnionKvList);
    lua_setfield(L, -2, "kv_list");
    lua_pushcfunction(L, luaOnionMillis);
    lua_setfield(L, -2, "millis");
    lua_pushcfunction(L, luaOnionTime);
    lua_setfield(L, -2, "time");
    lua_pushcfunction(L, luaOnionTimeSynced);
    lua_setfield(L, -2, "time_synced");
    lua_pushcfunction(L, luaOnionImages);
    lua_setfield(L, -2, "images");
    lua_pushcfunction(L, luaOnionButtons);
    lua_setfield(L, -2, "buttons");
    lua_pushcfunction(L, luaOnionButtonMask);
    lua_setfield(L, -2, "button_mask");
    lua_pushcfunction(L, luaOnionSleep);
    lua_setfield(L, -2, "sleep");
    lua_pushcfunction(L, luaOnionGpioRead);
    lua_setfield(L, -2, "gpio_read");
    lua_pushcfunction(L, luaOnionGpioPoll);
    lua_setfield(L, -2, "gpio_poll");
    lua_pushcfunction(L, luaOnionEspNowStart);
    lua_setfield(L, -2, "espnow_start");
    lua_pushcfunction(L, luaOnionEspNowStop);
    lua_setfield(L, -2, "espnow_stop");
    lua_pushcfunction(L, luaOnionEspNowMac);
    lua_setfield(L, -2, "espnow_mac");
    lua_pushcfunction(L, luaOnionEspNowInfo);
    lua_setfield(L, -2, "espnow_info");
    lua_pushcfunction(L, luaOnionEspNowSend);
    lua_setfield(L, -2, "espnow_send");
    lua_pushcfunction(L, luaOnionEspNowReceive);
    lua_setfield(L, -2, "espnow_receive");
    lua_pushcfunction(L, luaOnionEspNowSetPeerKey);
    lua_setfield(L, -2, "espnow_set_peer_key");
    lua_pushcfunction(L, luaOnionWifiDisconnect);
    lua_setfield(L, -2, "wifi_disconnect");
    lua_pushcfunction(L, luaOnionWifiReconnect);
    lua_setfield(L, -2, "wifi_reconnect");
    lua_setglobal(L, "onion");
}

static void* luaHeapAllocator(void*, void* ptr, size_t, size_t nsize) {
    if (nsize == 0) {
        heap_caps_free(ptr);
        return nullptr;
    }
    void* out = heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) out = heap_caps_realloc(ptr, nsize, MALLOC_CAP_8BIT);
    return out;
}

static lua_State* newLuaState() {
    return lua_newstate(luaHeapAllocator, nullptr, esp_random());
}

static void cleanupLuaRuntime() {
    luaMqttResetSubs();
    moduleShutdownActive();
    if (g_luaFramePending) { g_luaFramePending = false; refreshLuaCanvas(); }
    g_luaDeferFlush = false;
}

static bool finishLuaRun(lua_State* L, const String& name, int status, const String& logBeforeRun) {
    if (status == LUA_OK) status = lua_pcall(L, 0, 0, 0);
    if (status != LUA_OK) {
        const char* luaErr = lua_tostring(L, -1);
        String err = luaErr ? String(luaErr) : String("unknown Lua error");
        g_lastLuaError = err;
        lua_close(L);
        cleanupLuaRuntime();
        // Full error to serial; the e-paper status line only fits a stub.
        Serial.printf("[onion-os] Lua error in %s: %s\n", name.c_str(), err.c_str());
        setLog("Lua error: " + clipped(err, 22));
        return false;
    }

    lua_close(L);
    cleanupLuaRuntime();
    if (g_luaDisplayActive) {
        Serial.printf("[onion-os] Lua ran %s\n", name.c_str());
    } else {
        if (g_log == logBeforeRun) setLog("Lua ran " + name);
    }
    return true;
}

static bool prepareLuaState(lua_State*& L) {
    L = newLuaState();
    if (!L) {
        g_lastLuaError = "Lua state failed";
        setLog("Lua state failed");
        return false;
    }
    luaL_openlibs(L);
    registerOnionLua(L);
    return true;
}

static bool runLuaBuffer(const char* source, size_t sourceLen, const String& name) {
    g_lastLuaError = "";
    lua_State* L = nullptr;
    if (!prepareLuaState(L)) return false;
    String logBeforeRun = g_log;
    int status = luaL_loadbuffer(L, source, sourceLen, name.c_str());
    return finishLuaRun(L, name, status, logBeforeRun);
}

static bool runLuaSource(const String& source, const String& name) {
    return runLuaBuffer(source.c_str(), source.length(), name);
}

struct LuaFileReader {
    File* file;
    char buffer[512];
};

static const char* luaFileReader(lua_State*, void* data, size_t* size) {
    LuaFileReader* reader = (LuaFileReader*)data;
    if (!reader || !reader->file || !*reader->file) {
        *size = 0;
        return nullptr;
    }
    size_t n = reader->file->read((uint8_t*)reader->buffer, sizeof(reader->buffer));
    if (n == 0) {
        *size = 0;
        return nullptr;
    }
    *size = n;
    return reader->buffer;
}

static bool runStoredScript(const String& path) {
    g_lastLuaError = "";
    File file = SPIFFS.open(path, FILE_READ);
    if (!file) {
        g_lastLuaError = "Lua script missing: " + path;
        setLog("Lua script missing");
        return false;
    }
    size_t size = file.size();
    if (size > MAX_SCRIPT_BYTES) {
        file.close();
        g_lastLuaError = "Lua script too large: " + String(size) + " bytes";
        setLog("Lua script too large");
        return false;
    }

    lua_State* L = nullptr;
    if (!prepareLuaState(L)) {
        file.close();
        return false;
    }

    String logBeforeRun = g_log;
    LuaFileReader reader = {&file, {}};
    int status = lua_load(L, luaFileReader, &reader, path.c_str(), nullptr);
    file.close();
    return finishLuaRun(L, path, status, logBeforeRun);
}

static void runScriptByName(const String& name) {
    if (!name.length() || name.indexOf('/') >= 0) {
        setLog("Bad script name");
        return;
    }
    runStoredScript("/scripts_" + name);
}

static bool validScriptFileName(const String& name) {
    return validAssetFileName(name, ".lua");
}

static bool validStoredScriptPath(const String& path) {
    if (!path.startsWith("/scripts_") || !path.endsWith(".lua")) return false;
    return validScriptFileName(storedScriptDisplayName(path));
}

static bool deleteStoredScript(const String& path) {
    if (!validStoredScriptPath(path)) {
        setLog("Bad script path");
        return false;
    }
    String name = storedScriptDisplayName(path);
    if (!SPIFFS.remove(path)) {
        setLog("Lua delete failed");
        return false;
    }
    setLog("Deleted " + clipped(name, 20));
    return true;
}

static void deleteScriptByName(const String& name) {
    if (!validScriptFileName(name)) {
        setLog("Bad script name");
        return;
    }
    deleteStoredScript("/scripts_" + name);
}

static String pushedScriptFileName() {
    if (validScriptFileName(g_luaPrompt.fileName)) return g_luaPrompt.fileName;
    String suffix = g_luaPrompt.scriptId.length() ? g_luaPrompt.scriptId : g_luaPrompt.requestId;
    String safe;
    safe.reserve(64);
    for (size_t i = 0; i < suffix.length() && safe.length() < 64; ++i) {
        char ch = suffix[i];
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
        safe += ok ? ch : '_';
    }
    if (!safe.length()) safe = "script";
    return "pushed_" + safe + ".lua";
}

static bool copySpiffsFile(const String& srcPath, const String& dstPath) {
    File src = SPIFFS.open(srcPath, FILE_READ);
    if (!src) return false;
    SPIFFS.remove(dstPath);
    File dst = SPIFFS.open(dstPath, FILE_WRITE);
    if (!dst) {
        src.close();
        return false;
    }
    uint8_t buf[512];
    bool ok = true;
    while (src.available()) {
        size_t n = src.read(buf, sizeof(buf));
        if (n == 0) break;
        if (dst.write(buf, n) != n) {
            ok = false;
            break;
        }
    }
    src.close();
    dst.close();
    if (!ok) SPIFFS.remove(dstPath);
    return ok;
}

static bool stringIsDigits(const String& value) {
    if (!value.length()) return false;
    for (size_t i = 0; i < value.length(); i++) {
        if (value[i] < '0' || value[i] > '9') return false;
    }
    return true;
}

static String luaInstallError() {
    if (!g_lastLuaError.length()) return "Lua runtime error";

    int firstColon = g_lastLuaError.indexOf(':');
    int secondColon = firstColon >= 0 ? g_lastLuaError.indexOf(':', firstColon + 1) : -1;
    if (firstColon >= 0 && secondColon > firstColon) {
        String line = g_lastLuaError.substring(firstColon + 1, secondColon);
        if (stringIsDigits(line)) {
            String message = g_lastLuaError.substring(secondColon + 1);
            message.trim();
            return "Lua line " + line + ": " + clipped(message, 160);
        }
    }

    return "Lua: " + clipped(g_lastLuaError, 180);
}

static bool installAndRunPushedScript(String& error) {
    if (!g_luaPrompt.requestId.length()) {
        error = "Missing Lua request";
        return false;
    }
    bool fileBacked = g_luaPrompt.codePath.length() > 0;
    bool httpBacked = g_luaPrompt.downloadUrl.length() > 0;
    if (!httpBacked && !fileBacked && !g_luaPrompt.code.length()) {
        error = "Empty Lua script";
        return false;
    }
    if ((!httpBacked && !fileBacked && g_luaPrompt.code.length() > MAX_SCRIPT_BYTES) ||
        g_luaPrompt.sizeBytes > MAX_SCRIPT_BYTES) {
        error = "Lua script too large";
        return false;
    }

    String name = pushedScriptFileName();
    String path = "/scripts_" + name;
    if (httpBacked) {
        String url = resolveServerUrl(g_luaPrompt.downloadUrl);
        SPIFFS.remove(path);
        if (!downloadScriptFile(url, path)) {
            error = "Lua download failed";
            return false;
        }
        if (!runStoredScript(path)) {
            error = luaInstallError();
            return false;
        }
        return true;
    }

    if (fileBacked) {
        File src = SPIFFS.open(g_luaPrompt.codePath, FILE_READ);
        if (!src) {
            error = "Lua script missing";
            return false;
        }
        size_t scriptSize = src.size();
        src.close();
        if (scriptSize == 0) {
            error = "Empty Lua script";
            return false;
        }
        if (scriptSize > MAX_SCRIPT_BYTES) {
            error = "Lua script too large";
            return false;
        }
        SPIFFS.remove(path);
        if (!SPIFFS.rename(g_luaPrompt.codePath, path) && !copySpiffsFile(g_luaPrompt.codePath, path)) {
            error = "Lua write failed";
            return false;
        }
        if (!runStoredScript(path)) {
            error = luaInstallError();
            return false;
        }
        return true;
    }

    File file = SPIFFS.open(path, FILE_WRITE);
    if (!file) {
        error = "Lua write failed";
        return false;
    }
    size_t written = file.print(g_luaPrompt.code);
    file.close();
    if (written != g_luaPrompt.code.length()) {
        SPIFFS.remove(path);
        error = "Lua write short";
        return false;
    }

    if (!runLuaSource(g_luaPrompt.code, name)) {
        error = luaInstallError();
        return false;
    }
    return true;
}

static bool sendLuaPushResponse(bool approved) {
    if (!g_identity.onionId || !g_luaPrompt.requestId.length()) {
        setLog("No Lua push active");
        return false;
    }

    String error;
    bool accepted = approved;
    if (approved && !installAndRunPushedScript(error)) {
        accepted = false;
    }
    if (!approved) error = "User denied";

    String body = "{\"onionId\":" + String((unsigned long long)g_identity.onionId) +
        ",\"requestId\":\"" + jsonEscape(g_luaPrompt.requestId) +
        "\",\"approved\":" + String(accepted ? "true" : "false");
    if (!accepted && error.length()) {
        body += ",\"error\":\"" + jsonEscape(error) + "\"";
    }
    body += "}";

    bool sentOverMqtt = publishMqtt(topic("badge/" + String((unsigned long long)g_identity.onionId) + "/lua/response"), body);
    String response;
    int code = sentOverMqtt ? 200 : httpPostJson("/api/badge/lua-response", body, &response);
    if (code >= 200 && code < 300) {
        setLog(accepted ? "Lua installed" : (error.length() ? error : "Lua denied"));
        g_luaPrompt.active = false;
        g_luaPrompt.code = "";
        g_luaPrompt.downloadUrl = "";
        if (g_luaPrompt.codePath.length()) {
            SPIFFS.remove(g_luaPrompt.codePath);
            g_luaPrompt.codePath = "";
        }
        g_screen = SCREEN_STATUS;
    } else {
        setLog("Lua response HTTP " + String(code));
    }
    return code >= 200 && code < 300;
}

static void syncScripts() {
    if (!g_config.scriptManifestUrl.length()) {
        setLog("No script manifest URL");
        return;
    }
    if (!ensureWifi()) return;

    String payload;
    int code = httpGetString(g_config.scriptManifestUrl, &payload);

    if (code < 200 || code >= 300) {
        setLog("Manifest GET failed " + String(code));
        return;
    }

    cJSON* root = cJSON_Parse(payload.c_str());
    cJSON* scripts = root ? cJSON_GetObjectItemCaseSensitive(root, "scripts") : nullptr;
    cJSON* images = root ? cJSON_GetObjectItemCaseSensitive(root, "images") : nullptr;
    if (!cJSON_IsArray(scripts) && !cJSON_IsArray(images)) {
        if (root) cJSON_Delete(root);
        setLog("Bad script manifest");
        return;
    }

    int count = 0;
    if (cJSON_IsArray(scripts)) {
        cJSON* script = nullptr;
        cJSON_ArrayForEach(script, scripts) {
            String name = jsonString(script, "name");
            String url = jsonString(script, "url");
            bool autorun = jsonBool(script, "autorun", false);
            if (!validScriptFileName(name) || !url.length()) continue;
            String path = "/scripts_" + name;
            if (downloadScriptFile(url, path)) {
                count++;
                if (autorun) runStoredScript(path);
            }
        }
    }
    int imageCount = 0;
    if (cJSON_IsArray(images)) {
        cJSON* image = nullptr;
        cJSON_ArrayForEach(image, images) {
            String name = jsonString(image, "name");
            String url = jsonString(image, "url");
            if (!validImageFileName(name) || !url.length()) continue;
            if (downloadImageFile(url, imagePathForName(name))) imageCount++;
        }
    }
    cJSON_Delete(root);
    setLog("Synced S:" + String(count) + " I:" + String(imageCount));
}

static void printHelp() {
    Serial.println();
    Serial.println("Onion OS serial commands:");
    Serial.println("  api-key <badge_api_key>");
    Serial.println("  mqtt-auth [username] [password] [prefix]");
    Serial.println("  scripts-url <manifest_url>");
    Serial.println("  module <L1|L2|R>");
    Serial.println("  wallet");
    Serial.println("  keygen confirm");
    Serial.println("  handshake");
    Serial.println("  scripts");
    Serial.println("  run <script_name.lua>");
    Serial.println("  delete <script_name.lua>");
    Serial.println("  state");
    Serial.println("  help");
    Serial.println();
}

static std::vector<String> splitCommand(const String& line) {
    std::vector<String> parts;
    int start = 0;
    while (start < (int)line.length()) {
        while (start < (int)line.length() && line[start] == ' ') start++;
        if (start >= (int)line.length()) break;
        int end = line.indexOf(' ', start);
        if (end < 0) end = line.length();
        parts.push_back(line.substring(start, end));
        start = end + 1;
    }
    return parts;
}

static void handleSerial() {
    static String line;
    while (Serial.available()) {
        char ch = (char)Serial.read();
        if (ch == '\r') continue;
        if (ch != '\n') {
            line += ch;
            continue;
        }

        line.trim();
        std::vector<String> args = splitCommand(line);
        line = "";
        if (args.empty()) return;

        if (args[0] == "wifi") {
            setLog("WiFi is hardcoded");
        } else if (args[0] == "server") {
            g_config.serverBaseUrl = ONION_HARDCODED_SERVER_BASE_URL;
            if (args.size() >= 3) {
                g_config.badgeApiKey = args[2];
                saveConfigValue("api_key", g_config.badgeApiKey);
                setLog("API key saved; URL hardcoded");
            } else {
                setLog("Server URL is hardcoded");
            }
        } else if (args[0] == "api-key" && args.size() >= 2) {
            g_config.badgeApiKey = args[1];
            saveConfigValue("api_key", g_config.badgeApiKey);
            setLog("API key saved");
        } else if (args[0] == "mqtt" || args[0] == "mqtt-auth") {
            g_config.mqttUri = ONION_HARDCODED_MQTT_URI;
            size_t firstAuthArg = 1;
            if (args[0] == "mqtt" && args.size() >= 2 &&
                (args[1].indexOf("://") >= 0 || args[1].indexOf(':') >= 0)) {
                firstAuthArg = 2;
            }
            g_config.mqttUsername = args.size() > firstAuthArg ? args[firstAuthArg] : "";
            g_config.mqttPassword = args.size() > firstAuthArg + 1 ? args[firstAuthArg + 1] : "";
            g_config.mqttTopicPrefix = args.size() > firstAuthArg + 2 ? args[firstAuthArg + 2] : "oniondao";
            g_prefs.remove("mqtt_uri");
            saveConfigValue("mqtt_user", g_config.mqttUsername);
            saveConfigValue("mqtt_pass", g_config.mqttPassword);
            saveConfigValue("mqtt_prefix", g_config.mqttTopicPrefix);
            if (g_mqtt) {
                esp_mqtt_client_stop(g_mqtt);
                esp_mqtt_client_destroy(g_mqtt);
                g_mqtt = nullptr;
                g_mqttConnected = false;
            }
            setLog("MQTT auth saved; URL hardcoded");
        } else if (args[0] == "scripts-url" && args.size() >= 2) {
            g_config.scriptManifestUrl = args[1];
            saveConfigValue("script_url", g_config.scriptManifestUrl);
            setLog("Script URL saved");
        } else if (args[0] == "module" && args.size() >= 2) {
            String variant = args[1];
            variant.toUpperCase();
            if (variant == "L1" || variant == "L2" || variant == "R") {
                g_config.moduleVariant = variant;
                saveConfigValue("mod_variant", variant);
                setLog("Module variant " + variant);
            } else {
                setLog("Variant must be L1, L2, or R");
            }
        } else if (args[0] == "wallet") {
            String keyError;
            if (loadOrCreateSolanaKey(false, keyError)) {
                Serial.printf("wallet=%s\n", g_identity.solanaPublicKey.c_str());
                setLog("Wallet ready");
            } else {
                Serial.printf("wallet_error=%s\n", keyError.c_str());
                setLog(keyError);
            }
        } else if (args[0] == "keygen" && args.size() >= 2 && args[1] == "confirm") {
            if (g_identity.linked) {
                setLog("Refusing linked key rotation");
            } else {
                clearSolanaKey();
                String keyError;
                if (loadOrCreateSolanaKey(true, keyError)) {
                    Serial.printf("wallet=%s\n", g_identity.solanaPublicKey.c_str());
                    setLog("Wallet rotated");
                } else {
                    Serial.printf("wallet_error=%s\n", keyError.c_str());
                    setLog(keyError);
                }
            }
        } else if (args[0] == "handshake") {
            doHttpHandshake();
            doMqttHandshake();
        } else if (args[0] == "scripts") {
            syncScripts();
        } else if (args[0] == "run" && args.size() >= 2) {
            runScriptByName(args[1]);
        } else if ((args[0] == "delete" || args[0] == "rm") && args.size() >= 2) {
            deleteScriptByName(args[1]);
        } else if (args[0] == "state") {
            Serial.printf("hardwareId=%s\n", g_identity.hardwareId.c_str());
            Serial.printf("onionId=%llu\n", (unsigned long long)g_identity.onionId);
            Serial.printf("status=%s\n", g_identity.status.c_str());
            Serial.printf("username=%s\n", g_identity.username.c_str());
            Serial.printf("onions=%s\n", g_identity.onionCount.c_str());
            Serial.printf("wallet=%s\n", g_identity.solanaPublicKey.c_str());
            Serial.printf("wifi=%s\n", g_config.wifiSsid.c_str());
            Serial.printf("server=%s\n", g_config.serverBaseUrl.c_str());
            Serial.printf("mqtt=%s\n", g_config.mqttUri.c_str());
            Serial.printf("module=%s\n", g_config.moduleVariant.c_str());
        } else {
            printHelp();
        }
    }
}

static void handleButtons() {
    uint32_t now = millis();
    if (now - g_lastButtonPoll < 50) return;
    g_lastButtonPoll = now;

    uint8_t buttons = readButtons();
    uint8_t pressed = buttons & ~g_lastButtons;
    g_lastButtons = buttons;
    if (!pressed) return;

    if (g_luaDisplayActive) {
        g_luaDisplayActive = false;
        g_forceFullRefresh = true;  // guarantee clean full refresh over Lua content
        g_screen = SCREEN_STATUS;
        g_needsRedraw = true;
        return;
    }

    if (g_screen == SCREEN_LINK_PROMPT) {
        if (pressed & BTN_SELECT) sendLinkResponse(true);
        if (pressed & BTN_CANCEL) sendLinkResponse(false);
    } else if (g_screen == SCREEN_TX_PROMPT) {
        if (pressed & BTN_SELECT) sendTransactionResponse(true);
        if (pressed & BTN_CANCEL) sendTransactionResponse(false);
    } else if (g_screen == SCREEN_LUA_PROMPT) {
        if (pressed & BTN_SELECT) sendLuaPushResponse(true);
        if (pressed & BTN_CANCEL) sendLuaPushResponse(false);
    } else if (g_screen == SCREEN_CHECKIN_PROMPT) {
        if (pressed & BTN_SELECT) sendCheckInApproval(true);
        if (pressed & BTN_CANCEL) sendCheckInApproval(false);
    } else if (g_screen == SCREEN_CHECKIN_RESULT) {
        if (pressed & (BTN_SELECT | BTN_CANCEL)) {
            g_screen = SCREEN_STATUS;
        }
    } else if (g_screen == SCREEN_DELETE_CONFIRM) {
        if (pressed & BTN_CANCEL) {
            g_screen = SCREEN_SCRIPT_EXPLORER;
        } else if (pressed & BTN_LEFT) {
            g_deleteChoice = false;
        } else if (pressed & BTN_RIGHT) {
            g_deleteChoice = true;
        } else if (pressed & BTN_SELECT) {
            if (g_deleteChoice) {
                deleteStoredScript(g_pendingDeletePath);
                refreshScriptList();
            }
            g_screen = SCREEN_SCRIPT_EXPLORER;
        }
    } else if (g_screen == SCREEN_SCRIPT_EXPLORER) {
        if (pressed & BTN_CANCEL) {
            g_screen = SCREEN_STATUS;
        } else if ((pressed & BTN_LEFT) && g_scriptSelection > 0 && g_scriptSelection <= (int)g_scripts.size()) {
            g_pendingDeletePath = g_scripts[g_scriptSelection - 1];
            g_deleteChoice = false;
            g_screen = SCREEN_DELETE_CONFIRM;
        } else if (pressed & BTN_RIGHT) {
            syncScripts();
            refreshScriptList();
        } else if ((pressed & BTN_SELECT) && g_scriptSelection == 0) {
            syncScripts();
            refreshScriptList();
        } else if ((pressed & BTN_SELECT) && g_scriptSelection <= (int)g_scripts.size()) {
            runStoredScript(g_scripts[g_scriptSelection - 1]);
        } else {
            if ((pressed & BTN_UP) && g_scriptSelection > 0) g_scriptSelection--;
            if ((pressed & BTN_DOWN) && g_scriptSelection < (int)g_scripts.size()) g_scriptSelection++;
        }
    } else if (g_screen == SCREEN_SETTINGS) {
        if (pressed & BTN_CANCEL) {
            g_screen = SCREEN_STATUS;
        } else if (pressed & (BTN_UP | BTN_DOWN)) {
            g_settingsSel = (g_settingsSel + 1) % 2;
        } else if (pressed & BTN_SELECT) {
            if (g_settingsSel == 0) {
                g_wifiOverviewSel = 0;
                g_screen = SCREEN_WIFI_OVERVIEW;
            }
        }
    } else if (g_screen == SCREEN_WIFI_OVERVIEW) {
        if (pressed & BTN_CANCEL) {
            g_screen = SCREEN_SETTINGS;
        } else if (pressed & BTN_UP) {
            g_wifiOverviewSel = (g_wifiOverviewSel + 2) % 3;
        } else if (pressed & BTN_DOWN) {
            g_wifiOverviewSel = (g_wifiOverviewSel + 1) % 3;
        } else if (pressed & BTN_SELECT) {
            if (g_wifiOverviewSel == 0) {
                g_lastWifiAttempt = millis(); // prevent background reconnect during scan
                startWifiScan();
                g_screen = SCREEN_WIFI_SCANNING;
            } else if (g_wifiOverviewSel == 1) {
                WiFi.disconnect();
                setLog("WiFi disconnected");
            } else {
                g_screen = SCREEN_SETTINGS;
            }
        }
    } else if (g_screen == SCREEN_WIFI_SCANNING) {
        if (pressed & BTN_CANCEL) {
            g_wifiWorkerResult.store(WIFI_WORKER_IDLE);
            g_screen = SCREEN_WIFI_OVERVIEW;
        }
    } else if (g_screen == SCREEN_WIFI_LIST) {
        if (pressed & BTN_CANCEL) {
            g_screen = SCREEN_WIFI_OVERVIEW;
        } else if (pressed & BTN_UP) {
            if (g_wifiListSel > 0) g_wifiListSel--;
        } else if (pressed & BTN_DOWN) {
            if (g_wifiListSel + 1 < (int)g_wifiNetworks.size()) g_wifiListSel++;
        } else if ((pressed & BTN_SELECT) && !g_wifiNetworks.empty()) {
            g_wifiConnectSsid = String(g_wifiNetworks[g_wifiListSel].ssid);
            if (!g_wifiNetworks[g_wifiListSel].secured) {
                g_wifiPassBuf[0] = '\0';
                startWifiConnect(g_wifiConnectSsid.c_str(), "");
                g_screen = SCREEN_WIFI_CONNECTING;
            } else {
                g_wifiPassLen = 0;
                memset(g_wifiPassBuf, 0, sizeof(g_wifiPassBuf));
                g_kbRow = 1; g_kbCol = 0; g_kbCaps = false;
                g_screen = SCREEN_WIFI_PASSWORD;
            }
        }
    } else if (g_screen == SCREEN_WIFI_PASSWORD) {
        if (pressed & BTN_UP) {
            g_kbRow = (g_kbRow + kKbTotalRows - 1) % kKbTotalRows;
            int maxCol = kbRowLen(g_kbRow) - 1;
            if (g_kbCol > maxCol) g_kbCol = maxCol;
        } else if (pressed & BTN_DOWN) {
            g_kbRow = (g_kbRow + 1) % kKbTotalRows;
            int maxCol = kbRowLen(g_kbRow) - 1;
            if (g_kbCol > maxCol) g_kbCol = maxCol;
        } else if (pressed & BTN_LEFT) {
            int len = kbRowLen(g_kbRow);
            g_kbCol = (g_kbCol + len - 1) % len;
        } else if (pressed & BTN_RIGHT) {
            int len = kbRowLen(g_kbRow);
            g_kbCol = (g_kbCol + 1) % len;
        } else if (pressed & BTN_SELECT) {
            if (g_kbRow == 5) {
                if (g_kbCol == 0) {
                    g_kbCaps = !g_kbCaps;
                } else if (g_kbCol == 1 && g_wifiPassLen < 64) {
                    g_wifiPassBuf[g_wifiPassLen++] = ' ';
                } else if (g_kbCol == 2) {
                    if (g_wifiPassLen > 0) g_wifiPassLen--;
                } else if (g_kbCol == 3) {
                    g_wifiPassBuf[g_wifiPassLen] = '\0';
                    startWifiConnect(g_wifiConnectSsid.c_str(), g_wifiPassBuf);
                    g_screen = SCREEN_WIFI_CONNECTING;
                }
            } else if (g_wifiPassLen < 64) {
                const char* rowStr = g_kbCaps ? kKbCaps[g_kbRow] : kKbNormal[g_kbRow];
                g_wifiPassBuf[g_wifiPassLen++] = rowStr[g_kbCol];
            }
        } else if (pressed & BTN_CANCEL) {
            if (g_wifiPassLen > 0) {
                g_wifiPassLen--;
            } else {
                g_screen = SCREEN_WIFI_LIST;
            }
        }
    } else if (g_screen == SCREEN_WIFI_CONNECTING) {
        if (pressed & BTN_CANCEL) {
            g_wifiWorkerResult.store(WIFI_WORKER_IDLE);
            WiFi.disconnect();
            g_screen = SCREEN_WIFI_OVERVIEW;
        }
    } else if (g_screen == SCREEN_WIFI_RESULT) {
        if (pressed & (BTN_SELECT | BTN_CANCEL)) {
            g_wifiOverviewSel = 0;
            g_screen = SCREEN_WIFI_OVERVIEW;
        }
    } else {
        if (pressed & BTN_UP) {
            g_homeSelection = (g_homeSelection + HOME_ITEM_COUNT - 1) % HOME_ITEM_COUNT;
        }
        if (pressed & BTN_DOWN) {
            g_homeSelection = (g_homeSelection + 1) % HOME_ITEM_COUNT;
        }
        if (pressed & BTN_SELECT) {
            if (g_homeSelection == HOME_ITEM_SCRIPTS) {
                refreshScriptList();
                g_screen = SCREEN_SCRIPT_EXPLORER;
            } else if (g_homeSelection == HOME_ITEM_REFRESH) {
                doHttpHandshake();
                doMqttHandshake();
                refreshPublicProfile();
            } else if (g_homeSelection == HOME_ITEM_SETTINGS) {
                g_settingsSel = 0;
                g_screen = SCREEN_SETTINGS;
            }
        }
    }
    if (!g_luaDisplayActive) g_needsRedraw = true;
}

void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(500);
    printHelp();
    if (sodium_init() < 0) {
        Serial.println("libsodium init failed");
    }
    loadConfig();
    initPeripherals();
    g_screen = SCREEN_BOOT_SPLASH;
    g_needsRedraw = true;
    redraw();
    delay(BOOT_SPLASH_MS);
    sampleBattery(true);
    g_screen = SCREEN_STATUS;
    g_needsRedraw = true;
    redraw();
    String keyError;
    if (loadOrCreateSolanaKey(false, keyError)) {
        setLog("Onion OS ready");
    } else {
        setLog("Wallet locked: " + clipped(keyError, 18));
    }
    ensureWifi();
    doHttpHandshake();
    refreshPublicProfile();  // pull the current balance once at boot
}

void loop() {
    handleSerial();
    handleButtons();
    sampleBattery();

    // Handle WiFi worker state transitions
    int wr = g_wifiWorkerResult.load();
    if (g_screen == SCREEN_WIFI_SCANNING) {
        if (wr == WIFI_WORKER_DONE) {
            g_wifiWorkerResult.store(WIFI_WORKER_IDLE);
            g_wifiListSel = 0;
            g_screen = SCREEN_WIFI_LIST;
            g_needsRedraw = true;
        } else if (wr == WIFI_WORKER_FAILED) {
            g_wifiWorkerResult.store(WIFI_WORKER_IDLE);
            g_wifiResultMsg = "Scan failed";
            g_screen = SCREEN_WIFI_RESULT;
            g_needsRedraw = true;
        }
    } else if (g_screen == SCREEN_WIFI_CONNECTING) {
        if (wr == WIFI_WORKER_DONE) {
            g_wifiWorkerResult.store(WIFI_WORKER_IDLE);
            g_prefs.putString("wifi_ssid", g_wifiConnectSsid.c_str());
            g_prefs.putString("wifi_pass", g_wifiPassBuf);
            g_config.wifiSsid = g_wifiConnectSsid;
            g_config.wifiPassword = String(g_wifiPassBuf);
            setLog("WiFi connected");
            g_wifiResultMsg = "Connected!";
            g_screen = SCREEN_WIFI_RESULT;
            g_needsRedraw = true;
        } else if (wr == WIFI_WORKER_FAILED) {
            g_wifiWorkerResult.store(WIFI_WORKER_IDLE);
            g_wifiResultMsg = "Connect failed";
            g_screen = SCREEN_WIFI_RESULT;
            g_needsRedraw = true;
        }
    }

    if (WiFi.status() != WL_CONNECTED && millis() - g_lastWifiAttempt > 30000) {
        g_lastWifiAttempt = millis();
        triggerWifiReconnect();
    }
    ensureMqtt();
    processCheckInService();

    if (millis() - g_lastHandshake > HANDSHAKE_INTERVAL_MS) {
        g_lastHandshake = millis();
        if (g_mqttConnected) doMqttHandshake();
    }

    // Periodic Onion-balance refresh. Handshakes only carry onionId/status, so
    // without this the displayed balance only updates on a manual "Refresh
    // Profile" or a fresh link — it goes stale as soon as the server total
    // changes. Quiet so an idle badge stays silent; skipped while offline or a
    // WiFi worker is busy so it never blocks the UI.
    if (WiFi.status() == WL_CONNECTED &&
        g_wifiWorkerResult.load() != WIFI_WORKER_RUNNING &&
        millis() - g_lastProfileRefresh > PROFILE_REFRESH_INTERVAL_MS) {
        g_lastProfileRefresh = millis();
        refreshPublicProfile(true);
    }

    redraw();
    delay(20);
}
