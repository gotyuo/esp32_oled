/*
 * EnvMon ESP32-S3 固件 - 主程序
 *
 * 版本: 8.0.0
 * 设备: ESP32-S3-N16R8
 * 目标: 接入 ICU 监护平台 http://172.22.22.83:12090
 *
 * 功能:
 * - WiFi 连接 + 自动重连
 * - DHT22 温湿度 + BMP280 气压采集
 * - OLED 状态显示 (I2C D8/D9)
 * - 喇叭报警 (I2S DAC MAX98357A, D38/D18/D39)
 * - HTTP POST 上报到 /api/ingest
 * - OTA 固件升级 (HTTPUpdateServer)
 * - 心电监护预留 (stub)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClient.h>
#include <HTTPUpdateServer.h>

#include "envmon_esp32.h"

// ========== WiFi 连接 (非阻塞) ==========
void connectWiFi();
void updateWiFi();

// ========== 全局状态实例 ==========
RuntimeState g_state;

WebServer server(OTA_PORT);
HTTPUpdateServer httpUpdateServer;

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

// ========== WiFi 连接 (非阻塞状态机) ==========
// 调用一次后, 后续在 loop() 里每圈调用 updateWiFi() 轮询。
// 连接期间 setup() 不会阻塞 —— 传感器/音频/OTA 都能初始化。
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.persistent(false);
  WiFi.setAutoConnect(true);
  WiFi.setAutoReconnect(true);

  g_state.wifiConnected      = false;
  g_state.wifiReconnectCount = 0;
  g_state.wifiAttemptStartMs = millis();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("启动 WiFi 连接 (非阻塞): %s\n", WIFI_SSID);
  oledShowWifiStatus(false, 0);
}

// 由 loop() 每圈调用: 轮询状态、刷新屏幕、处理看门狗超时、断线重连。
void updateWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!g_state.wifiConnected) {
      g_state.wifiConnected      = true;
      g_state.wifiReconnectCount = 0;
      Serial.printf("WiFi 已连接 IP: %s RSSI: %d dBm\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }
    oledShowWifiStatus(true, 0);
    return;
  }

  // 未连接: 让出 CPU 给 WiFi 栈, 屏幕照常刷新, 不阻塞任何初始化。
  delay(20);
  unsigned long elapsed = millis() - g_state.wifiAttemptStartMs;

  if (elapsed > WIFI_CONNECT_TIMEOUT_MS) {
    Serial.println("\nWiFi 连接超时，重启...");
    oledShowWifiStatus(false, g_state.wifiReconnectCount);
    delay(500);
    ESP.restart();
  }

  static unsigned long lastDotMs = 0;
  if (millis() - lastDotMs >= 1000) {
    lastDotMs = millis();
    Serial.print(".");
    g_state.wifiReconnectCount++;
    oledShowWifiStatus(false, g_state.wifiReconnectCount);
  }
}

// ========== OTA 服务器初始化 ==========
void setupOTA() {
  if (!OTA_ENABLED) return;

  server.on("/", HTTP_GET, []() {
    String response = String("EnvMon ESP32 v") + FIRMWARE_VERSION;
    response += "\nIP: " + WiFi.localIP().toString();
    response += "\nOTA: /update";
    server.send(200, "text/plain", response);
  });
  server.begin();

  httpUpdateServer.setup(&server, OTA_USERNAME, OTA_PASSWORD);
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
    if (lastState == HIGH) {
      pressStart = millis();
    }
    lastState = LOW;
  } else {
    if (lastState == LOW && millis() - pressStart > 200) {
      if (g_state.alarmState == AlarmState::ACTIVE) {
        g_state.alarmState = AlarmState::SNOOZED;
        g_state.alarmSnoozedAt = millis();
        audioMute();
        ELOG("Alarm snoozed by button");
      } else if (g_state.alarmState == AlarmState::SNOOZED) {
        g_state.alarmState = AlarmState::ACTIVE;
        audioUnmute();
        ELOG("Alarm un-snoozed by button");
      } else {
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
    g_state.alarmState = AlarmState::ACTIVE;
    g_state.alarmCauses = (AlarmCause)causes;
    g_state.alarmTriggeredAt = millis();
    ELOG("ALARM triggered: causes=0x%02X", causes);
    audioAlarm(true);
    oledShowAlarm((AlarmCause)causes, g_state.alarmState);
  } else {
    if (g_state.alarmState != AlarmState::NONE) {
      ELOG("ALARM cleared");
      g_state.alarmState = AlarmState::NONE;
      g_state.alarmCauses = AlarmCause::NONE;
      audioAlarm(false);
    }
  }
}

// ========== Setup ==========
void setup() {
  Serial.begin(115200);
  delay(500);
  printBanner();

  // 1) 先点亮 OLED —— 在任何阻塞操作之前
  oledInit();
  oledShowBoot();

  // 2) 启动 WiFi (非阻塞, 立即返回) —— 连接在后台进行
  connectWiFi();

  // 3) WiFi 连接期间不等待: 传感器/音频/ECG 立刻初始化
  //    (旧版在这里阻塞在 WiFi, 连不上时这些全都不初始化)
  calibrateSensors();
  if (!sensorsIsCalibrated()) {
    ELOG("传感器校准失败, 使用默认值");
  }

  audioInit();

  ekgInitialize();

  SensorReading initial = readSensors();
  if (initial.anyValid()) {
    g_state.lastReading = initial;
  }

  g_state.lastReportMs  = millis();
  g_state.lastDisplayMs = millis();

  // 4) WiFi 连上后 (loop() 里首次检测到) 再做 OTA 和平台注册
}

// ========== 主循环 ==========
void loop() {
  updateWiFi();   // 非阻塞轮询: 超时重启 / 刷新屏幕 / 检测断线

  // OTA 服务器和平台注册只在 WiFi 连上后初始化一次
  static bool networkReady = false;
  if (g_state.wifiConnected) {
    if (!networkReady) {
      networkReady = true;
      setupOTA();
      if (!platformRegister()) {
        ELOG("设备注册失败, 继续运行");
      }
    }
    server.handleClient();
  } else {
    return;   // WiFi 未连上, 只轮询连接, 不处理业务
  }

  processButton();

  SensorReading reading = readSensors();
  if (reading.anyValid()) {
    g_state.lastReading = reading;
    g_state.sensorReadCount++;
  } else {
    g_state.sensorFailCount++;
  }

  processAlarm();

  audioUpdate();

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

  if (millis() - g_state.lastReportMs > REPORT_INTERVAL_MS) {
    bool ok = platformReport(g_state.lastReading);
    if (!ok) {
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

  static unsigned long lastOtaCheck = 0;
  if (millis() - lastOtaCheck > 300000) {
    lastOtaCheck = millis();
    int progress = 0;
    if (platformCheckOta(progress)) {
      g_state.otaInProgress = true;
      g_state.otaProgressPct = progress;
      oledShowOtaProgress(progress);
    }
  }

  delay(100);
}
