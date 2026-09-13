// ============================================================
// Xiaozhi Audio Backend
// ============================================================
//
// Ported from ForgeUI 30_Audio.c (fg_audio_* -> xz_audio_*) for
// the BETTA-HA-PANEL firmware.
//
// Current Hardware Path:
//
// Waveshare ESP32-P4-WIFI6-Touch-LCD-7B
// BSP audio init
// ESP codec device speaker output (ES8311)
//
// ============================================================

#include "xiaozhi_audio.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"

#include "esp_codec_dev.h"

#if CONFIG_APP_PANEL_VARIANT_10INCH_JC
#include "drivers/board_extras_panel10jc.h"
#else
#include "bsp/esp32_p4_wifi6_touch_lcd_7b.h"
#include "driver/i2s_std.h"
#endif

#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

static const char *TAG = "XZ_AUDIO";

/* Default speaker volume. The ES8311 volume register is 0.5 dB/step and the
 * BSP adds ~+3.6 dB of hw-gain compensation, so volume 85 lands around
 * register 0xB7 (~ -3.9 dB), just below the clean 0 dB point (0xBF). That is
 * a clearly louder but still distortion-free default vs. the old 70 (~0xA8).
 * The value is persisted so a reboot no longer drops the volume back down. */
#define XZ_VOLUME_DEFAULT  75
/* Max clean volume. Diagnosis on the panel: a pure 1000 Hz sine at -10 dBFS
 * is clean at 70% but crackles slightly at 90% with the DAC register still
 * negative (~0xBC, -1.4 dB, so no digital clipping). The distortion is in the
 * analog stage (NS4150B amp / panel speaker) once the DAC drives it past a
 * certain level -- a hardware limit, not a decode/config fault. 80% is a
 * conservative clean ceiling (DAC ~0xB3, -6.4 dB after hw-gain comp); beyond
 * it the amp begins to distort. Clamp so the slider can never enter the
 * crackle zone. */
#define XZ_VOLUME_MAX      80
#define XZ_AUDIO_NVS_NS    "xz_audio"
#define XZ_AUDIO_NVS_VOL   "volume"

static esp_codec_dev_handle_t g_speaker = NULL;
static esp_codec_dev_handle_t g_mic = NULL;
static bool g_ready = false;
static bool g_mic_open = false;
static bool g_busy  = false;
static int  g_volume = XZ_VOLUME_DEFAULT;

#define XZ_AUDIO_SAMPLE_RATE 16000
#define XZ_AUDIO_CHANNELS    1
#define XZ_AUDIO_BITS        16

/* Diagnostic beeps: fixed tones plus frequency sweeps so bass / mid /
 * sibilant handling can be judged in one button press. 600 ms per signal,
 * 200 ms silence in between. A chirp sweeping a band has the broadband,
 * transient character of real speech, so it reveals speaker/PA breakup
 * that a steady sine cannot (and it is generated locally, with no Opus or
 * network in the path). */
typedef enum { BEEP_TONE, BEEP_CHIRP } beep_kind_t;
static const struct { beep_kind_t kind; int a; int b; int dur_ms; } s_beep_tones[] = {
    { BEEP_TONE,   150,    0, 600 },
    { BEEP_TONE,   300,    0, 600 },
    { BEEP_TONE,  1000,    0, 600 },
    { BEEP_TONE,  5000,    0, 600 },
    { BEEP_CHIRP,   80,  800, 600 },  /* bass sweep                      */
    { BEEP_CHIRP, 2000, 8000, 600 },  /* sibilant band sweep             */
};
#define BEEP_AMPLITUDE 10000   /* -10.3 dBFS (proven clean at 100% volume)  */

static int16_t g_beep_buffer[XZ_AUDIO_SAMPLE_RATE]; /* 1 s chunk (32 KB) */

#if !CONFIG_APP_PANEL_VARIANT_10INCH_JC
/*
 * The BSP's internal default is 22050 Hz mono/16-bit. Xiaozhi (and Opus)
 * work best at 16 kHz, so we pass an explicit I2S config. GPIOs come from the
 * BSP header so this stays in sync with the board definition.
 */
static i2s_std_config_t xz_audio_build_i2s_config(void)
{
    i2s_std_config_t cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(XZ_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws   = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din  = BSP_I2S_DSIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    return cfg;
}
#endif /* !CONFIG_APP_PANEL_VARIANT_10INCH_JC */

esp_codec_dev_sample_info_t xz_audio_sample_info(void)
{
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = XZ_AUDIO_BITS,
        .channel = XZ_AUDIO_CHANNELS,
        .channel_mask = 0,
        .sample_rate = XZ_AUDIO_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    return fs;
}

/* Mic capture channel count is board dependent: mono on the JC panel
 * (single ES8311 mic), stereo on the 7B (two ES7210 mics). */
esp_codec_dev_sample_info_t xz_audio_mic_sample_info(void)
{
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = XZ_AUDIO_BITS,
        .channel = XZ_MIC_CHANNELS,
        .channel_mask = 0,
        .sample_rate = XZ_AUDIO_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    return fs;
}

static void xz_audio_save_volume(void)
{
    nvs_handle_t h;
    if (nvs_open(XZ_AUDIO_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_i32(h, XZ_AUDIO_NVS_VOL, (int32_t)g_volume);
    nvs_commit(h);
    nvs_close(h);
}

static void xz_audio_load_volume(void)
{
    nvs_handle_t h;
    if (nvs_open(XZ_AUDIO_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    int32_t v = -1;
    if (nvs_get_i32(h, XZ_AUDIO_NVS_VOL, &v) == ESP_OK && v >= 0 && v <= 100) {
        g_volume = (int)v;
    }
    if (g_volume > XZ_VOLUME_MAX) {
        g_volume = XZ_VOLUME_MAX;
    }
    nvs_close(h);
}

esp_err_t xz_audio_init(void)
{
    if (g_ready) return ESP_OK;

    ESP_LOGI(TAG, "Init audio...");

    xz_audio_load_volume();

#if CONFIG_APP_PANEL_VARIANT_10INCH_JC
    /* JC panel: reuse the board audio driver (ES8311 on the touch I2C bus,
     * I2S at 16 kHz).  app_main already called audio_init() best-effort; it
     * is idempotent, so calling it again is safe and covers the case where
     * the first attempt ran before the I2C bus was up. */
    esp_err_t err = audio_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio_init failed: %s", esp_err_to_name(err));
        return err;
    }

    g_speaker = audio_get_speaker();
    g_mic = audio_get_mic();
    if (!g_speaker || !g_mic) {
        ESP_LOGE(TAG, "JC codec handles unavailable (speaker=%p mic=%p)",
                 (void *)g_speaker, (void *)g_mic);
        return ESP_FAIL;
    }
#else
    i2s_std_config_t i2s_cfg = xz_audio_build_i2s_config();
    esp_err_t err = bsp_audio_init(&i2s_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init failed: %s", esp_err_to_name(err));
        return err;
    }

    g_speaker = bsp_audio_codec_speaker_init();
    if (!g_speaker) {
        ESP_LOGE(TAG, "Speaker init failed");
        return ESP_FAIL;
    }

    g_mic = bsp_audio_codec_microphone_init();
    if (!g_mic) {
        ESP_LOGE(TAG, "Mic init failed");
        return ESP_FAIL;
    }
#endif

    esp_codec_dev_sample_info_t fs = xz_audio_sample_info();

    err = esp_codec_dev_open(g_speaker, &fs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open(speaker) failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_codec_dev_set_out_vol(g_speaker, g_volume);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Initial volume set failed: %s", esp_err_to_name(err));
    }

    g_ready = true;
    ESP_LOGI(TAG, "Audio ready (duplex %d Hz / mono / 16-bit)", XZ_AUDIO_SAMPLE_RATE);

    return ESP_OK;
}

esp_err_t xz_audio_set_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > XZ_VOLUME_MAX) volume = XZ_VOLUME_MAX;

    g_volume = volume;

    esp_err_t err = xz_audio_init();
    if (err != ESP_OK) return err;

    err = esp_codec_dev_set_out_vol(g_speaker, g_volume);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Volume set failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Volume set: %d%%", g_volume);
        xz_audio_save_volume();
    }

    return err;
}

int xz_audio_get_volume(void)
{
    return g_volume;
}

int xz_audio_get_max_volume(void)
{
    return XZ_VOLUME_MAX;
}

esp_err_t xz_audio_test_beep(void)
{
    ESP_LOGI(TAG, "test_beep() called (busy=%d, ready=%d, speaker=%p)",
             g_busy, g_ready, (void *)g_speaker);

    if (g_busy) {
        ESP_LOGW(TAG, "Beep already running");
        return ESP_OK;
    }

    g_busy = true;

    esp_err_t err = xz_audio_init();
    if (err != ESP_OK) {
        g_busy = false;
        return err;
    }

    esp_codec_dev_set_out_vol(g_speaker, g_volume);

    for (size_t t = 0; t < sizeof(s_beep_tones) / sizeof(s_beep_tones[0]); t++) {
        int samples = XZ_AUDIO_SAMPLE_RATE * s_beep_tones[t].dur_ms / 1000;
        if (samples > (int)(sizeof(g_beep_buffer) / sizeof(g_beep_buffer[0]))) {
            samples = (int)(sizeof(g_beep_buffer) / sizeof(g_beep_buffer[0]));
        }
        if (s_beep_tones[t].kind == BEEP_CHIRP) {
            /* Linear chirp a -> b Hz. Phase is accumulated so the
             * instantaneous frequency stays exact across the sweep. */
            double phase = 0.0;
            double f0 = (double)s_beep_tones[t].a;
            double f1 = (double)s_beep_tones[t].b;
            double inv = (samples > 1) ? 1.0 / (double)(samples - 1) : 0.0;
            for (int i = 0; i < samples; i++) {
                double f = f0 + (f1 - f0) * (double)i * inv;
                phase += 2.0 * M_PI * f / XZ_AUDIO_SAMPLE_RATE;
                g_beep_buffer[i] = (int16_t)(sin(phase) * BEEP_AMPLITUDE);
            }
            ESP_LOGI(TAG, "chirp %d -> %d Hz for %d ms", s_beep_tones[t].a,
                     s_beep_tones[t].b, s_beep_tones[t].dur_ms);
        } else {
            for (int i = 0; i < samples; i++) {
                float sec = (float)i / XZ_AUDIO_SAMPLE_RATE;
                g_beep_buffer[i] = (int16_t)(sinf(2.0f * (float)M_PI *
                                           s_beep_tones[t].a * sec) * BEEP_AMPLITUDE);
            }
            ESP_LOGI(TAG, "beep %d Hz for %d ms", s_beep_tones[t].a,
                     s_beep_tones[t].dur_ms);
        }
        err = esp_codec_dev_write(g_speaker, g_beep_buffer,
                                  samples * sizeof(int16_t));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Write failed: %s", esp_err_to_name(err));
            break;
        }
        /* 200 ms of silence so consecutive tones are clearly separable. */
        int gap = XZ_AUDIO_SAMPLE_RATE * 200 / 1000;
        if (gap > (int)(sizeof(g_beep_buffer) / sizeof(g_beep_buffer[0]))) {
            gap = (int)(sizeof(g_beep_buffer) / sizeof(g_beep_buffer[0]));
        }
        memset(g_beep_buffer, 0, gap * sizeof(int16_t));
        err = esp_codec_dev_write(g_speaker, g_beep_buffer, gap * sizeof(int16_t));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Write failed: %s", esp_err_to_name(err));
            break;
        }
    }

    /* Decisive source-vs-hardware test. TTS crackles on loud phonemes while
     * the pure tones/sweeps above are clean. Those tones bypass xz_audio_play
     * (no de-esser). To split "Opus/server source" from "de-esser or
     * analogue path", route a synthetic voiced signal through xz_audio_play()
     * — the FULL TTS path including the 4 kHz de-esser — with no Opus and no
     * network. A 150 Hz sawtooth has dense harmonics across the speech band
     * (like a vowel); white noise mimics broadband transients. If these are
     * clean while TTS crackles, the artifact lives in the Opus/server source. */
    if (err == ESP_OK) {
        int n = XZ_AUDIO_SAMPLE_RATE * 800 / 1000;
        double phase = 0.0;
        for (int i = 0; i < n; i++) {
            phase += 150.0 / XZ_AUDIO_SAMPLE_RATE;
            double frac = phase - floor(phase);
            g_beep_buffer[i] = (int16_t)((2.0 * frac - 1.0) * BEEP_AMPLITUDE);
        }
        ESP_LOGI(TAG, "speech test: 150 Hz sawtooth through de-esser path");
        err = xz_audio_play(g_beep_buffer, n);
    }

    if (err == ESP_OK) {
        int gap = XZ_AUDIO_SAMPLE_RATE * 200 / 1000;
        memset(g_beep_buffer, 0, gap * sizeof(int16_t));
        (void)xz_audio_play(g_beep_buffer, gap);
    }

    if (err == ESP_OK) {
        int n = XZ_AUDIO_SAMPLE_RATE * 800 / 1000;
        uint32_t lfsr = 0xACE1u;
        for (int i = 0; i < n; i++) {
            lfsr = (lfsr >> 1) ^ (-(lfsr & 1u) & 0xB400u);
            float w = (float)(lfsr & 0xFFFFu) / 32768.0f - 1.0f;
            g_beep_buffer[i] = (int16_t)(w * BEEP_AMPLITUDE);
        }
        ESP_LOGI(TAG, "speech test: white noise through de-esser path");
        err = xz_audio_play(g_beep_buffer, n);
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "BEEP sequence sent at %d%% volume", g_volume);
    }

    g_busy = false;
    return err;
}

esp_err_t xz_audio_open_mic(void)
{
    esp_err_t err = xz_audio_init();
    if (err != ESP_OK) {
        return err;
    }
    if (g_mic_open) {
        return ESP_OK;
    }

    esp_codec_dev_sample_info_t fs = xz_audio_mic_sample_info();
    if (esp_codec_dev_open(g_mic, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Failed to open mic device");
        return ESP_FAIL;
    }
    g_mic_open = true;
    ESP_LOGI(TAG, "Mic opened: %d ch, %d Hz", XZ_MIC_CHANNELS,
             XZ_AUDIO_SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t xz_audio_close_mic(void)
{
    if (g_mic == NULL || !g_mic_open) {
        return ESP_OK;
    }
    esp_codec_dev_close(g_mic);
    g_mic_open = false;
    return ESP_OK;
}

esp_err_t xz_audio_set_mic_gain(float db)
{
    if (g_mic == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    int r = esp_codec_dev_set_in_gain(g_mic, db);
    if (r != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "set_in_gain(%.1f) failed: %d", db, r);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* TTS de-esser low-pass                                               */
/* ------------------------------------------------------------------ */
/* The Xiaozhi voice stream arrives as Opus and its sibilants ("s", "sz",
 * "c") carry a harsh high-frequency crackle that survives level cuts:
 * a pure -10.3 dBFS sine plays clean while TTS still crackles at -20 dBFS,
 * so the artifact lives in the signal itself, not in analogue clipping.
 * A 2nd-order Butterworth low-pass rolls off the band above ~4 kHz where
 * that sibilant harshness sits. 4 kHz keeps full telephone-band speech
 * (300 Hz - 3.4 kHz) intact, so intelligibility is unaffected while the
 * 5-8 kHz "s"/"c" hiss is heavily attenuated. The filter state persists
 * across the 60 ms playback chunks, so chunk boundaries stay click-free. */
#define XZ_TTS_LPF_HZ  4000.0f
#define XZ_TTS_LPF_Q   0.70710678f   /* Butterworth */

static float g_lpf_b0, g_lpf_b1, g_lpf_b2, g_lpf_a1, g_lpf_a2;
static float g_lpf_x1, g_lpf_x2, g_lpf_y1, g_lpf_y2;
static bool  g_lpf_ready = false;

static void xz_tts_lpf_init(void)
{
    float w = 2.0f * (float)M_PI * XZ_TTS_LPF_HZ / (float)XZ_AUDIO_SAMPLE_RATE;
    float c = cosf(w);
    float s = sinf(w);
    float alpha = s / (2.0f * XZ_TTS_LPF_Q);

    float b0 = (1.0f - c) / 2.0f;
    float b1 = 1.0f - c;
    float b2 = (1.0f - c) / 2.0f;
    float a0 = 1.0f + alpha;
    float a1 = -2.0f * c;
    float a2 = 1.0f - alpha;

    g_lpf_b0 = b0 / a0;
    g_lpf_b1 = b1 / a0;
    g_lpf_b2 = b2 / a0;
    g_lpf_a1 = a1 / a0;
    g_lpf_a2 = a2 / a0;

    g_lpf_x1 = g_lpf_x2 = g_lpf_y1 = g_lpf_y2 = 0.0f;
    g_lpf_ready = true;
    ESP_LOGI(TAG, "TTS de-esser enabled: %.0f Hz (Q=%.2f)",
             XZ_TTS_LPF_HZ, XZ_TTS_LPF_Q);
}

static void xz_tts_lpf_process(int16_t *pcm, int n)
{
    if (!g_lpf_ready) {
        xz_tts_lpf_init();
    }
    for (int i = 0; i < n; i++) {
        float x = (float)pcm[i];
        float y = g_lpf_b0 * x + g_lpf_b1 * g_lpf_x1 + g_lpf_b2 * g_lpf_x2
                - g_lpf_a1 * g_lpf_y1 - g_lpf_a2 * g_lpf_y2;
        g_lpf_x2 = g_lpf_x1;
        g_lpf_x1 = x;
        g_lpf_y2 = g_lpf_y1;
        g_lpf_y1 = y;
        if (y > 32767.0f) y = 32767.0f;
        if (y < -32768.0f) y = -32768.0f;
        pcm[i] = (int16_t)y;
    }
}

esp_err_t xz_audio_play(int16_t *pcm, int sample_count)
{
    if (pcm == NULL || sample_count <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = xz_audio_init();
    if (err != ESP_OK) {
        return err;
    }
    xz_tts_lpf_process(pcm, sample_count);
    int bytes = sample_count * sizeof(int16_t) * XZ_AUDIO_CHANNELS;
    int w = esp_codec_dev_write(g_speaker, (void *)pcm, bytes);
    if (w < 0) {
        ESP_LOGW(TAG, "Speaker write failed: %d", w);
        return ESP_FAIL;
    }
    return ESP_OK;
}

int xz_audio_read(int16_t *pcm, int max_frames)
{
    if (g_mic == NULL || !g_mic_open || pcm == NULL || max_frames <= 0) {
        return -1;
    }
    /* esp_codec_dev_read returns an error code (0 = OK), not a byte count;
     * on success the requested buffer has been filled (interleaved stereo,
     * one frame = one sample per mic). */
    int bytes = max_frames * sizeof(int16_t) * XZ_MIC_CHANNELS;
    int r = esp_codec_dev_read(g_mic, pcm, bytes);
    if (r < 0) {
        return -1;
    }
    return max_frames;
}

esp_codec_dev_handle_t xz_audio_get_speaker(void)
{
    return g_speaker;
}

esp_codec_dev_handle_t xz_audio_get_mic(void)
{
    return g_mic;
}
