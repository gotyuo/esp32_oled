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
#include <WiFiServer.h>
#include <WiFiClient.h>
#include <ESP32HTTPUpdateServer.h>

#include "envmon_esp32.h"

// ========== 全局状态实例 ==========
RuntimeState g_state;

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
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.persistent(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("连接 WiFi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    g_state.wifiReconnectCount++;
    oledShowWifiStatus(false, g_state.wifiReconnectCount);
    // 30 秒超时重启
    if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) {
      Serial.println("\nWiFi 连接超时，重启...");
      delay(2000);
      ESP.restart();
    }
  }
  Serial.println("\nWiFi 已连接");
  Serial.printf(" IP: %s\n", WiFi.localIP().toString().c_str());
  g_state.wifiConnected = true;
  g_state.wifiReconnectCount = 0;
}

// ========== OTA 服务器初始化 ==========
void setupOTA() {
  if (!OTA_ENABLED) return;

  server.on("/", HTTP_GET, []() {
    String response = "EnvMon ESP32 v" FIRMWARE_VERSION
      "\nIP: " + WiFi.localIP().toString()
      "\nOTA: /update";
    server.send(200, "text/plain", response);
  });
  server.begin();

  httpUpdateServer.start(80, "admin", "admin123");
  httpUpdateServer.setHTTPAsyncServer(&server);
  Serial.println("[OTA] 服务器已启动，访问 http://<ip>/update");
}

// ========== 报警判断 ==========
uint8_t computeAlarmCauses(const SensorReading& r) {
  uint8_t causes = 0;
  if (r.valid_temp) {
    if (r.temp_c > TEMP_MAX_C)  causes |= (uint8_t)AlarmCause::TEMP_HIGH;
    if (r.temp_c < TEMP_MIN_C)  causes |= (uint8_t)AlarmCause::TEMP_LOW;
  }
  if (r.valid_hum) {
    if (r.hum_pct > HUM_MAX_PCT)  causes |= (uint8_t)AlarmCause::HUM_HIGH;
    if (r.hum_pct < HUM_MIN_PCT)  causes |= (uint8_t)AlarmCause::HUM_LOW;
  }
  if (r.valid_pres) {
    if (r.pres_hpa > PRES_MAX_HPA)  causes |= (uint8_t)AlarmCause::PRES_HIGH;
    if (r.pres_hpa < PRES_MIN_HPA)  causes |= (uint8_t)AlarmCause::PRES_LOW;
  }
  if (r.valid_noise && r.noise_db > NOISE_MAX_DB) {
    causes |= (uint8_t)AlarmCause::NOISE_HIGH;
  }
  return causes;
}

bool isAlarmDue(const SensorReading& r) {
  uint8_t causes = computeAlarmCauses(r);
  if (causes == 0) return false;
  // 冷却检查: 触发后 N 秒内不重复触发
  if (g_state.alarmState == AlarmState::ACTIVE &&
      millis() - g_state.alarmTriggeredAt < ALARM_COOLDOWN_MS) {
    return false;
  }
  return true;
}

// ========== 按钮处理 ==========
void processButton() {
  static uint32_t lastPress = 0;
  static bool lastState = HIGH;
  static uint32_t pressStart = 0;

  if (!digitalRead(BTN_PIN)) {
    // 按钮按下
    if (lastState == HIGH) {
      pressStart = millis();
    }
    lastState = LOW;
  } else {
    // 按钮释放
    if (lastState == LOW && millis() - pressStart > 200) {
      // 确认有效按下 (>200ms 防抖)
      if (g_state.alarmState == AlarmState::ACTIVE) {
        // 静音报警
        g_state.alarmState = AlarmState::SNOOZED;
        g_state.alarmSnoozedAt = millis();
        audioMute();
        ELOG("Alarm snoozed by button");
      } else if (g_state.alarmState == AlarmState::SNOOZED) {
        // 恢复报警
        g_state.alarmState = AlarmState::ACTIVE;
        audioUnmute();
        ELOG("Alarm un-snoozed by button");
      } else {
        // 测试音
        audioPlayTestTone();
      }
      lastPress = millis();
    }
    lastState = HIGH;
  }
}

// ========== 报警处理 ==========
void processAlarm() {
  const SensorReading& r = g_state.lastReading;
  uint8_t causes = computeAlarmCauses(r);

  if (isAlarmDue(r)) {
    // 触发报警
    g_state.alarmState = AlarmState::ACTIVE;
    g_state.alarmCauses = (AlarmCause)causes;
    g_state.alarmTriggeredAt = millis();
    ELOG("ALARM triggered: causes=0x%02X", causes);
    audioAlarm(true);
    oledShowAlarm((AlarmCause)causes, g_state.alarmState);
  } else {
    // 恢复
    if (g_state.alarmState != AlarmState::NONE) {
      ELOG("ALARM cleared");
      g_state.alarmState = AlarmState::NONE;
      g_state.alarmCauses = AlarmCause::NONE;
      audioAlarm(false);
    }
  }
}

// ========== 主循环 ==========
void loop() {
  httpUpdateServer.loop();

  // WiFi 断线重连
  if (!g_state.wifiConnected || WiFi.status() != WL_CONNECTED) {
    g_state.wifiConnected = false;
    connectWiFi();
    return;
  }

  // 处理按钮
  processButton();

  // 读取传感器
  SensorReading reading = readSensors();
  if (reading.anyValid()) {
    g_state.lastReading = reading;
    g_state.sensorReadCount++;
  } else {
    g_state.sensorFailCount++;
  }

  // 处理报警
  processAlarm();

  // 更新音频 (报警音相位)
  audioUpdate();

  // 更新 OLED 显示 (每秒)
  if (millis() - g_state.lastDisplayMs > DISPLAY_INTERVAL_MS) {
    g_state.lastDisplayMs = millis();
    if (g_state.otaInProgress) {
      oledShowOtaProgress(g_state.otaProgressPct);
    } else if (g_state.alarmState != AlarmState::NONE) {
      oledShowAlarm(g_state.alarmCauses, g_state.alarmState);
    } else {
      oledShowEnvironment(g_state.lastReading);
    }
  }

  // 定期上报 (10 秒)
  if (millis() - g_state.lastReportMs > REPORT_INTERVAL_MS) {
    bool ok = platformReport(g_state.lastReading);
    if (!ok) {
      // 上报失败，重试 3 次
      for (int i = 0; i < 3; i++) {
        delay(2000);
        if (platformReport(g_state.lastReading)) {
          ok = true;
          break;
        }
      }
    }
    if (!ok) {
      oledShowWifiStatus(false, g_state.wifiReconnectCount);
    }
  }

  // OTA 检查 (每 5 分钟)
  static unsigned long lastOtaCheck = 0;
  if (millis() - lastOtaCheck > 300000) {
    lastOtaCheck = millis();
    int progress = 0;
    if (platformCheckOta(progress)) {
      g_state.otaInProgress = true;
      g_state.otaProgressPct = progress;
      // OTA 下载由 esp_http_ota 处理
      // 这里简化处理
      oledShowOtaProgress(progress);
    }
  }

  delay(100);
}

// ========== Setup ==========
void setup() {
  Serial.begin(115200);
  delay(500);
  printBanner();

  // 初始化 OLED
  oledInit();
  oledShowBoot();

  // 初始化传感器
  calibrateSensors();  // 校准传感器 (零点偏置)
  if (!sensorsIsCalibrated()) {
    ELOG("传感器校准失败, 使用默认值");
  }

  // 初始化音频
  audioInit();

  // 初始化心电监护
  ekgInitialize();

  // WiFi 连接
  oledShowWifiStatus(false, 0);
  connectWiFi();

  // OTA 服务器
  setupOTA();

  // 注册设备
  if (!platformRegister()) {
    ELOG("设备注册失败, 继续运行");
  }

  // 发送初始数据
  SensorReading initial = readSensors();
  if (initial.anyValid()) {
    g_state.lastReading = initial;
    platformReport(initial);
  }

  g_state.lastReportMs = millis();
  g_state.lastDisplayMs = millis();
}
