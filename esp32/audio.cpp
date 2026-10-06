/**
 * =============================================================================
 * EnvMon ESP32 - 音频系统驱动 (audio.cpp)
 * =============================================================================
 *
 * 硬件: I2S DAC 喇叭 (MAX98357A)
 * 引脚: DOUT=D38(GPIO38), BCLK=D18(GPIO18), LRC=D39(GPIO39)
 * 库  : I2S 驱动 (legacy API: i2s_driver_install / i2s_write)
 *
 * 版本: v8.0.0
 * =============================================================================
 */

#include "envmon_esp32.h"
#include <driver/i2s.h>

// 音频参数
#define AUDIO_SAMPLE_RATE    16000
#define AUDIO_PORT           I2S_NUM_1

#define ALARM_FREQ_HZ        1000
#define ALARM_DUTY_MS        1000
#define ALARM_GAP_MS         500
#define ALARM_VOLUME         0.5f
#define TEST_FREQ_HZ         440
#define TEST_DURATION_MS     1500
#define WAVE_PEAK            30000

namespace {
  bool     s_inited       = false;
  bool     s_muted        = false;
  bool     s_alarmActive  = false;
  uint32_t s_alarmPhaseMs = 0;
  bool     s_alarmToneOn  = false;
  uint32_t s_testStartMs  = 0;
  bool     s_testActive   = false;
  float    s_phase        = 0.0f;
  float    s_phaseInc     = 0.0f;
}

static void toneSetFreq(int hz) {
  s_phaseInc = (2.0f * (float)PI * (float)hz) / (float)AUDIO_SAMPLE_RATE;
  if (s_phaseInc > 0.5f) s_phaseInc = 0.5f;
  s_phase = 0.0f;
}

static int16_t toneNextSample() {
  s_phase += s_phaseInc;
  if (s_phase >= 2.0f * (float)PI) s_phase -= 2.0f * (float)PI;
  float sample = sinf(s_phase) * WAVE_PEAK * ALARM_VOLUME;
  return (int16_t)sample;
}

static bool audioSetupChannel() {
  if (s_inited) i2s_driver_uninstall(AUDIO_PORT);

  i2s_config_t cfg = {};
  cfg.mode               = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate        = AUDIO_SAMPLE_RATE;
  cfg.bits_per_sample    = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format     = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags   = ESP_INTR_FLAG_IRAM;
  cfg.dma_buf_count      = 6;
  cfg.dma_buf_len        = 256;
  cfg.use_apll           = false;
  cfg.tx_desc_auto_clear = true;
  cfg.fixed_mclk         = 0;
  cfg.mclk_multiple      = I2S_MCLK_MULTIPLE_DEFAULT;
  cfg.bits_per_chan      = I2S_BITS_PER_CHAN_DEFAULT;

  esp_err_t err = i2s_driver_install(AUDIO_PORT, &cfg, 3, nullptr);
  if (err != ESP_OK) {
    ELOG("Audio I2S init failed: %s", esp_err_to_name(err));
    return false;
  }

  i2s_pin_config_t pin = {};
  pin.bck_io_num   = SPK_BCLK_PIN;
  pin.ws_io_num    = SPK_LRC_PIN;
  pin.data_out_num = SPK_DOUT_PIN;
  pin.data_in_num  = I2S_PIN_NO_CHANGE;

  err = i2s_set_pin(AUDIO_PORT, &pin);
  if (err != ESP_OK) {
    ELOG("Audio I2S pin config failed: %s", esp_err_to_name(err));
    i2s_driver_uninstall(AUDIO_PORT);
    return false;
  }

  ELOG("Audio I2S DAC init OK (DOUT=%d BCLK=%d LRC=%d, %dHz)",
       SPK_DOUT_PIN, SPK_BCLK_PIN, SPK_LRC_PIN, AUDIO_SAMPLE_RATE);
  return true;
}

static void audioTaskFunc(void*) {
  int16_t* bufA = (int16_t*)malloc(2048 * sizeof(int16_t));
  int16_t* bufB = (int16_t*)malloc(2048 * sizeof(int16_t));
  if (!bufA || !bufB) { free(bufA); free(bufB); return; }

  int16_t* buf[2] = { bufA, bufB };
  size_t written = 0;

  while (s_inited) {
    int16_t* out = buf[written % 2];
    bool shouldTone = false;
    if (s_alarmActive && s_alarmToneOn && !s_muted) shouldTone = true;
    else if (s_testActive && !s_muted) shouldTone = true;

    for (size_t i = 0; i < 2048; i++) {
      out[i] = shouldTone ? toneNextSample() : 0;
    }

    size_t bytesW = 0;
    esp_err_t err = i2s_write(AUDIO_PORT, out, 2048 * sizeof(int16_t),
                              &bytesW, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
      ELOG("Audio write failed: %s", esp_err_to_name(err));
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    written++;
  }

  free(bufA); free(bufB);
  vTaskDelete(nullptr);
}

static TaskHandle_t s_audioTask = nullptr;

bool audioInit() {
  if (s_inited) return true;

  if (!audioSetupChannel()) return false;

  toneSetFreq(ALARM_FREQ_HZ);

  BaseType_t ok = xTaskCreatePinnedToCore(audioTaskFunc, "audio_tx",
      4096, nullptr, 3, &s_audioTask, 1);
  if (ok != pdPASS) {
    ELOG("Audio task creation failed");
    i2s_driver_uninstall(AUDIO_PORT);
    return false;
  }

  s_inited = true;
  ELOG("Audio system ready (DAC: MAX98357A, I2S_NUM_1, %dHz)", AUDIO_SAMPLE_RATE);
  return true;
}

void audioAlarm(bool enable) {
  if (!s_inited) return;
  if (enable && !s_alarmActive) {
    s_alarmActive  = true;
    s_alarmPhaseMs = millis();
    s_alarmToneOn  = true;
    toneSetFreq(ALARM_FREQ_HZ);
    ELOG("Alarm tone ON (freq=%dHz)", ALARM_FREQ_HZ);
  } else if (!enable && s_alarmActive) {
    s_alarmActive = false;
    s_alarmToneOn = false;
    ELOG("Alarm tone OFF");
  }
}

void audioPlayTestTone() {
  if (!s_inited) return;
  s_testActive  = true;
  s_testStartMs = millis();
  toneSetFreq(TEST_FREQ_HZ);
  ELOG("Test tone playing (%dHz, %ums)", TEST_FREQ_HZ, TEST_DURATION_MS);
}

void audioMute() { s_muted = true; }
void audioUnmute() { s_muted = false; }
bool audioIsMuted() { return s_muted; }

void audioUpdate() {
  if (!s_inited) return;
  if (s_alarmActive) {
    uint32_t elapsed = millis() - s_alarmPhaseMs;
    if (s_alarmToneOn && elapsed >= ALARM_DUTY_MS) {
      s_alarmToneOn = false;
      s_alarmPhaseMs = millis();
    } else if (!s_alarmToneOn && elapsed >= ALARM_GAP_MS) {
      s_alarmToneOn = true;
      s_alarmPhaseMs = millis();
    }
  }
  if (s_testActive) {
    if (millis() - s_testStartMs >= TEST_DURATION_MS) {
      s_testActive = false;
      toneSetFreq(ALARM_FREQ_HZ);
      ELOG("Test tone finished");
    }
  }
}

void audioShutdown() {
  if (!s_inited) return;
  s_inited = false;
  vTaskDelay(pdMS_TO_TICKS(100));
  i2s_driver_uninstall(AUDIO_PORT);
  s_audioTask = nullptr;
  ELOG("Audio system shut down");
}
