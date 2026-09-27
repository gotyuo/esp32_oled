/*
 * EnvMon ESP8266 - OLED 显示驱动
 *
 * 使用 U8g2 库驱动 128x64 OLED 显示屏 (SSD1315, SPI)
 *
 * 引脚分配 (ESP8266 专用!):
 *   CS  = GPIO15
 *   DC  = GPIO5
 *   SCK = GPIO14
 *   MOSI = GPIO13
 *   RESET = GPIO16
 */

#include <Arduino.h>
#include <SPI.h>
#include <U8g2lib.h>

#include "envmon_esp8266.h"

// ========== OLED 实例 ==========
static U8G2_SSD1315_128X64_NONAME_F_4W_HW_SPI u8g2(U8G2_R0, OLED_CS_PIN, OLED_DC_PIN, OLED_RESET_PIN);

void oledInit() {
  Serial.printf("[OLED] init cs=%d dc=%d reset=%d\n", OLED_CS_PIN, OLED_DC_PIN, OLED_RESET_PIN);

  SPI.begin();
  if (!u8g2.begin()) {
    Serial.println("[OLED] 初始化失败!");
    return;
  }

  u8g2.setContrast(255);
  u8g2.setFont(u8g2_font_ncenB08_tr);

  u8g2.clearBuffer();
  u8g2.drawStr(0, 12, "EnvMon");
  u8g2.drawStr(0, 28, "v");
  u8g2.drawStr(0, 44, "OK");
  u8g2.sendBuffer();

  delay(1000);
  Serial.println("[OLED] 初始化成功");
}

static void clearScreen() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);
}

void oledUpdate(const SensorData& data) {
  clearScreen();

  // 标题
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.drawStr(0, 10, "EnvMon v");
  u8g2.drawStr(48, 10, FIRMWARE_VERSION);
  u8g2.drawHLine(0, 14, OLED_WIDTH);

  // 第一行: WiFi + 设备 ID
  u8g2.drawStr(0, 26, "WiFi: ");
  u8g2.drawStr(32, 26, netConnected ? "OK" : "NO");
  u8g2.drawStr(70, 26, "ID:");
  u8g2.drawStr(90, 26, DEVICE_ID);

  // 中间: SpO2 / HR (大字)
  u8g2.setFont(u8g2_font_ncenB18_tr);
  char tmp[8];
  if (data.vital_valid) {
    // 左: SpO2
    snprintf(tmp, sizeof(tmp), "%d", (int)data.spo2);
    u8g2.drawStr(0, 42, tmp);
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.drawStr(30, 42, "%SpO2");

    // 右: HR
    snprintf(tmp, sizeof(tmp), "%d", (int)data.heart_rate);
    u8g2.setFont(u8g2_font_ncenB18_tr);
    u8g2.drawStr(70, 42, tmp);
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.drawStr(100, 42, "bpm");
  } else {
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.drawStr(0, 42, "SpO2: --");
    u8g2.drawStr(50, 42, "HR: --");
  }

  // 底行: 温湿度 + 血压估算
  u8g2.setFont(u8g2_font_ncenB08_tr);
  snprintf(tmp, sizeof(tmp), "T=%.0fC H=%.0f%%", data.temp_c, data.hum_pct);
  u8g2.drawStr(0, 58, tmp);

  // 血压显示 (仅 valid 时显示)
  if (data.bp_systolic > 0 && data.bp_diastolic > 0) {
    snprintf(tmp, sizeof(tmp), "%.0f/%.0f", data.bp_systolic, data.bp_diastolic);
    u8g2.drawStr(80, 58, tmp);
  }

  u8g2.sendBuffer();
}

void oledShowAlarm(const char* msg) {
  clearScreen();

  u8g2.setFont(u8g2_font_ncenB18_tr);
  u8g2.drawStr(0, 12, "! ALARM");
  u8g2.drawHLine(0, 18, OLED_WIDTH);

  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.drawStr(0, 32, msg);

  u8g2.sendBuffer();
}

void oledShowOTA(int progress) {
  clearScreen();

  u8g2.setFont(u8g2_font_ncenB18_tr);
  u8g2.drawStr(0, 12, "OTA");
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.drawStr(0, 26, "Updating...");

  int barX = 5;
  int barY = 38;
  int barW = 118;
  int barH = 12;

  u8g2.drawFrame(barX, barY, barW, barH);
  int fillW = (progress * (barW - 2)) / 100;
  u8g2.drawBox(barX + 1, barY + 1, fillW, barH - 2);

  char pctStr[8];
  snprintf(pctStr, sizeof(pctStr), "%d%%", progress);
  u8g2.drawStr(54, barY + 16, pctStr);

  u8g2.sendBuffer();
}

void oledShowStatus(const char* msg) {
  clearScreen();

  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.drawStr(0, 10, "Status");
  u8g2.drawHLine(0, 14, OLED_WIDTH);

  u8g2.drawStr(0, 28, msg);

  u8g2.sendBuffer();
}
