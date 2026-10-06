/**
 * =============================================================================
 * EnvMon ESP32 - OLED 显示驱动 (oled_driver.cpp)
 * =============================================================================
 *
 * 硬件: SSD1306 128x64 I2C OLED
 * 引脚: SDA=GPIO8, SCL=GPIO9 (ESP32-S3)
 * 库  : Adafruit_SSD1306 (基于 Adafruit_GFX)
 *
 * 版本: v8.0.0
 * =============================================================================
 */

#include "envmon_esp32.h"
#include <WiFi.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// 屏幕几何常量 (必须在使用前定义)
#define OLED_WIDTH    128
#define OLED_HEIGHT   64

Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET_PIN);

// 行基线
#define LINE0_Y       10
#define LINE1_Y       26
#define LINE2_Y       42
#define LINE3_Y       56

#define OLED_MIN_REFRESH_MS 200UL

namespace {
  bool         s_inited       = false;
  uint32_t     s_lastRefreshMs = 0;
  uint8_t      s_lastView     = 0xFF;
  uint32_t     s_bootShownMs  = 0;
}

enum ViewId : uint8_t {
  VIEW_NONE = 0,
  VIEW_BOOT,
  VIEW_ENV,
  VIEW_ALARM,
  VIEW_OTA,
  VIEW_WIFI,
};

static bool shouldRefresh(uint8_t viewId) {
  if (viewId != s_lastView) return true;
  if (millis() - s_lastRefreshMs >= OLED_MIN_REFRESH_MS) return true;
  return false;
}

static void markRefreshed(uint8_t viewId) {
  s_lastView      = viewId;
  s_lastRefreshMs = millis();
}

static void drawStatusBar() {
  const char* wifiStr = g_state.wifiConnected ? "W:1" : "W:0";
  oled.setCursor(OLED_WIDTH - 24, 0);
  oled.print(wifiStr);
  oled.setCursor(OLED_WIDTH - 26, OLED_HEIGHT - 8);
  oled.print(F("v"));
  oled.print(FIRMWARE_VERSION);
}

static void printValidFloat(float value, bool valid) {
  if (!valid || isnan(value)) {
    oled.print("--");
  } else {
    oled.print(value, 1);
  }
}

bool oledInit() {
  if (s_inited) {
    ELOG("OLED already initialized, skip");
    return true;
  }

  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);

  bool ok = false;
  if (oled.begin(SSD1306_SWITCHCAPVCC, 0x3C, false, false)) {
    ok = true;
  } else {
    ELOG("OLED init failed @0x3C, trying 0x3D");
    if (oled.begin(SSD1306_SWITCHCAPVCC, 0x3D, false, false)) {
      ok = true;
    }
  }

  if (!ok) {
    ELOG("OLED init FAILED - display unavailable");
    ELOG("I2C scan:");
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
      Wire.beginTransmission(addr);
      if (Wire.endTransmission() == 0) {
        ELOG("  device @0x%02X", addr);
      }
    }
    s_inited = false;
    return false;
  }

  // 显式设置对比度/反向/显示使能 —— 部分模块 begin() 后对比度过低导致"看不见"
  oled.invertDisplay(false);
  oled.dim(false);
  // SSD1306 原始命令: 0x81=对比度设置, 0xFF=最高, 0xAF=显示开启
  oled.ssd1306_command(0x81);
  oled.ssd1306_command(0xFF);
  oled.ssd1306_command(0xAF);
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);

  // 全亮测试: 整屏填充一帧, 确认面板物理点亮
  oled.fillScreen(SSD1306_WHITE);
  oled.display();
  delay(300);
  oled.fillScreen(SSD1306_BLACK);
  oled.display();
  delay(200);

  s_inited = true;
  ELOG("OLED init OK (%dx%d, FONT5X7, contrast=255)", OLED_WIDTH, OLED_HEIGHT);
  return true;
}

void oledShowBoot() {
  if (!s_inited) return;

  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setCursor(8, 8);
  oled.print("EnvMon ESP32");
  oled.setCursor(8, LINE0_Y);
  oled.print(F("ID:"));
  oled.print(DEVICE_ID);
  oled.setCursor(8, LINE1_Y);
  oled.print(F("FW:"));
  oled.print(FIRMWARE_VERSION);
  oled.setCursor(8, LINE2_Y);
  oled.print(F("Connecting..."));
  oled.setCursor(8, LINE3_Y);
  oled.print(F("please wait"));

  drawStatusBar();
  oled.display();
  markRefreshed(VIEW_BOOT);
  s_bootShownMs = millis();
}

void oledShowEnvironment(const SensorReading& r) {
  if (!s_inited) return;
  if (!shouldRefresh(VIEW_ENV)) return;

  oled.clearDisplay();
  oled.setTextSize(1);

  oled.setCursor(0, LINE0_Y);
  oled.print(F("T:"));
  printValidFloat(r.temp_c, r.valid_temp);
  oled.print("C");

  oled.setCursor(0, LINE1_Y);
  oled.print(F("H:"));
  printValidFloat(r.hum_pct, r.valid_hum);
  oled.print(F("%"));

  oled.setCursor(0, LINE2_Y);
  oled.print(F("P:"));
  printValidFloat(r.pres_hpa, r.valid_pres);
  oled.print(F("hPa"));

  oled.setCursor(0, LINE3_Y);
  oled.print(F("N:"));
  printValidFloat(r.noise_db, r.valid_noise);
  oled.print(F("dB"));

  drawStatusBar();
  oled.display();
  markRefreshed(VIEW_ENV);
}

void oledShowAlarm(AlarmCause causes, AlarmState state) {
  if (!s_inited) return;
  if (!shouldRefresh(VIEW_ALARM)) return;

  oled.clearDisplay();
  oled.setTextSize(1);

  if (state == AlarmState::ACTIVE) {
    oled.setCursor(0, 0);
    oled.print(F("*** ALARM ***"));
  } else if (state == AlarmState::SNOOZED) {
    oled.setCursor(0, 0);
    oled.print(F("ALARM SNOOZED"));
  } else {
    oled.setCursor(0, 0);
    oled.print(F("ALARM CLEARED"));
  }

  String reason;
  auto c = (uint8_t)causes;
  if (c & (uint8_t)AlarmCause::TEMP_HIGH) { if (reason.length()) reason += F(","); reason += F("T-HI"); }
  if (c & (uint8_t)AlarmCause::TEMP_LOW)  { if (reason.length()) reason += F(","); reason += F("T-LO"); }
  if (c & (uint8_t)AlarmCause::HUM_HIGH)  { if (reason.length()) reason += F(","); reason += F("H-HI"); }
  if (c & (uint8_t)AlarmCause::HUM_LOW)   { if (reason.length()) reason += F(","); reason += F("H-LO"); }
  if (c & (uint8_t)AlarmCause::PRES_HIGH) { if (reason.length()) reason += F(","); reason += F("P-HI"); }
  if (c & (uint8_t)AlarmCause::PRES_LOW)  { if (reason.length()) reason += F(","); reason += F("P-LO"); }
  if (c & (uint8_t)AlarmCause::NOISE_HIGH){ if (reason.length()) reason += F(","); reason += F("N-HI"); }
  if (reason.length() == 0) reason = F("UNKNOWN");

  oled.setCursor(0, LINE0_Y);
  oled.print(F("Cause:"));
  oled.print(reason);

  oled.setCursor(0, LINE1_Y);
  oled.print(F("T:"));
  printValidFloat(g_state.lastReading.temp_c, g_state.lastReading.valid_temp);
  oled.print("C");

  oled.setCursor(0, LINE2_Y);
  oled.print(F("H:"));
  printValidFloat(g_state.lastReading.hum_pct, g_state.lastReading.valid_hum);
  oled.print(F("%"));

  oled.setCursor(0, LINE3_Y);
  oled.print(F("Press BTN to mute"));

  drawStatusBar();
  oled.display();
  markRefreshed(VIEW_ALARM);
}

void oledShowOtaProgress(int percent) {
  if (!s_inited) return;
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;

  oled.clearDisplay();
  oled.setTextSize(1);

  oled.setCursor(0, 0);
  oled.print(F("OTA UPGRADE"));

  const int barX = 4, barY = 24, barW = OLED_WIDTH - 8, barH = 12;
  oled.drawRect(barX, barY, barW, barH, SSD1306_WHITE);
  int fill = (int)((long)barW * percent / 100);
  if (fill > barW) fill = barW;
  oled.fillRect(barX + 1, barY + 1, fill, barH - 2, SSD1306_WHITE);

  oled.setCursor(0, LINE2_Y);
  oled.print(F("Progress:"));
  oled.print(percent);
  oled.print(F("%"));

  oled.setCursor(0, LINE3_Y);
  oled.print(F("DO NOT POWER OFF"));

  drawStatusBar();
  oled.display();
  markRefreshed(VIEW_OTA);
}

void oledShowWifiStatus(bool connected, int reconnectCount) {
  if (!s_inited) return;
  if (!shouldRefresh(VIEW_WIFI)) return;

  oled.clearDisplay();
  oled.setTextSize(1);

  oled.setCursor(0, 0);
  oled.print(F("WIFI STATUS"));

  oled.setCursor(0, LINE0_Y);
  if (connected) {
    oled.print(F("Connected"));
    oled.setCursor(0, LINE1_Y);
    oled.print(WIFI_SSID);
    oled.setCursor(0, LINE2_Y);
    oled.print(F("RSSI:"));
    oled.print(WiFi.RSSI());
    oled.print(F("dBm"));
  } else {
    oled.print(F("Disconnected"));
    oled.setCursor(0, LINE1_Y);
    oled.print(F("Retries:"));
    oled.print(reconnectCount);
    oled.setCursor(0, LINE2_Y);
    oled.print(F("SSID:"));
    oled.print(WIFI_SSID);
    oled.setCursor(0, LINE3_Y);
    oled.print(F("Attempting..."));
  }

  drawStatusBar();
  oled.display();
  markRefreshed(VIEW_WIFI);
}
