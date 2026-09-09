/*
 * OLED 显示驱动 (ESP8266)
 * 
 * 引脚: SDA=GPIO4, SCL=GPIO5 (注意与 ESP32 不同!)
 * 库: Adafruit_SSD1306, Adafruit_GFX
 * 显示: 128x64 OLED
 */

#include "Arduino.h"
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include "envmon_esp8266.h"

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET_PIN);

void oledInit() {
  Wire.begin(OLED_SDA, OLED_SCL);
  
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("[OLED] 初始化失败");
    return;
  }
  
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("EnvMon ESP8266");
  display.printf("v%s\n", FIRMWARE_VERSION);
  display.printf("ID: %s\n", DEVICE_ID);
  display.println("正在初始化...");
  display.display();
  
  Serial.println("[OLED] 初始化成功");
}

void oledUpdate(const SensorData& data) {
  if (!data.valid) {
    oledShowStatus("传感器故障");
    return;
  }
  
  display.clearDisplay();
  
  // 标题栏
  display.setTextSize(1);
  display.fillRect(0, 0, 128, 10, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(2, 1);
  display.print("EnvMon " FIRMWARE_VERSION);
  display.setTextColor(SSD1306_WHITE);
  
  // 设备 ID
  display.setTextSize(1);
  display.setCursor(0, 12);
  display.printf("ID: %s\n", DEVICE_ID);
  
  // 分隔线
  display.drawLine(0, 22, 128, 22, SSD1306_WHITE);
  
  // 温湿度 (ESP8266 没有气压)
  display.setTextSize(2);
  display.setCursor(0, 26);
  display.printf("%.1fC\n", data.temp_c);
  
  display.setTextSize(2);
  display.setCursor(60, 26);
  display.printf("%.1f%%\n", data.hum_pct);
  
  // 分隔线
  display.drawLine(0, 50, 128, 50, SSD1306_WHITE);
  
  // 状态
  display.setTextSize(1);
  display.setCursor(0, 52);
  display.print("WiFi: ");
  if (WiFi.status() == WL_CONNECTED) {
    display.println("OK");
  } else {
    display.println("Lost");
  }
  
  // 时间戳
  display.printf("Time: %lu ms\n", data.timestamp_ms);
  
  display.display();
}

void oledShowAlarm(const char* msg) {
  display.clearDisplay();
  display.setTextSize(2);
  
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("!!! ALARM !!!");
  
  display.setTextSize(1);
  display.setCursor(0, 18);
  
  // 简单文本换行
  char line[17];
  int lineNum = 0;
  for (int i = 0; msg[i] && lineNum < 4; i++) {
    strncpy(line, msg + i, 16);
    line[16] = '\0';
    display.setCursor(0, 18 + lineNum * 10);
    display.println(line);
    lineNum++;
    i += 15;
  }
  
  display.display();
}

void oledShowOTA(int progress) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("OTA Update...");
  
  // 进度条
  display.drawRect(0, 20, 128, 15, SSD1306_WHITE);
  display.fillRect(1, 21, (126 * progress) / 100, 13, SSD1306_WHITE);
  
  display.setCursor(0, 40);
  display.printf("%d%%\n", progress);
  
  display.display();
}

void oledShowStatus(const char* msg) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.printf("Status:\n%s\n", msg);
  display.display();
}
