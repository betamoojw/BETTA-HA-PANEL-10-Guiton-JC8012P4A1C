/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * ES8311 audio codec driver for the Guition JC8012P4A1C-I-W-Y 10.1" panel.
 *
 * The board carries an ES8311 mono codec on the same I2C bus as the GSL3680
 * touch controller (I2C_NUM_1: SCL=GPIO8, SDA=GPIO7, device address 0x18/0x30)
 * and an I2S link (SCLK=12, MCLK=13, LCLK=10, DOUT=9, DSIN=11) with a power
 * amplifier enable on GPIO20.  Wiring vendored from the official Guition
 * esp32_p4_function_ev_board reference.
 *
 * The codec is shared by the Xiaozhi voice assistant (16 kHz mono duplex)
 * and any future audio features.  audio_init() is best-effort and must never
 * block boot when the codec is not fitted.
 */
#include "drivers/board_extras_panel10jc.h"

#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "esp_err.h"
#include "esp_log.h"

#include "util/log_tags.h"

#define JC_AUDIO_I2C_PORT I2C_NUM_1
#define JC_AUDIO_CODEC_ADDR ES8311_CODEC_DEFAULT_ADDR

#define JC_AUDIO_I2S_PORT I2S_NUM_1
#define JC_AUDIO_I2S_MCLK GPIO_NUM_13
#define JC_AUDIO_I2S_SCLK GPIO_NUM_12
#define JC_AUDIO_I2S_LCLK GPIO_NUM_10
#define JC_AUDIO_I2S_DOUT GPIO_NUM_9
#define JC_AUDIO_I2S_DSIN GPIO_NUM_11
#define JC_AUDIO_PA_GPIO GPIO_NUM_20

/* 16 kHz matches the Xiaozhi/Opus pipeline exactly; the ES8311 is the only
 * audio consumer on this panel, so there is no benefit to 22.05 kHz. */
#define JC_AUDIO_SAMPLE_RATE_HZ 16000

static i2s_chan_handle_t s_i2s_tx = NULL;
static i2s_chan_handle_t s_i2s_rx = NULL;
static const audio_codec_data_if_t *s_i2s_data_if = NULL;
static esp_codec_dev_handle_t s_speaker = NULL;
static esp_codec_dev_handle_t s_mic = NULL;

static esp_err_t jc_audio_i2s_init(void)
{
    if (s_i2s_data_if != NULL) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(JC_AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; /* drop stale DMA data after re-init */
    /* Bigger DMA ring: 8 x 1024 frames (2 KB each) = 16 KB, ~512 ms of audio
     * @ 16 kHz mono/16-bit. The stock default (6 x 240, auto-adjusted to 256)
     * is only ~96 ms, so a brief playback-task stall underruns the DMA and
     * surfaces as crackle on TTS. Safe now that the Xiaozhi capture-task
     * stack, playback-task stack and playback stream buffer all live in PSRAM
     * (see xiaozhi_client.c), which frees the required internal RAM. */
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 1024;
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_i2s_tx, &s_i2s_rx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_AUDIO, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(JC_AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = JC_AUDIO_I2S_MCLK,
            .bclk = JC_AUDIO_I2S_SCLK,
            .ws = JC_AUDIO_I2S_LCLK,
            .dout = JC_AUDIO_I2S_DOUT,
            .din = JC_AUDIO_I2S_DSIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    if (s_i2s_tx != NULL) {
        err = i2s_channel_init_std_mode(s_i2s_tx, &std_cfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG_AUDIO, "i2s tx init failed: %s", esp_err_to_name(err));
            return err;
        }
        err = i2s_channel_enable(s_i2s_tx);
        if (err != ESP_OK) {
            ESP_LOGW(TAG_AUDIO, "i2s tx enable failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    if (s_i2s_rx != NULL) {
        err = i2s_channel_init_std_mode(s_i2s_rx, &std_cfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG_AUDIO, "i2s rx init failed: %s", esp_err_to_name(err));
            return err;
        }
        err = i2s_channel_enable(s_i2s_rx);
        if (err != ESP_OK) {
            ESP_LOGW(TAG_AUDIO, "i2s rx enable failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = JC_AUDIO_I2S_PORT,
        .tx_handle = s_i2s_tx,
        .rx_handle = s_i2s_rx,
    };
    s_i2s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (s_i2s_data_if == NULL) {
        ESP_LOGW(TAG_AUDIO, "audio_codec_new_i2s_data failed");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static const audio_codec_ctrl_if_t *jc_audio_i2c_ctrl(i2c_master_bus_handle_t bus)
{
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = JC_AUDIO_I2C_PORT,
        .addr = JC_AUDIO_CODEC_ADDR,
        .bus_handle = bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (ctrl_if == NULL) {
        ESP_LOGW(TAG_AUDIO, "audio_codec_new_i2c_ctrl failed");
    }
    return ctrl_if;
}

static esp_codec_dev_handle_t jc_audio_speaker_init(i2c_master_bus_handle_t bus)
{
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (gpio_if == NULL) {
        ESP_LOGW(TAG_AUDIO, "audio_codec_new_gpio failed");
        return NULL;
    }

    const audio_codec_ctrl_if_t *ctrl_if = jc_audio_i2c_ctrl(bus);
    if (ctrl_if == NULL) {
        return NULL;
    }

    const esp_codec_dev_hw_gain_t gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = JC_AUDIO_PA_GPIO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = gain,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);
    if (codec_if == NULL) {
        ESP_LOGW(TAG_AUDIO, "es8311_codec_new (speaker) failed");
        return NULL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = s_i2s_data_if,
    };
    return esp_codec_dev_new(&dev_cfg);
}

static esp_codec_dev_handle_t jc_audio_mic_init(i2c_master_bus_handle_t bus)
{
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (gpio_if == NULL) {
        ESP_LOGW(TAG_AUDIO, "audio_codec_new_gpio failed");
        return NULL;
    }

    const audio_codec_ctrl_if_t *ctrl_if = jc_audio_i2c_ctrl(bus);
    if (ctrl_if == NULL) {
        return NULL;
    }

    const esp_codec_dev_hw_gain_t gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };

    /* The mic device must own ONLY the ADC path (ADC mode, not BOTH).  The
     * ES8311 is one physical chip shared by the speaker (DAC) and mic (ADC)
     * codec devices.  With codec_mode=BOTH the mic's es8311_enable(false) on
     * close would mute the DAC and drop the PA even while the speaker is
     * still open, so every Xiaozhi VAD cycle ended with the output muted —
     * the "silent speaker" bug.  ADC mode leaves DAC/PA control entirely to
     * the speaker device. */
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC,
        .pa_pin = JC_AUDIO_PA_GPIO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = gain,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);
    if (codec_if == NULL) {
        ESP_LOGW(TAG_AUDIO, "es8311_codec_new (mic) failed");
        return NULL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec_if,
        .data_if = s_i2s_data_if,
    };
    return esp_codec_dev_new(&dev_cfg);
}

esp_err_t audio_init(void)
{
    if (s_speaker != NULL || s_mic != NULL) {
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = jc8012_i2c_bus_get();
    if (bus == NULL) {
        ESP_LOGW(TAG_AUDIO, "Shared I2C bus not ready (touch not initialised?)");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = jc_audio_i2s_init();
    if (err != ESP_OK) {
        return err;
    }

    s_speaker = jc_audio_speaker_init(bus);
    s_mic = jc_audio_mic_init(bus);

    if (s_speaker == NULL && s_mic == NULL) {
        ESP_LOGW(TAG_AUDIO, "ES8311 codec not detected on I2C bus");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG_AUDIO, "ES8311 audio codec ready (speaker=%d, mic=%d)",
             s_speaker != NULL ? 1 : 0, s_mic != NULL ? 1 : 0);
    return ESP_OK;
}

bool audio_is_ready(void)
{
    return s_speaker != NULL || s_mic != NULL;
}

esp_codec_dev_handle_t audio_get_speaker(void)
{
    return s_speaker;
}

esp_codec_dev_handle_t audio_get_mic(void)
{
    return s_mic;
}

void audio_deinit(void)
{
    if (s_speaker != NULL) {
        esp_codec_dev_delete(s_speaker);
        s_speaker = NULL;
    }
    if (s_mic != NULL) {
        esp_codec_dev_delete(s_mic);
        s_mic = NULL;
    }
    s_i2s_data_if = NULL;
    if (s_i2s_tx != NULL) {
        i2s_channel_disable(s_i2s_tx);
        i2s_del_channel(s_i2s_tx);
        s_i2s_tx = NULL;
    }
    if (s_i2s_rx != NULL) {
        i2s_channel_disable(s_i2s_rx);
        i2s_del_channel(s_i2s_rx);
        s_i2s_rx = NULL;
    }
}
