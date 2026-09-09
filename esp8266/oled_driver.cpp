/*
 * EnvMon ESP8266 - OLED 显示驱动
 * 
 * 使用 Adafruit SSD1306 库驱动 SSD1306 OLED 显示屏 (128x64 I2C)
 * 
 * 引脚分配 (ESP8266 专用!):
 *   SDA = GPIO4  (注意: ESP32 为 GPIO21)
 *   SCL = GPIO5  (注意: ESP32 为 GPIO22)
 * 
 * 显示内容:
 *   - 温湿度实时数据
 *   - WiFi 状态
 *   - 设备状态
 *   - 报警信息
 *   - OTA 进度条
 */

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>

#include "envmon_esp8266.h"

// ========== OLED 实例 ==========
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET_PIN);

// ========== 初始化 ==========
void oledInit() {
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);  // 400kHz I2C
  
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    // 尝试备用地址 0x3D
    Wire.end();
    Wire.begin(OLED_SDA, OLED_SCL);
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3D)) {
      Serial.println("[OLED] 初始化失败!");
      return;
    }
  }
  
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  
  // 显示启动信息
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("EnvMon ESP8266");
  display.println("v" FIRMWARE_VERSION);
  display.setCursor(0, 32);
  display.println("初始化完成");
  display.display();
  
  delay(1000);
  Serial.println("[OLED] 初始化成功");
}

// ========== 清除屏幕 ==========
static void clearScreen() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
}

// ========== 主显示: 传感器数据 ==========
void oledUpdate(const SensorData& data) {
  clearScreen();
  
  // 标题栏
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(F("EnvMon "));
  display.print(F("v"));
  display.println(FIRMWARE_VERSION);
  display.drawLine(0, 9, OLED_WIDTH, 9, SSD1306_WHITE);
  
  // 状态指示
  display.setCursor(0, 12);
  display.print(F("WiFi: "));
  if (WiFi.status() == WL_CONNECTED) {
    display.print(F("OK"));
  } else {
    display.print(F("断开"));
  }
  
  // 温度
  display.setCursor(0, 26);
  display.setTextSize(2);
  display.print(data.temp_c);
  display.setTextSize(1);
  display.print(F(" C"));
  
  // 湿度
  display.setCursor(0, 42);
  display.setTextSize(2);
  display.print(data.hum_pct);
  display.setTextSize(1);
  display.print(F(" %"));
  
  // 状态指示 (右下角)
  display.setCursor(80, 12);
  display.print(F("ID: "));
  display.print(DEVICE_ID);
  
  // 时间戳
  display.setCursor(0, 54);
  display.print(F("T="));
  display.println(data.timestamp_ms);
  
  display.display();
}

// ========== 报警显示 ==========
void oledShowAlarm(const char* msg) {
  clearScreen();
  
  // 报警标题
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.println(F("! 报警 !"));
  display.setTextSize(1);
  display.drawLine(0, 16, OLED_WIDTH, 16, SSD1306_WHITE);
  
  // 报警原因 (可能需要换行)
  display.setTextSize(1);
  display.setCursor(0, 22);
  
  // 限制报警消息长度以适应 OLED 宽度
  char truncated[32];
  strncpy(truncated, msg, sizeof(truncated) - 1);
  truncated[sizeof(truncated) - 1] = '\0';
  
  // 简单换行处理
  char line[17];
  int pos = 0;
  int y = 22;
  
  for (int i = 0; i < 16 && truncated[i]; i++) {
    line[i] = truncated[i];
    pos++;
  }
  line[pos] = '\0';
  display.setCursor(0, y);
  display.println(line);
  
  y += 10;
  pos = 0;
  for (int i = 16; i < 32 && truncated[i]; i++) {
    line[pos] = truncated[i];
    pos++;
  }
  line[pos] = '\0';
  if (pos > 0) {
    display.setCursor(0, y);
    display.println(line);
  }
  
  // 底部闪烁提示
  if (millis() % 2000 < 1000) {
    display.setTextSize(1);
    display.setCursor(0, 54);
    display.println(F("请检查传感器!"));
  }
  
  display.display();
}

// ========== OTA 进度显示 ==========
void oledShowOTA(int progress) {
  clearScreen();
  
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.println(F("OTA"));
  display.setTextSize(1);
  display.println(F("固件升级中..."));
  display.println();
  
  // 进度条
  int barX = 5;
  int barY = 36;
  int barW = 118;
  int barH = 12;
  
  // 边框
  display.drawRect(barX, barY, barW, barH, SSD1306_WHITE);
  
  // 填充
  int fillW = (progress * (barW - 2)) / 100;
  display.fillRect(barX + 1, barY + 1, fillW, barH - 2, SSD1306_WHITE);
  
  // 百分比
  char pctStr[8];
  snprintf(pctStr, sizeof(pctStr), "%d%%", progress);
  display.setCursor(54, barY + 14);
  display.println(pctStr);
  
  display.display();
}

// ========== 状态消息显示 ==========
void oledShowStatus(const char* msg) {
  clearScreen();
  
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(F("状态"));
  display.drawLine(0, 9, OLED_WIDTH, 9, SSD1306_WHITE);
  
  display.setTextSize(1);
  
  // 简单换行处理状态消息
  char truncated[32];
  strncpy(truncated, msg, sizeof(truncated) - 1);
  truncated[sizeof(truncated) - 1] = '\0';
  
  char line[17];
  int y = 14;
  
  for (int start = 0; start < 32; start += 16) {
    int pos = 0;
    for (int i = start; i < start + 16 && i < 32 && truncated[i]; i++) {
      line[pos] = truncated[i];
      pos++;
    }
    line[pos] = '\0';
    if (pos > 0) {
      display.setCursor(0, y);
      display.println(line);
      y += 12;
    }
  }
  
  display.display();
}
