/*
 * EnvMon ESP8266 - 传感器驱动
 *
 * 传感器:
 *   - DHT11 温湿度 (GPIO3)
 *
 * ESP8266 与 ESP32 的区别:
 *   - 使用 DHT11 (ESP32 使用 DHT22)
 *   - 无麦克风、喇叭、心电监护支持
 *   - 无 BMP280 气压传感器 (ESP32 有)
 *
 * DHT11 精度: ±2°C 温度, ±5% 湿度
 * 采样间隔: 至少 2 秒 (DHT11 官方建议)
 */

#include <Arduino.h>
#include <DHT.h>

#include "envmon_esp8266.h"
#include "max30102_config.h"
#include "bp_calibration.h"

// ========== DHT11 实例 ==========
DHT dht(TEMP_PIN, DHT_TYPE);

// ========== 校准参数 ==========
static float tempOffset = 0.0;
static float humOffset  = 0.0;

static bool isNanValue(float x) {
  return x != x;
}

// ========== 初始化 ==========
bool sensorsInit() {
  Serial.println("[Sensor] DHT11 初始化...");

  pinMode(TEMP_PIN, INPUT_PULLUP);
  Serial.printf("[Sensor] DHT11 pin %d initial level=%d\n", TEMP_PIN, digitalRead(TEMP_PIN));

  dht.begin();

  for (int attempt = 0; attempt < 3; attempt++) {
    delay(2000);

    float t = dht.readTemperature();
    float h = dht.readHumidity();

    if (!isNanValue(t) && !isNanValue(h)) {
      Serial.printf("[Sensor] DHT11 OK: %.1f°C, %.1f%%\n", t, h);
      return true;
    }

    Serial.printf("[Sensor] DHT11 read failed attempt=%d temp_nan=%d hum_nan=%d\n", attempt + 1, isNanValue(t), isNanValue(h));
  }

  Serial.println("[Sensor] DHT11 读取失败!");
  return false;
}

// ========== 校准 ==========
void sensorsCalibrate() {
  // ESP8266 无气压传感器，只做温湿度偏移校准
  // 默认零偏移
  tempOffset = 0.0;
  humOffset  = 0.0;
  Serial.println("[Sensor] 校准完成 (默认零偏移)");
}

// ========== 读取传感器 ==========
void sensorsRead(SensorData& data) {
  // 先重置, 让无效字段保持 0
  data.valid = false;
  data.vital_valid = false;

  // DHT11 最低采样频率 0.5Hz (每 2 秒一次)
  static unsigned long lastReadTime = 0;

  if (millis() - lastReadTime >= 2000) {
    lastReadTime = millis();

    float h = dht.readHumidity();
    float t = dht.readTemperature();

    if (!isNanValue(h) && !isNanValue(t)) {
      data.hum_pct = h + humOffset;
      data.temp_c = t + tempOffset;
      data.timestamp_ms = millis();
      data.valid = true;
    }
  }

  // MAX30102 血氧/心率 (v2.1.0 新增)
  if (max30102IsInitialized()) {
    float spo2, hr;
    bool vital_ok;
    max30102GetResult(&spo2, &hr, &vital_ok);
    if (vital_ok) {
      data.spo2 = spo2;
      data.heart_rate = hr;
      data.vital_valid = true;

      // 血压估算 (v2.2.0; v2.3.0 启用个体基线校准)
      #if BP_ESTIMATE_ENABLED
      float sbp, dbp; bool bp_ok;
      max30102GetBpResult(&sbp, &dbp, &bp_ok);
      if (bp_ok) {
        data.bp_systolic = sbp;
        data.bp_diastolic = dbp;
        data.bp_calibrated = bpCalibIsCalibrated();
      } else {
        data.bp_calibrated = false;
      }
      #endif
    }
  }
}
