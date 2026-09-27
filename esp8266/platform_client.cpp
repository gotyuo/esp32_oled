/*
 * HTTP 客户端 (ESP8266)
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

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ArduinoOTA.h>
#include "envmon_esp8266.h"

// 全局网络状态
bool netConnected = false;

// 前置声明
void triggerOTA(const String& version);

bool netInit() {
  netConnected = false;
  Serial.printf("[Net] 网络初始化完成，目标: %s:%d\n", SERVER_HOST, SERVER_PORT);
  return true;
}

// ========== 设备注册 ==========
bool netRegister() {
  HTTPClient http;
  WiFiClient client;
  
  String url = "http://" + String(SERVER_HOST) + ":" + String(SERVER_PORT) + "/api/devices";
  
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + AUTH_TOKEN);
  
  // 注册请求体
  StaticJsonDocument<256> doc;
  doc["id"] = DEVICE_ID;
  doc["name"] = DEVICE_NAME;
  doc["platform"] = "esp8266";
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
  
  HTTPClient http;
  WiFiClient client;
  
  String url = "http://" + String(SERVER_HOST) + ":" + String(SERVER_PORT) + INGEST_PATH;
  
  http.begin(client, url);
  http.setTimeout(5000);
  
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + AUTH_TOKEN);
  
  // 构建 JSON payload
  StaticJsonDocument<256> doc;
  doc["device_id"] = DEVICE_ID;
  
  // 传感器数据 (ESP8266 只有温湿度)
  if (!isnan(data.temp_c)) {
    doc["temp_c"] = data.temp_c;
  }
  if (!isnan(data.hum_pct)) {
    doc["hum_pct"] = data.hum_pct;
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
// 平台无 /api/heartbeat 端点, 心跳数据通过 /api/ingest 上报
// 心跳不携带传感器数据, 仅上报设备状态供平台更新 last_seen
void netHeartbeat() {
  if (!WiFi.isConnected()) return;
  
  HTTPClient http;
  WiFiClient client;
  
  String url = "http://" + String(SERVER_HOST) + ":" + String(SERVER_PORT) + INGEST_PATH;
  
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + AUTH_TOKEN);
  
  // 心跳 payload: 仅 device_id + timestamp, 无传感器数据
  // 平台 ingest 要求至少一个测量值, 使用占位值
  StaticJsonDocument<128> doc;
  doc["device_id"] = DEVICE_ID;
  doc["temp_c"] = -999.0;  // 占位值, 平台侧应忽略
  doc["timestamp"] = millis();
  
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
// 平台无 /api/ota/check 端点, 改用 GET /api/ota/list 获取固件列表
// 然后对比版本号判断是否需要升级
void otaCheck() {
  if (!OTA_ENABLED) return;
  
  // 防止 OTA 过程中重复检查
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < 60000) {
    return; // 每 60 秒检查一次
  }
  lastCheck = millis();
  
  HTTPClient http;
  WiFiClient client;
  
  String url = String("http://") + SERVER_HOST + ":" + SERVER_PORT + OTA_LIST_PATH;
  
  http.begin(client, url);
  http.addHeader("Authorization", String("Bearer ") + AUTH_TOKEN);
  
  int code = http.GET();
  
  if (code == 200) {
    String body = http.getString();
    
    // 解析固件列表
    DynamicJsonDocument doc(512);
    DeserializationError error = deserializeJson(doc, body);
    
    if (!error) {
      JsonArray images = doc["images"];
      if (images != nullptr && images.size() > 0) {
        // 取最后一个 (假设按版本排序)
        JsonObject lastImg = images[images.size() - 1];
        const char* newVer = lastImg["version"];
        
        if (newVer && strcmp(newVer, FIRMWARE_VERSION) > 0) {
          Serial.printf("[OTA] 发现新版本: %s\n", newVer);
          
          // 触发 OTA 更新
          triggerOTA(newVer);
        }
      }
    }
  } else if (code != 404) {
    Serial.printf("[OTA] 检查失败: HTTP %d\n", code);
  }
  
  http.end();
}

// ========== 触发 OTA 更新 ==========
// 平台无 /api/ota/download 端点, 使用 /api/ota/push/{device_id} 触发 OTA
// 实际 OTA 下载由 ArduinoOTA 回调处理
void triggerOTA(const String& version) {
  Serial.printf("[OTA] 请求平台推送版本: %s\n", version.c_str());
  oledShowOTA(0);
  
  HTTPClient http;
  WiFiClient client;
  
  // 使用 /api/ota/push/{device_id} 端点
  String url = String("http://") + SERVER_HOST + ":" + SERVER_PORT + "/api/ota/push/";
  url += DEVICE_ID;
  
  http.begin(client, url);
  http.addHeader("Authorization", String("Bearer ") + AUTH_TOKEN);
  http.addHeader("Content-Type", "application/json");
  
  // 请求体: 指定目标版本
  StaticJsonDocument<128> doc;
  doc["target_version"] = version;
  
  String payload;
  serializeJson(doc, payload);
  
  int code = http.POST(payload);
  http.end();
  
  if (code == 200) {
    Serial.println("[OTA] 已请求 OTA 推送, 等待下载...");
    oledShowOTA(50);
    
    // ArduinoOTA 回调会处理实际下载和写入
    // 这里等待一段时间后重启
    delay(5000);
    
    Serial.println("[OTA] 重启设备以应用更新");
    oledShowOTA(100);
    delay(2000);
    ESP.restart();
  } else {
    Serial.printf("[OTA] 触发失败: HTTP %d\n", code);
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
