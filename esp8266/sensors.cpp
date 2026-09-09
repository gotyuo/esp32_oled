/*
 * 传感器驱动 (ESP8266)
 * 
 * - DHT11 温湿度: GPIO3 (注意与 ESP32 的 GPIO4 不同)
 * 
 * 库: DHT_sensor_library
 * 
 * ESP8266 没有:
 * - BMP280 气压传感器
 * - 麦克风
 * - 喇叭
 * - 心电监护
 */

#include "Arduino.h"
#include <DHT.h>
#include "envmon_esp8266.h"

// ========== DHT11 ==========
DHT dht(TEMP_PIN, DHT11);

// 校准数据
float temp_offset = 0.0;
float hum_offset = 0.0;

bool sensorsInit() {
  dht.begin();
  delay(2000);
  
  float t = dht.readTemperature();
  if (isnan(t)) {
    Serial.println("[DHT] 读取失败");
    return false;
  }
  Serial.printf("[DHT] 初始化成功，温度: %.1fC\n", t);
  
  return true;
}

void sensorsCalibrate() {
  Serial.println("传感器校准中...");
  
  float tempSum = 0, humSum = 0;
  int count = 10;
  
  for (int i = 0; i < count; i++) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    
    if (!isnan(t)) tempSum += t;
    if (!isnan(h)) humSum += h;
    
    delay(500);
  }
  
  temp_offset = tempSum / count;
  hum_offset = humSum / count;
  
  Serial.printf("校准完成: T=%.2f, H=%.2f\n", temp_offset, hum_offset);
}

void sensorsRead(SensorData& data) {
  data.valid = false;
  data.timestamp_ms = millis();
  
  // DHT11
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  
  if (isnan(t) || isnan(h)) {
    // 重试一次
    delay(100);
    t = dht.readTemperature();
    h = dht.readHumidity();
  }
  
  if (isnan(t) || isnan(h)) {
    Serial.println("[DHT] 读取失败");
    return;
  }
  
  data.temp_c = t;
  data.hum_pct = h;
  
  data.valid = true;
}

// 读取单个传感器值 (用于测试)
float readTemperature() {
  return dht.readTemperature();
}

float readHumidity() {
  return dht.readHumidity();
}
