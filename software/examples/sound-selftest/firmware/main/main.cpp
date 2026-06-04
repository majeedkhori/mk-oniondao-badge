// OnionDAO Badge — Sound Module self-test (variant sweep)
// NS4168 Class-D amp (speaker) + SPH0641 PDM mic.
//
// The module can be wired to one of three side-port pin groups (L1 / L2 / R)
// and we don't yet know which. This firmware SWEEPS all three:
//   for each variant -> play a 1 kHz tone (listen for a beep)
//                    -> sample the PDM mic and print RMS/peak
// The correct variant is the one where you HEAR the tone AND the mic shows a
// low, reactive noise floor (tap it to confirm). Wrong variants stay silent
// and the mic rails near full-scale (~30000) because the data pin floats.
//
// Serial: 115200. Loops forever so you can re-listen / tap the mic.
//
// NOTE: shares GPIOs with the CC1101 radio slot — nothing else may use them.

#include <Arduino.h>
#include <math.h>
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"

#define PIN_PWR      18      // peripheral power gate — HIGH = on
#define SAMPLE_RATE  16000   // -> PDM clock ~1.02 MHz, in SPH0641 range
#define TONE_HZ      1000

struct Variant {
    const char* name;
    int mic;    // SPH0641 PDM DATA
    int bclk;   // bit / PDM clock
    int ws;     // word-select / mic SELECT
    int sdo;    // I2S audio data -> NS4168 SD
    int ctrl;   // NS4168 mode / shutdown
};

static const Variant VARIANTS[] = {
    { "L1", 48, 47, 19, 42, 41 },
    { "L2", 40, 41, 42, 19, 47 },
    { "R",  38, 39, 16, 15,  7 },
};

// ── Speaker: 1 kHz tone for ~2 s on the given pins ──────────────────────────
static void speaker_test(const Variant& v) {
    pinMode(v.ctrl, OUTPUT);
    digitalWrite(v.ctrl, HIGH);     // un-shutdown the amp

    i2s_chan_handle_t tx = nullptr;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    if (i2s_new_channel(&chan_cfg, &tx, nullptr) != ESP_OK) return;

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)v.bclk,
            .ws   = (gpio_num_t)v.ws,
            .dout = (gpio_num_t)v.sdo,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    if (i2s_channel_init_std_mode(tx, &std_cfg) != ESP_OK) { i2s_del_channel(tx); return; }
    i2s_channel_enable(tx);

    const int period = SAMPLE_RATE / TONE_HZ;   // 16 samples / cycle
    int16_t buf[256];
    for (int i = 0; i < 256; i++) {
        buf[i] = (int16_t)(0.30f * 32767.0f * sinf(2.0f * 3.14159265f * (i % period) / period));
    }
    size_t written;
    uint32_t t0 = millis();
    while (millis() - t0 < 2000) {
        i2s_channel_write(tx, buf, sizeof(buf), &written, 100);
    }

    i2s_channel_disable(tx);
    i2s_del_channel(tx);
    digitalWrite(v.ctrl, LOW);       // mute amp during mic sample
}

// ── Mic: sample PDM for ~2 s on the given pins, report level ────────────────
static void mic_test(const Variant& v) {
    // SPH0641 SELECT (= WS) tied LOW -> mic drives LEFT slot (what we sample).
    gpio_reset_pin((gpio_num_t)v.ws);
    pinMode(v.ws, OUTPUT);
    digitalWrite(v.ws, LOW);

    i2s_chan_handle_t rx = nullptr;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    if (i2s_new_channel(&chan_cfg, nullptr, &rx) != ESP_OK) return;

    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg  = I2S_PDM_RX_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                   I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = (gpio_num_t)v.bclk,
            .din = (gpio_num_t)v.mic,
            .invert_flags = { .clk_inv = false },
        },
    };
    if (i2s_channel_init_pdm_rx_mode(rx, &pdm_cfg) != ESP_OK) { i2s_del_channel(rx); return; }
    i2s_channel_enable(rx);

    static int16_t samples[1024];
    size_t bytes_read;
    uint32_t t0 = millis();
    while (millis() - t0 < 2000) {
        if (i2s_channel_read(rx, samples, sizeof(samples), &bytes_read, 200) == ESP_OK) {
            int n = bytes_read / sizeof(int16_t);
            double sumsq = 0; int peak = 0;
            for (int i = 0; i < n; i++) {
                int s = samples[i];
                sumsq += (double)s * s;
                if (abs(s) > peak) peak = abs(s);
            }
            double rms = n ? sqrt(sumsq / n) : 0;
            int bars = (int)(rms / 200); if (bars > 40) bars = 40;
            char bar[41]; for (int i = 0; i < 40; i++) bar[i] = (i < bars) ? '#' : ' '; bar[40] = '\0';
            Serial.printf("      mic RMS %6.0f  peak %5d  |%s|\n", rms, peak, bar);
        }
        delay(50);
    }
    i2s_channel_disable(rx);
    i2s_del_channel(rx);
    gpio_reset_pin((gpio_num_t)v.ws);
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== OnionDAO Sound Module self-test — variant sweep ===");
    Serial.println("For each variant: listen for a 2 s beep, then watch the mic level.");
    Serial.println("Correct variant = you HEAR the tone AND mic shows a LOW, reactive");
    Serial.println("floor (tap it). Wrong variants are silent and the mic rails ~30000.\n");

    pinMode(PIN_PWR, OUTPUT);
    digitalWrite(PIN_PWR, HIGH);     // power the peripheral rail
    delay(50);
}

void loop() {
    for (const Variant& v : VARIANTS) {
        Serial.printf("\n>>> Variant %-2s  (mic=G%d bclk=G%d ws=G%d sdo=G%d ctrl=G%d)\n",
                      v.name, v.mic, v.bclk, v.ws, v.sdo, v.ctrl);
        Serial.printf("    [tone] listen NOW for ~2 s...\n");
        speaker_test(v);
        Serial.printf("    [mic ] sampling ~2 s (tap the mic):\n");
        mic_test(v);
        delay(800);
    }
    Serial.println("\n--- sweep complete, repeating ---");
    delay(500);
}
