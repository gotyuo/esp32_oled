/**
 * =============================================================================
 * EnvMon ESP32 - 传感器读取驱动 (sensors.cpp)
 * =============================================================================
 *
 * 传感器:
 *   - DHT22  温湿度   GPIO4 (DHT22)
 *   - BMP280 气压     I2C (SDA=8, SCL=9)
 *   - I2S    麦克风   SD=-1, SCK=-1, WS=-1 (暂时禁用)
 *
 * 版本: v8.0.0
 * =============================================================================
 */

#include "envmon_esp32.h"
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP280.h>
#include <DHT.h>
#include <driver/i2s.h>

namespace {
  DHT dht(TEMP_PIN, DHT22);
  Adafruit_BMP280 bmp;

  i2s_port_t s_micI2sPort = I2S_NUM_0;
  bool s_micInitialized = false;

  float s_tempOffset   = 0.0;
  float s_presOffset   = 0.0;
  float s_noiseFloorDb = 30.0;

  uint32_t s_tempFailStreak   = 0;
  uint32_t s_humFailStreak    = 0;
  uint32_t s_presFailStreak   = 0;
  uint32_t s_noiseFailStreak  = 0;

  bool s_calibrated = false;
}

static bool readDht22(float* tempC, float* humPct) {
  for (uint8_t attempt = 0; attempt < SENSOR_RETRY_ATTEMPTS; attempt++) {
    float h = dht.readHumidity();
    float t = dht.readTemperature();
    if (!isnan(h) && !isnan(t) && h >= 0.0 && t > -50.0) {
      *humPct = h;
      *tempC  = t + s_tempOffset;
      return true;
    }
    delay(SENSOR_RETRY_DELAY_MS);
  }
  return false;
}

static bool readBmp280(float* presHpa) {
  for (uint8_t attempt = 0; attempt < SENSOR_RETRY_ATTEMPTS; attempt++) {
    float p = bmp.readPressure();
    if (!isnan(p) && p > 0.0) {
      *presHpa = (p / 100.0f) + s_presOffset;
      return true;
    }
    delay(SENSOR_RETRY_DELAY_MS);
  }
  return false;
}

static bool initMicI2s() {
  if (s_micInitialized) return true;

  if (MIC_SD_PIN < 0 || MIC_SCK_PIN < 0 || MIC_WS_PIN < 0) {
    ELOG("I2S mic disabled (pins not set)");
    return false;
  }

  i2s_config_t i2s_config = {};
  i2s_config.mode               = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  i2s_config.sample_rate        = 16000;
  i2s_config.bits_per_sample    = I2S_BITS_PER_SAMPLE_16BIT;
  i2s_config.channel_format     = I2S_CHANNEL_FMT_ONLY_LEFT;
  i2s_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2s_config.intr_alloc_flags   = ESP_INTR_FLAG_IRAM;
  i2s_config.dma_buf_count      = 6;
  i2s_config.dma_buf_len        = 256;
  i2s_config.use_apll           = false;
  i2s_config.tx_desc_auto_clear = false;
  i2s_config.fixed_mclk         = 0;
  i2s_config.mclk_multiple      = I2S_MCLK_MULTIPLE_DEFAULT;
  i2s_config.bits_per_chan      = I2S_BITS_PER_CHAN_DEFAULT;

  esp_err_t err = i2s_driver_install(s_micI2sPort, &i2s_config, 3, nullptr);
  if (err != ESP_OK) {
    ELOG("I2S mic init failed: %s", esp_err_to_name(err));
    return false;
  }

  i2s_pin_config_t pin_config = {};
  pin_config.bck_io_num   = MIC_SCK_PIN;
  pin_config.ws_io_num    = MIC_WS_PIN;
  pin_config.data_out_num = I2S_PIN_NO_CHANGE;
  pin_config.data_in_num  = MIC_SD_PIN;

  err = i2s_set_pin(s_micI2sPort, &pin_config);
  if (err != ESP_OK) {
    ELOG("I2S mic pin config failed: %s", esp_err_to_name(err));
    i2s_driver_uninstall(s_micI2sPort);
    return false;
  }

  s_micInitialized = true;
  ELOG("I2S mic init OK (SD=%d SCK=%d WS=%d)",
       MIC_SD_PIN, MIC_SCK_PIN, MIC_WS_PIN);
  return true;
}

static bool readNoiseDb(float* noiseDb) {
  if (!s_micInitialized && !initMicI2s()) return false;

  const size_t bufSamples = 1600;
  int16_t* buf = (int16_t*)malloc(bufSamples * sizeof(int16_t));
  if (buf == nullptr) return false;

  size_t bytesRead = 0;
  esp_err_t err = i2s_read(s_micI2sPort, buf, bufSamples * sizeof(int16_t),
                           &bytesRead, portMAX_DELAY);
  if (err != ESP_OK || bytesRead < (bufSamples * sizeof(int16_t) / 2)) {
    free(buf);
    return false;
  }

  double sumSq = 0.0;
  int count = bytesRead / (int)sizeof(int16_t);
  for (int i = 0; i < count; i++) {
    sumSq += (double)buf[i] * (double)buf[i];
  }
  free(buf);

  double rms = sqrt(sumSq / count);
  if (rms < 1.0) rms = 1.0;
  double dbSpl = 20.0 * log10(rms / 32767.0) + 120.0;
  double effective = dbSpl - s_noiseFloorDb;
  if (effective < 0) effective = 0;

  *noiseDb = (float)effective;
  return true;
}

static bool ensureSensorsReady() {
  static bool initialized = false;
  if (initialized) return true;

  Wire.begin(BMP280_SDA, BMP280_SCL);
  if (!bmp.begin(BMP280_ADDRESS)) {
    ELOG("BMP280 init failed @0x%02X", BMP280_ADDRESS);
    if (!bmp.begin(0x76)) {
      ELOG("BMP280 init FAILED - pressure unavailable");
    } else {
      ELOG("BMP280 init OK @0x76");
    }
  } else {
    ELOG("BMP280 init OK @0x%02X", BMP280_ADDRESS);
  }

  dht.begin();
  ELOG("DHT22 begin (pin %d)", TEMP_PIN);

  initialized = true;
  return true;
}

SensorReading readSensors() {
  ensureSensorsReady();

  SensorReading r;
  r.timestamp = millis();
  r.temp_c = NAN; r.valid_temp = false;
  r.hum_pct = NAN; r.valid_hum = false;
  r.pres_hpa = NAN; r.valid_pres = false;
  r.noise_db = NAN; r.valid_noise = false;

  if (readDht22(&r.temp_c, &r.hum_pct)) {
    r.valid_temp = true;
    r.valid_hum  = true;
    s_tempFailStreak = 0;
    s_humFailStreak  = 0;
  } else {
    s_tempFailStreak++;
    s_humFailStreak++;
    ELOG("DHT22 read failed (streak: temp=%u hum=%u)",
         s_tempFailStreak, s_humFailStreak);
  }

  if (readBmp280(&r.pres_hpa)) {
    r.valid_pres = true;
    s_presFailStreak = 0;
  } else {
    s_presFailStreak++;
    ELOG("BMP280 read failed (streak: %u)", s_presFailStreak);
  }

  if (readNoiseDb(&r.noise_db)) {
    r.valid_noise = true;
    s_noiseFailStreak = 0;
  } else {
    s_noiseFailStreak++;
    ELOG("Mic noise read failed (streak: %u)", s_noiseFailStreak);
  }

  g_state.sensorReadCount++;
  if (!r.anyValid()) {
    g_state.sensorFailCount++;
    ELOG("ALL sensors failed this cycle (total fails: %u)",
         g_state.sensorFailCount);
  }

  return r;
}

bool calibrateSensors() {
  ELOG("=== Starting sensor calibration ===");
  ensureSensorsReady();

  delay(2000);

  SensorReading ref = readSensors();

  if (!ref.valid_temp) {
    ELOG("Calibration FAILED: DHT22 unavailable");
    return false;
  }
  if (!ref.valid_pres) {
    ELOG("Calibration FAILED: BMP280 unavailable");
    return false;
  }

  s_tempOffset = -ref.temp_c;
  s_presOffset = -ref.pres_hpa;

  if (ref.valid_noise) {
    s_noiseFloorDb = ref.noise_db;
    ELOG("Noise floor set: %.1f dB", s_noiseFloorDb);
  }

  s_calibrated = true;
  ELOG("Calibration OK: temp_off=%.2fC pres_off=%.2fhPa",
       s_tempOffset, s_presOffset);
  return true;
}

bool sensorsIsCalibrated() {
  return s_calibrated;
}
