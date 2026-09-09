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

// ========== DHT11 实例 ==========
DHT dht(TEMP_PIN, DHT_TYPE);

// ========== 校准参数 ==========
static float tempOffset = 0.0;
static float humOffset  = 0.0;

// ========== 初始化 ==========
bool sensorsInit() {
  Serial.println("[Sensor] DHT11 初始化...");
  
  dht.begin();
  
  // DHT11 需要短暂延时后首次读取
  delay(2000);
  
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  
  if (isnan(t) || isnan(h)) {
    Serial.println("[Sensor] DHT11 读取失败!");
    return false;
  }
  
  Serial.printf("[Sensor] DHT11 OK: %.1f°C, %.1f%%\n", t, h);
  return true;
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
  // DHT11 最低采样频率 0.5Hz (每 2 秒一次)
  static unsigned long lastReadTime = 0;
  
  if (millis() - lastReadTime < 2000) {
    data.valid = false;
    return;
  }
  
  lastReadTime = millis();
  
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  
  if (isnan(h) || isnan(t)) {
    data.valid = false;
    return;
  }
  
  // 应用校准偏移
  data.hum_pct = h + humOffset;
  data.temp_c = t + tempOffset;
  
  // 气压传感器不可用 (ESP8266 无 BMP280)
  data.pres_hpa = 0.0;
  
  data.timestamp_ms = millis();
  data.valid = true;
}
