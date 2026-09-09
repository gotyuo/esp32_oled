/*
 * HTTP 客户端 (ESP32)
 * 
 * 协议: HTTP POST 到 /api/ingest
 * 鉴权: Authorization: Bearer <token>
 * 
 * 功能:
 * - 设备注册
 * - 数据上报
 * - 心跳
 * - OTA 检查
 * 
 * 服务器: 192.168.68.119:12090
 */

#include "Arduino.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "envmon_esp32.h"

// 全局网络状态
bool netConnected = false;
unsigned long lastNetError = 0;

bool netInit() {
  WiFiClientSecure client;
  client.setInsecure();
  
  // 测试连接
  if (!WiFi.isConnected()) {
    Serial.println("[Net] WiFi 未连接");
    return false;
  }
  
  netConnected = true;
  Serial.printf("[Net] 初始化成功，目标: %s:%d\n", SERVER_HOST, SERVER_PORT);
  return true;
}

// ========== 设备注册 ==========
bool netRegister() {
  WiFiClient client;
  HTTPClient http;
  
  String url = "http://" + String(SERVER_HOST) + ":" + String(SERVER_PORT) + "/api/devices";
  
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + AUTH_TOKEN);
  
  // 注册请求体
  StaticJsonDocument<512> doc;
  doc["id"] = DEVICE_ID;
  doc["name"] = DEVICE_NAME;
  doc["platform"] = "esp32";
  doc["firmware_version"] = FIRMWARE_VERSION;
  
  String payload;
  serializeJson(doc, payload);
  
  int code = http.POST(payload);
  Serial.printf("[Register] HTTP %d: %s\n", code, http.errorToString(code).c_str());
  
  if (code == 201 || code == 200) {
    Serial.println("[Register] 设备注册成功");
    http.end();
    return true;
  }
  
  if (code == 400) {
    Serial.println("[Register] 设备已存在，继续");
    http.end();
    return true;
  }
  
  http.end();
  return false;
}

// ========== 数据上报 ==========
bool netReport(const SensorData& data) {
  if (!data.valid) {
    Serial.println("[Report] 数据无效，跳过");
    return false;
  }
  
  WiFiClient client;
  HTTPClient http;
  
  String url = "http://" + String(SERVER_HOST) + ":" + String(SERVER_PORT) + INGEST_PATH;
  
  http.begin(client, url);
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + AUTH_TOKEN);
  
  // 构建 JSON payload
  StaticJsonDocument<512> doc;
  doc["device_id"] = DEVICE_ID;
  
  // 传感器数据
  if (!isnan(data.temp_c)) {
    doc["temp_c"] = data.temp_c;
  }
  if (!isnan(data.hum_pct)) {
    doc["hum_pct"] = data.hum_pct;
  }
  if (!isnan(data.pres_hpa)) {
    doc["pres_hpa"] = data.pres_hpa;
  }
  
  // 麦克风采样
  if (data.mic_sample != 0) {
    doc["mic_sample"] = data.mic_sample;
  }
  
  // 心电监护 (预留)
  if (EKG_ENABLE && data.ekg_sample != 0) {
    doc["ekg_sample"] = data.ekg_sample;
  }
  
  // 时间戳
  doc["timestamp"] = millis();
  
  String payload;
  serializeJson(doc, payload);
  
  // 发送
  int code = http.POST(payload);
  
  if (code == 200) {
    Serial.printf("[Report] 上报成功: %s\n", payload.c_str());
    http.end();
    return true;
  }
  
  Serial.printf("[Report] 上报失败: HTTP %d %s\n", 
                code, http.errorToString(code).c_str());
  
  if (code == 401) {
    Serial.println("[Report] 认证失败，检查 token");
  } else if (code == 404) {
    Serial.println("[Report] 端点不存在，检查服务器配置");
  }
  
  http.end();
  return false;
}

// ========== 心跳 ==========
void netHeartbeat() {
  WiFiClient client;
  HTTPClient http;
  
  String url = "http://" + String(SERVER_HOST) + ":" + String(SERVER_PORT) + "/api/heartbeat";
  
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + AUTH_TOKEN);
  
  StaticJsonDocument<256> doc;
  doc["device_id"] = DEVICE_ID;
  doc["timestamp"] = millis();
  doc["status"] = WiFi.status() == WL_CONNECTED ? "online" : "offline";
  
  String payload;
  serializeJson(doc, payload);
  
  int code = http.POST(payload);
  
  if (code == 200) {
    Serial.println("[Heartbeat] 心跳成功");
  } else {
    Serial.printf("[Heartbeat] 心跳失败: HTTP %d\n", code);
  }
  
  http.end();
}

// ========== OTA 检查 ==========
void otaCheck() {
  if (!OTA_ENABLED) return;
  
  // 使用 HTTPUpdate 库检查 OTA
  // 需要在 setup() 中初始化 HTTPUpdate
  // 这里简化处理，实际应由 httpUpdateServer 管理
  
  // 检查服务器是否有新版本
  WiFiClient client;
  HTTPClient http;
  
  String url = "http://" + String(SERVER_HOST) + ":" + String(SERVER_PORT) + "/api/ota/check";
  url += "?device=";
  url += DEVICE_ID;
  url += "&version=";
  url += FIRMWARE_VERSION;
  
  http.begin(client, url);
  http.addHeader("Authorization", String("Bearer ") + AUTH_TOKEN);
  
  int code = http.GET();
  
  if (code == 200) {
    String body = http.getString();
    
    // 解析 OTA 信息
    StaticJsonDocument<256> doc;
    DeserializationError error = parseJson(doc, body);
    
    if (!error) {
      if (doc["available"]) {
        String newVersion = doc["version"];
        Serial.printf("[OTA] 发现新版本: %s\n", newVersion.c_str());
        
        // 触发 OTA 更新
        triggerOTA(newVersion);
      }
    }
  } else if (code != 404) {
    Serial.printf("[OTA] 检查失败: HTTP %d\n", code);
  }
  
  http.end();
}

// ========== 触发 OTA 更新 ==========
void triggerOTA(const String& version) {
  Serial.printf("[OTA] 开始更新到 %s\n", version.c_str());
  
  oledShowOTA(0);
  
  // 使用 HTTPUpdate 库
  HTTPUpdate httpUpdate;
  WiFiClient client;
  
  String url = "http://" + String(SERVER_HOST) + ":" + String(SERVER_PORT) + "/api/ota/download";
  url += "?version=";
  url += version;
  
  int code = httpUpdate.update(client, url, String(), String("Bearer ") + AUTH_TOKEN);
  
  if (code > 0) {
    int progress = httpUpdate.progress();
    oledShowOTA(progress);
    
    if (progress >= 100) {
      Serial.println("[OTA] 更新成功，重启...");
      oledShowOTA(100);
      delay(2000);
      ESP.restart();
    }
  } else {
    Serial.printf("[OTA] 更新失败: %d\n", code);
    oledShowStatus("OTA 失败");
  }
}

// ========== 网络状态查询 ==========
bool isNetConnected() {
  return netConnected && WiFi.isConnected();
}

void printNetStatus() {
  Serial.printf("[Net] 状态: %s\n", isNetConnected() ? "已连接" : "断开");
  Serial.printf("       IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("       SSID: %s\n", WiFi.SSID().c_str());
  Serial.printf("       RSSI: %d dBm\n", WiFi.RSSI());
}
