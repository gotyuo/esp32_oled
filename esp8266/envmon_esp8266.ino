/*
 * EnvMon ESP8266 固件 - 主程序
 * 
 * 版本: 2.0.0
 * 目标: 接入 ICU 监护平台 http://192.168.68.119:12090
 * 
 * 功能:
 * - WiFi 连接 + 自动重连
 * - DHT11 温湿度采集 (ESP8266: GPIO3)
 * - OLED 状态显示 (I2C SDA=4, SCL=5 — 注意与 ESP32 不同!)
 * - HTTP POST 上报到 /api/ingest
 * - OTA 固件升级 (ArduinoOTA)
 * 
 * ESP8266 与 ESP32 的主要区别:
 *   - OLED 引脚: SDA=4, SCL=5 (ESP32 为 21/22)
 *   - 温湿度传感器: DHT11 (ESP32 为 DHT22)
 *   - 无麦克风、喇叭、心电监护支持
 *   - 无 BMP280 气压传感器
 *   - 内存有限 (约 80KB RAM)，需要优化
 * 
 * 通信协议:
 * POST http://192.168.68.119:12090/api/ingest
 * Authorization: Bearer ***
 * Content-Type: application/json
 * {"device_id":"esp8266-001","temp_c":36.5,"hum_pct":55.0}
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiUdp.h>
#include <ESP8266HTTPServer.h>
#include <ESP8266OTA.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>

#include "envmon_esp8266.h"

// ========== 全局变量 ==========
State currentState = STATE_INIT;
SensorData currentData = {0, 0, 0, 0, false};
unsigned long lastReportTime = 0;
unsigned long lastOledUpdate = 0;
unsigned long lastHeartbeat = 0;
bool alarmActive = false;
char alarmReason[128] = "";

// ESP8266 HTTP 服务器 (OTA + 状态页)
ESP8266HTTPServer server(80);
HTTPUpdateServer httpUpdate;

// ========== 启动信息 ==========
void printBanner() {
  Serial.println();
  Serial.println("=================================");
  Serial.println(" EnvMon ESP8266 Firmware v" FIRMWARE_VERSION);
  Serial.println(" Build: " FIRMWARE_BUILD);
  Serial.println("=================================");
  Serial.printf(" WiFi: %s\n", WIFI_SSID);
  Serial.printf(" Server: %s:%d\n", SERVER_HOST, SERVER_PORT);
  Serial.printf(" Device: %s\n", DEVICE_ID);
  Serial.println("=================================");
}

// ========== WiFi 连接 ==========
void connectWiFi() {
  currentState = STATE_WIFI_CONNECTING;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  Serial.print("连接 WiFi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    // 30 秒超时重启
    if (millis() - start > 30000) {
      Serial.println("\nWiFi 连接超时，重启...");
      oledShowStatus("WiFi 超时，重启...");
      delay(2000);
      ESP.restart();
    }
  }
  Serial.println("\nWiFi 已连接");
  Serial.printf(" IP: %s\n", WiFi.localIP().toString().c_str());
  
  currentState = STATE_WIFI_CONNECTED;
}

// ========== OTA 服务器初始化 ==========
void setupOTA() {
  if (!OTA_ENABLED) return;
  
  // 状态页面
  server.on("/", HTTP_GET, [](ESP8266HTTPServer& srv) {
    srv.send(200, "text/plain", "EnvMon ESP8266 v" FIRMWARE_VERSION "\n");
  });
  server.begin();
  
  // 使用 ArduinoOTA 进行 OTA 更新
  ArduinoOTA.setHostname(DEVICE_ID);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  
  ArduinoOTA.onStart([]() {
    Serial.println("[OTA] 升级开始");
    currentState = STATE_OTA;
    oledShowStatus("OTA 升级开始...");
  });
  
  ArduinoOTA.onEnd([]() {
    Serial.println("[OTA] 升级完成");
    currentState = STATE_WIFI_CONNECTED;
    oledShowStatus("OTA 完成，重启...");
  });
  
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    int pct = (int)((progress * 100) / total);
    oledShowOTA(pct);
  });
  
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] 错误: %d\n", (int)error);
    currentState = STATE_ERROR;
    oledShowStatus("OTA 失败");
  });
  
  ArduinoOTA.begin();
  
  Serial.println("[OTA] ESP8266 OTA 已启用");
  Serial.printf("[OTA] 端口: %d, 密码: %s\n", OTA_PORT, OTA_PASSWORD);
}

// ========== 报警检查 ==========
// 注意: ESP8266 无气压传感器，只检查温湿度
void checkAlarm(const SensorData& data) {
  if (!data.valid) {
    alarmActive = false;
    return;
  }
  
  bool alarm = false;
  char reason[128] = "";
  
  if (data.temp_c < TEMP_LOW || data.temp_c > TEMP_HIGH) {
    alarm = true;
    snprintf(reason, sizeof(reason), "温度 %.1fC 超出 [%.1f, %.1f]",
             data.temp_c, TEMP_LOW, TEMP_HIGH);
  } else if (data.hum_pct < HUM_LOW || data.hum_pct > HUM_HIGH) {
    alarm = true;
    snprintf(reason, sizeof(reason), "湿度 %.1f%% 超出 [%.1f, %.1f]",
             data.hum_pct, HUM_LOW, HUM_HIGH);
  }
  
  if (alarm != alarmActive) {
    alarmActive = alarm;
    if (alarm) {
      snprintf(alarmReason, sizeof(alarmReason), "%s", reason);
      Serial.printf("[ALARM] %s\n", alarmReason);
      currentState = STATE_ALARM;
    } else {
      Serial.println("[ALARM] 恢复");
      currentState = STATE_WIFI_CONNECTED;
    }
  }
}

// ========== 主循环 ==========
void loop() {
  // WiFi 断线重连
  if (WiFi.status() != WL_CONNECTED) {
    currentState = STATE_WIFI_CONNECTING;
    connectWiFi();
    return;
  }
  
  // 读取传感器
  sensorsRead(currentData);
  
  // 检查报警
  checkAlarm(currentData);
  
  // 更新 OLED 显示 (每秒)
  if (millis() - lastOledUpdate > OLED_UPDATE_MS) {
    lastOledUpdate = millis();
    if (alarmActive) {
      oledShowAlarm(alarmReason);
      // ESP8266 无喇叭，仅显示报警
    } else {
      oledUpdate(currentData);
    }
  }
  
  // 定期上报 (30 秒)
  if (millis() - lastReportTime > REPORT_INTERVAL) {
    lastReportTime = millis();
    currentState = STATE_REPORTING;
    
    bool ok = netReport(currentData);
    if (!ok) {
      // 上报失败，重试
      for (int i = 0; i < RETRY_COUNT; i++) {
        delay(RETRY_DELAY_MS);
        if (netReport(currentData)) {
          ok = true;
          break;
        }
      }
    }
    
    if (ok) {
      currentState = STATE_WIFI_CONNECTED;
    } else {
      currentState = STATE_ERROR;
      oledShowStatus("上报失败，重试中...");
    }
  }
  
  // 心跳 (30 秒)
  if (millis() - lastHeartbeat > HEARTBEAT_INTERVAL) {
    lastHeartbeat = millis();
    netHeartbeat();
  }
  
  delay(100);
}

// ========== Setup ==========
void setup() {
  Serial.begin(115200);
  delay(500);
  printBanner();
  
  // 打印内存使用情况 (ESP8266 内存有限，需要监控)
  Serial.printf(" Free heap: %u bytes\n", ESP.getFreeHeap());
  
  // 初始化子系统
  oledInit();
  sensorsCalibrate();
  sensorsInit();
  netInit();
  
  // WiFi 连接
  connectWiFi();
  
  // OTA 服务器
  setupOTA();
  
  // 注册设备
  currentState = STATE_INIT;
  if (!netRegister()) {
    Serial.println("设备注册失败，尝试继续上报...");
  }
  
  // 发送初始数据
  sensorsRead(currentData);
  netReport(currentData);
  
  currentState = STATE_WIFI_CONNECTED;
  lastReportTime = millis();
  lastHeartbeat = millis();
  
  Serial.printf(" Free heap after init: %u bytes\n", ESP.getFreeHeap());
}
