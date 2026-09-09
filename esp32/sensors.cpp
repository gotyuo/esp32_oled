/**
 * =============================================================================
 * EnvMon ESP32 - 传感器读取驱动 (sensors.cpp)
 * =============================================================================
 *
 * 传感器:
 *   - DHT22  温湿度   GPIO4
 *   - BMP280 气压     I2C (SDA=21, SCL=22)
 *   - I2S    麦克风   SD=34, SCK=32, WS=25  (噪声估算)
 *
 * 提供:
 *   - readSensors()      读取所有传感器, 返回 SensorReading
 *   - calibrateSensors() 校准 (零点偏置, 噪声基准)
 *
 * 错误处理策略:
 *   - 每个传感器独立读取, 单个故障不影响其他
 *   - 读取失败按 SENSOR_RETRY_ATTEMPTS 重试
 *   - 失败时字段 valid_* = false, 数值置 NaN
 *   - 连续失败计数 (g_state.sensorFailCount) 供诊断
 *
 * 版本: v2.0.0
 * =============================================================================
 */

#include "envmon_esp32.h"

#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP280.h>

#if ENVMON_LOG_ENABLE
// DHT22 库
#include <DHT.h>
#endif

// 麦克风: 使用 ESP32 I2S 驱动
#include <driver/i2s.h>

// =============================================================================
// 模块内部状态
// =============================================================================
namespace {
  // DHT22 实例
  DHT dht(TEMP_PIN, DHT22);

  // BMP280 实例
  Adafruit_BMP280 bmp;

  // I2S 麦克风通道句柄
  i2s_port_t s_micI2sPort = I2S_NUM_0;
  bool s_micInitialized = false;

  // 校准偏置 (零点): 用于补偿传感器系统误差
  float s_tempOffset   = 0.0;   // 温度零点偏置 (摄氏度)
  float s_presOffset   = 0.0;   // 气压零点偏置 (hPa)
  float s_noiseFloorDb = 30.0;  // 噪声底噪 (dB), 用于估算有效噪声

  // 连续失败计数 (每传感器独立, 用于故障诊断/上报)
  uint32_t s_tempFailStreak   = 0;
  uint32_t s_humFailStreak    = 0;
  uint32_t s_presFailStreak   = 0;
  uint32_t s_noiseFailStreak  = 0;

  // 校准状态
  bool s_calibrated = false;
}

// =============================================================================
// 内部: DHT22 温湿度读取 (带重试)
// =============================================================================
static bool readDht22(float* tempC, float* humPct) {
  for (uint8_t attempt = 0; attempt < SENSOR_RETRY_ATTEMPTS; attempt++) {
    float h = dht.readHumidity();
    float t = dht.readTemperature();

    // DHT 库在失败时返回 -1 或 NaN
    if (!isnan(h) && !isnan(t) && h >= 0.0 && t > -50.0) {
      *humPct = h;
      *tempC  = t + s_tempOffset;   // 应用校准偏置
      return true;
    }
    delay(SENSOR_RETRY_DELAY_MS);
  }
  return false;
}

// =============================================================================
// 内部: BMP280 气压读取 (带重试)
// =============================================================================
static bool readBmp280(float* presHpa) {
  for (uint8_t attempt = 0; attempt < SENSOR_RETRY_ATTEMPTS; attempt++) {
    float p = bmp.readPressure();   // 返回 Pa
    if (!isnan(p) && p > 0.0) {
      *presHpa = (p / 100.0f) + s_presOffset;  // Pa -> hPa, 应用偏置
      return true;
    }
    delay(SENSOR_RETRY_DELAY_MS);
  }
  return false;
}

// =============================================================================
// 内部: I2S 麦克风初始化
// =============================================================================
static bool initMicI2s() {
  if (s_micInitialized) return true;

  i2s_config_t i2s_config = {};
  i2s_config.rx_mode      = I2S_MODE_RX;
  i2s_config.tx_mode      = I2S_MODE_DISABLED;
  i2s_config.use_apll     = false;
  i2s_config.duplex_mode  = I2S_MODE_HALFDUPLEX;
  i2s_config.i2s_port     = s_micI2sPort;
  i2s_config.sample_rate  = 16000;          // 16kHz 采样率
  i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  i2s_config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  i2s_config.comm_format    = I2S_COMM_FORMAT_I2S;
  i2s_config.intr_alloc     = I2S_INT_ALLOC_MIN;
  i2s_config.auto_clear     = false;

  esp_err_t err = i2s_init_channel(&i2s_config);
  if (err != ESP_OK) {
    ELOG("I2S mic init failed: %s", esp_err_to_name(err));
    return false;
  }

  i2s_pin_config_t pin_config = {};
  pin_config.bclk_io_num   = MIC_SCK_PIN;
  pin_config.ws_io_num     = MIC_WS_PIN;
  pin_config.data_out_num  = I2S_PIN_NO_CHANGE;
  pin_config.data_in_num   = MIC_SD_PIN;

  err = i2s_set_pin(s_micI2sPort, &pin_config);
  if (err != ESP_OK) {
    ELOG("I2S mic pin config failed: %s", esp_err_to_name(err));
    return false;
  }

  err = i2s_channel_enable(s_micI2sPort, true);
  if (err != ESP_OK) {
    ELOG("I2S mic enable failed: %s", esp_err_to_name(err));
    return false;
  }

  s_micInitialized = true;
  ELOG("I2S mic init OK (SD=%d SCK=%d WS=%d)",
       MIC_SD_PIN, MIC_SCK_PIN, MIC_WS_PIN);
  return true;
}

// =============================================================================
// 内部: 噪声估算
//
// 从一段 I2S 采样计算 RMS, 再换算为近似 dB(A)。
// 注意: 这是基于声压级的粗略估算, 未经专业校准, 仅供环境趋势参考。
// =============================================================================
static bool readNoiseDb(float* noiseDb) {
  if (!s_micInitialized && !initMicI2s()) {
    return false;
  }

  // 读取 100ms 采样窗口 (16000 * 0.1 = 1600 samples)
  const size_t bufSamples = 1600;
  int16_t* buf = (int16_t*)heap_caps_malloc(
      bufSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  if (buf == nullptr) {
    // SPIRAM 不足, 退回普通堆
    buf = (int16_t*)malloc(bufSamples * sizeof(int16_t));
    if (buf == nullptr) return false;
  }

  size_t bytesRead = 0;
  esp_err_t err = i2s_channel_read(s_micI2sPort, buf, bufSamples * sizeof(int16_t),
                                   &bytesRead, portMAX_DELAY);
  if (err != ESP_OK || bytesRead < (bufSamples * sizeof(int16_t) / 2)) {
    free(buf);
    return false;
  }

  // 计算 RMS (峰值归一化到 32767)
  double sumSq = 0.0;
  int count = bytesRead / (int)sizeof(int16_t);
  int16_t peak = 0;
  for (int i = 0; i < count; i++) {
    sumSq += (double)buf[i] * (double)buf[i];
    int16_t abs = buf[i] < 0 ? -buf[i] : buf[i];
    if (abs > peak) peak = abs;
  }
  free(buf);

  double rms = sqrt(sumSq / count);
  if (rms < 1.0) rms = 1.0;  // 避免 log10 负数

  // 声压级估算: 假设传感器满量程 32767 对应 ~120 dB SPL
  double dbSpl = 20.0 * log10(rms / 32767.0) + 120.0;
  // 减去底噪, 得到有效噪声
  double effective = dbSpl - s_noiseFloorDb;
  if (effective < 0) effective = 0;

  *noiseDb = (float)effective;
  return true;
}

// =============================================================================
// 对外接口
// =============================================================================

/**
 * @brief 初始化所有传感器 (在 setup 中调用)
 *
 * 注意: 此函数由各传感器模块的 init 间接调用, 但本模块不暴露
 *       独立 init, 因为 DHT/BMP 的初始化可在 readSensors 内惰性完成。
 *       如需预初始化可调用 calibrateSensors()。
 */
static bool ensureSensorsReady() {
  static bool initialized = false;
  if (initialized) return true;

  // BMP280 (I2C, 与 OLED 共用总线)
  Wire.begin(BMP280_SDA, BMP280_SCL);
  if (!bmp.begin(BMP280_ADDRESS)) {
    ELOG("BMP280 init failed @0x%02X", BMP280_ADDRESS);
    // 尝试备用地址 0x77
    if (!bmp.begin(0x77)) {
      ELOG("BMP280 init FAILED - pressure unavailable");
    } else {
      ELOG("BMP280 init OK @0x77");
    }
  } else {
    ELOG("BMP280 init OK @0x%02X", BMP280_ADDRESS);
  }

  // DHT22
  dht.begin();
  ELOG("DHT22 begin (pin %d)", TEMP_PIN);

  initialized = true;
  return true;
}

/**
 * @brief 读取所有传感器, 返回采样结果
 *
 * @return SensorReading 各字段有效性由 valid_* 指示
 *
 * 设计:
 *   - 首次调用惰性初始化传感器
 *   - 每传感器独立重试, 互不阻塞
 *   - 全失败时 anyValid() 为 false, 调用方应跳过上报/显示
 */
SensorReading readSensors() {
  ensureSensorsReady();

  SensorReading r;
  r.timestamp = millis();
  r.temp_c = NAN; r.valid_temp = false;
  r.hum_pct = NAN; r.valid_hum = false;
  r.pres_hpa = NAN; r.valid_pres = false;
  r.noise_db = NAN; r.valid_noise = false;

  // 温湿度 (DHT22)
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

  // 气压 (BMP280)
  if (readBmp280(&r.pres_hpa)) {
    r.valid_pres = true;
    s_presFailStreak = 0;
  } else {
    s_presFailStreak++;
    ELOG("BMP280 read failed (streak: %u)", s_presFailStreak);
  }

  // 噪声 (麦克风)
  if (readNoiseDb(&r.noise_db)) {
    r.valid_noise = true;
    s_noiseFailStreak = 0;
  } else {
    s_noiseFailStreak++;
    // 噪声读取失败较常见 (硬件未接), 仅 debug 级别提示
    ELOG("Mic noise read failed (streak: %u)", s_noiseFailStreak);
  }

  // 统计更新
  g_state.sensorReadCount++;
  if (!r.anyValid()) {
    g_state.sensorFailCount++;
    ELOG("ALL sensors failed this cycle (total fails: %u)",
         g_state.sensorFailCount);
  }

  // 连续失败达阈值时打印告警, 便于现场判断是否需更换传感器
  if (s_tempFailStreak >= 10 && (s_tempFailStreak % 10 == 0)) {
    ELOG("WARNING: DHT22 continuously failing, check wiring");
  }
  if (s_presFailStreak >= 10 && (s_presFailStreak % 10 == 0)) {
    ELOG("WARNING: BMP280 continuously failing, check I2C");
  }

  return r;
}

/**
 * @brief 校准传感器
 *
 * 校准内容:
 *   - 读取当前读数作为零点基准 (用户可在已知环境下调用)
 *   - 测量当前底噪作为 noiseFloorDb
 *
 * @return true 校准成功; false 关键传感器不可用
 *
 * 使用建议: 在已知标准环境下调用一次, 将结果保存到 NVS 持久化
 *           (当前版本仅内存保存, 重启后需重新校准)。
 */
bool calibrateSensors() {
  ELOG("=== Starting sensor calibration ===");
  ensureSensorsReady();

  // 等待传感器稳定 (DHT22 预热约 2 秒)
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

  // 以当前读数作为零点 (偏置 = -当前读数)
  s_tempOffset = -ref.temp_c;
  s_presOffset = -ref.pres_hpa;

  // 测量底噪
  if (ref.valid_noise) {
    s_noiseFloorDb = ref.noise_db;
    ELOG("Noise floor set: %.1f dB", s_noiseFloorDb);
  }

  s_calibrated = true;
  ELOG("Calibration OK: temp_off=%.2fC pres_off=%.2fhPa",
       s_tempOffset, s_presOffset);
  return true;
}

/**
 * @brief 获取校准状态 (供诊断用)
 */
bool sensorsIsCalibrated() {
  return s_calibrated;
}
