#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClient.h>

#include <GxEPD2_BW.h>
#include <gdey/GxEPD2_270_GDEY027T91.h>
#include <Fonts/FreeMono9pt7b.h>
#include <Fonts/FreeMonoBold9pt7b.h>

// ── Config ─────────────────────────────────────────────────────────────────────
#ifndef WIFI_SSID
#define WIFI_SSID "iPhone"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS "web3weekends"
#endif
#ifndef PING_URL
#define PING_URL  "http://172.20.10.6:8080/"
#endif

#define PING_INTERVAL_MS 5000

// ── Pins ───────────────────────────────────────────────────────────────────────
#define PIN_PWR      18
#define PIN_SCL       9
#define PIN_SDA      10
#define PIN_EPD_MOSI 17
#define PIN_EPD_SCK  11
#define PIN_EPD_CS   12
#define PIN_EPD_DC   13
#define PIN_EPD_RST  14
#define PIN_EPD_BUSY 21

// ── Display ────────────────────────────────────────────────────────────────────
GxEPD2_BW<GxEPD2_270_GDEY027T91, GxEPD2_270_GDEY027T91::HEIGHT> display(
    GxEPD2_270_GDEY027T91(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY)
);

static void show(const char* title, const char* line1, const char* line2 = nullptr) {
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);

        display.setFont(&FreeMonoBold9pt7b);
        display.setTextColor(GxEPD_BLACK);
        display.setCursor(8, 22);
        display.print(title);
        display.drawFastHLine(0, 30, display.width(), GxEPD_BLACK);

        display.setFont(&FreeMono9pt7b);
        display.setCursor(8, 54);
        display.print(line1);

        if (line2) {
            display.setCursor(8, 74);
            display.print(line2);
        }
    } while (display.nextPage());
    display.hibernate();
}

void setup() {
    Serial.begin(115200);

    pinMode(PIN_PWR, OUTPUT);
    digitalWrite(PIN_PWR, HIGH);
    delay(50);

    Wire.begin(PIN_SDA, PIN_SCL);
    SPI.begin(PIN_EPD_SCK, /*MISO*/-1, PIN_EPD_MOSI, PIN_EPD_CS);
    display.init(115200, true, 10, false);
    display.setRotation(1);

    show("PING SERVER", "Connecting WiFi...", WIFI_SSID);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000)
        delay(200);

    if (WiFi.status() != WL_CONNECTED) {
        show("PING SERVER", "WiFi failed.", "Check credentials.");
        Serial.println("[ping] WiFi connect failed");
        return;
    }

    Serial.printf("[ping] WiFi up, IP %s\n", WiFi.localIP().toString().c_str());
    show("PING SERVER", "WiFi connected.", WiFi.localIP().toString().c_str());
}

static uint32_t s_last_ping    = 0;
static uint32_t s_ok_count     = 0;
static uint32_t s_fail_count   = 0;

void loop() {
    if (millis() - s_last_ping < PING_INTERVAL_MS)
        return;
    s_last_ping = millis();

    if (WiFi.status() != WL_CONNECTED) {
        show("PING SERVER", "WiFi lost.", "Reconnecting...");
        WiFi.reconnect();
        return;
    }

    WiFiClient client;
    uint32_t t0 = millis();
    int code = -1;

    if (client.connect("172.20.10.6", 8080)) {
        client.print("GET / HTTP/1.0\r\nHost: 172.20.10.6\r\nConnection: close\r\n\r\n");
        uint32_t deadline = millis() + 3000;
        while (!client.available() && millis() < deadline) delay(10);
        if (client.available()) {
            String status = client.readStringUntil('\n');
            if (status.startsWith("HTTP/"))
                code = status.substring(9, 12).toInt();
        }
        client.stop();
    }

    uint32_t rtt = millis() - t0;

    char line1[32], line2[32];
    if (code == 200) {
        s_ok_count++;
        snprintf(line1, sizeof(line1), "OK  %u ms", (unsigned)rtt);
        snprintf(line2, sizeof(line2), "ok:%u fail:%u", (unsigned)s_ok_count, (unsigned)s_fail_count);
        Serial.printf("[ping] 200  %u ms\n", (unsigned)rtt);
    } else {
        s_fail_count++;
        snprintf(line1, sizeof(line1), "FAIL  HTTP %d", code);
        snprintf(line2, sizeof(line2), "ok:%u fail:%u", (unsigned)s_ok_count, (unsigned)s_fail_count);
        Serial.printf("[ping] failed: %d\n", code);
    }

    show("PING SERVER", line1, line2);
}
