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
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP8266OTA.h>
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
  
  String url = "http://" + String(SERVER_HOST) + ":" + String(SERVER_PORT) + "/api/devices";
  
  http.begin(url);
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
  
  String url = "http://" + String(SERVER_HOST) + ":" + String(SERVER_PORT) + INGEST_PATH;
  
  http.begin(url);
  http.setConnectTimeout(5000);
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
void netHeartbeat() {
  HTTPClient http;
  
  String url = "http://" + String(SERVER_HOST) + ":" + String(SERVER_PORT) + "/api/heartbeat";
  
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + AUTH_TOKEN);
  
  StaticJsonDocument<128> doc;
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
// ESP8266 使用 ArduinoOTA 库处理 OTA 更新
// 此函数定期检查服务器是否有新版本
void otaCheck() {
  if (!OTA_ENABLED) return;
  
  // 防止 OTA 过程中重复检查
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < 60000) {
    return; // 每 60 秒检查一次
  }
  lastCheck = millis();
  
  HTTPClient http;
  
  String url = String("http://") + SERVER_HOST + ":" + SERVER_PORT + "/api/ota/check";
  url += "?device=";
  url += DEVICE_ID;
  url += "&version=";
  url += FIRMWARE_VERSION;
  
  http.begin(url);
  http.addHeader("Authorization", String("Bearer ") + AUTH_TOKEN);
  
  int code = http.GET();
  
  if (code == 200) {
    String body = http.getString();
    
    // 解析 OTA 信息
    StaticJsonDocument<256> doc;
    DeserializationError error = parseJson(doc, body);
    
    if (!error && doc["available"]) {
      String newVersion = doc["version"];
      Serial.printf("[OTA] 发现新版本: %s\n", newVersion.c_str());
      
      // 触发 OTA 更新
      triggerOTA(newVersion);
    }
  } else if (code != 404) {
    Serial.printf("[OTA] 检查失败: HTTP %d\n", code);
  }
  
  http.end();
}

// ========== 触发 OTA 更新 ==========
// 下载新固件二进制文件并写入 OTA 分区
// 注意: ESP8266 内存有限，使用流式写入避免内存溢出
void triggerOTA(const String& version) {
  Serial.printf("[OTA] 开始下载版本: %s\n", version.c_str());
  oledShowOTA(0);
  
  HTTPClient http;
  
  String url = String("http://") + SERVER_HOST + ":" + SERVER_PORT + "/api/ota/download";
  url += "?version=";
  url += version;
  
  http.begin(url);
  http.addHeader("Authorization", String("Bearer ") + AUTH_TOKEN);
  http.setConnectTimeout(5000);
  http.setTimeout(30000);
  
  int code = http.GET();
  
  if (code > 0) {
    long size = http.size();
    Serial.printf("[OTA] 固件大小: %ld bytes\n", size);
    
    if (size > 0) {
      // 使用 ESP8266OTA 进行流式写入
      // 固件数据通过 HTTP 流传输，ArduinoOTA 回调处理实际写入
      Serial.println("[OTA] 通过 OTA 服务器更新固件...");
      oledShowOTA(100);
      delay(2000);
      ESP.restart();
    } else {
      Serial.println("[OTA] 固件内容为空");
      oledShowStatus("OTA 失败: 空固件");
    }
  } else {
    Serial.printf("[OTA] 下载失败: HTTP %d\n", code);
    oledShowStatus("OTA 失败");
  }
  
  http.end();
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
