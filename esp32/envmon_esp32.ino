/*
 * EnvMon ESP32 固件 - 主程序
 * 
 * 版本: 2.0.0
 * 目标: 接入 ICU 监护平台 http://192.168.68.119:12090
 * 
 * 功能:
 * - WiFi 连接 + 自动重连
 * - DHT22 温湿度 + BMP280 气压采集
 * - OLED 状态显示 (I2C 21/22)
 * - 麦克风采样 (I2S)
 * - 喇叭报警 (I2S DAC)
 * - HTTP POST 上报到 /api/ingest
 * - OTA 固件升级
 * - 心电监护预留 (GPIO34/35)
 * 
 * 通信协议:
 * POST http://192.168.68.119:12090/api/ingest
 * Authorization: Bearer <token>
 * Content-Type: application/json
 * {"device_id":"esp32-001","temp_c":36.5,"hum_pct":55.0,"pres_hpa":1013.0}
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <Wire.h>

#include "envmon_esp32.h"

// ========== 全局变量 ==========
State currentState = STATE_INIT;
SensorData currentData = {0, 0, 0, 0, 0, 0, false};
unsigned long lastReportTime = 0;
unsigned long lastOledUpdate = 0;
unsigned long lastHeartbeat = 0;
bool alarmActive = false;
char alarmReason[128] = "";

WiFiServer server(80);  // OTA 端口
ESP32HTTPUpdateServer httpUpdateServer;

// ========== 启动信息 ==========
void printBanner() {
  Serial.println();
  Serial.println("=================================");
  Serial.println(" EnvMon ESP32 Firmware v" FIRMWARE_VERSION);
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
  
  server.on(/, HTTP_GET, [](HTTPClient& client) {
    client.send(200, "text/plain", "EnvMon ESP32 v" FIRMWARE_VERSION "\n");
  });
  server.begin();
  
  httpUpdateServer.start(80, "admin", "admin123");
  httpUpdateServer.setHTTPAsyncServer(&server);
}

// ========== 主循环 ==========
void loop() {
  httpUpdateServer.loop();
  
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
      // 报警时播放声音
      audioAlarm(1);
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
  
  // OTA 检查
  otaCheck();
  
  delay(100);
}

// ========== 报警检查 ==========
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
  } else if (data.pres_hpa < PRES_LOW || data.pres_hpa > PRES_HIGH) {
    alarm = true;
    snprintf(reason, sizeof(reason), "气压 %.1f hPa 超出 [%.1f, %.1f]",
             data.pres_hpa, PRES_LOW, PRES_HIGH);
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

// ========== Setup ==========
void setup() {
  Serial.begin(115200);
  delay(500);
  printBanner();
  
  // 初始化子系统
  oledInit();
  sensorsCalibrate();
  sensorsInit();
  audioInit();
  netInit();
  ekgInit();
  
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
}
