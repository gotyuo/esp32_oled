/**
 * =============================================================================
 * EnvMon ESP32 - OLED 显示驱动 (oled_driver.cpp)
 * =============================================================================
 *
 * 硬件: SSD1306 128x64 I2C OLED
 * 引脚: SDA=GPIO21, SCL=GPIO22 (复用 I2C 总线)
 * 库  : Adafruit_SSD1306 (基于 Adafruit_GFX)
 * 字体: FONT5X7 (Adafruit_GFX 内置最小字体)
 *
 * 提供:
 *   - oledInit()            初始化 I2C + OLED
 *   - oledShowBoot()        启动画面
 *   - oledShowEnvironment() 温湿度/气压/噪声读数
 *   - oledShowAlarm()       报警信息
 *   - oledShowOtaProgress() OTA 进度条
 *   - oledShowWifiStatus()  WiFi 连接状态
 *
 * 状态管理: 本模块维护自身初始化标志与"最后显示的内容类别",
 *           以便主循环按需切换而不重复刷屏。
 *
 * 版本: v2.0.0
 * =============================================================================
 */

#include "envmon_esp32.h"

#if ENVMON_LOG_ENABLE
#include <Adafruit_SSD1306.h>
#endif

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// 屏幕几何常量 (SSD1306 128x64 模组)
#define OLED_WIDTH    128
#define OLED_HEIGHT   64

// 行基线 (FONT5X7 行高约 8px, 取 12 间距)
#define LINE0_Y       10
#define LINE1_Y       26
#define LINE2_Y       42
#define LINE3_Y       56

// 屏幕刷新最小间隔 (毫秒), 防止 I2C 总线被刷屏拖慢
#define OLED_MIN_REFRESH_MS 200UL

// =============================================================================
// 模块内部状态 (不暴露给其他文件)
// =============================================================================
namespace {
  bool         s_inited       = false;   // 初始化成功标志
  uint32_t     s_lastRefreshMs = 0;      // 上次刷新时刻
  uint8_t      s_lastView     = 0xFF;    // 上次显示的内容类别 (见 ViewId)
  uint32_t     s_bootShownMs  = 0;       // 启动画面展示时长
}

enum ViewId : uint8_t {
  VIEW_NONE = 0,
  VIEW_BOOT,
  VIEW_ENV,
  VIEW_ALARM,
  VIEW_OTA,
  VIEW_WIFI,
};

// =============================================================================
// 内部工具: 带刷新节流的显示调度
// =============================================================================

/**
 * @brief 判断是否到达刷新时刻 (节流, 避免高频刷屏拖慢 I2C 总线)
 */
static bool shouldRefresh(uint8_t viewId) {
  // 内容类别变化时立即刷新, 不受节流限制
  if (viewId != s_lastView) return true;
  if (millis() - s_lastRefreshMs >= OLED_MIN_REFRESH_MS) return true;
  return false;
}

static void markRefreshed(uint8_t viewId) {
  s_lastView      = viewId;
  s_lastRefreshMs = millis();
}

/**
 * @brief 在底部绘制统一的状态条: WiFi 图标 + 设备 ID
 */
static void drawStatusBar() {
  // 右上角: WiFi 状态指示 (简单用 "W:1" / "W:0" 文本)
  const char* wifiStr = g_state.wifiConnected ? "W:1" : "W:0";
  oled.setCursor(OLED_WIDTH - 24, 0);
  oled.print(wifiStr);

  // 右下角: 固件版本 (便于现场核对)
  oled.setCursor(OLED_WIDTH - 26, OLED_HEIGHT - 8);
  oled.print(F("v"));
  oled.print(FIRMWARE_VERSION);
}

/**
 * @brief 将 NaN / 无效值显示为 "--" 占位, 避免刷屏乱码
 */
static void printValidFloat(float value, bool valid) {
  if (!valid || isnan(value)) {
    oled.print("--");
  } else {
    oled.print(value, 1);  // 保留 1 位小数, 屏幕空间有限
  }
}

// =============================================================================
// 对外接口
// =============================================================================

/**
 * @brief 初始化 OLED 显示
 *
 * @return true  初始化成功; false 初始化失败 (I2C 扫描不到设备或地址无效)
 *
 * 失败处理: 初始化失败不阻塞主程序, 后续调用会在未初始化时安全返回。
 *           串口日志会打印失败原因, 便于现场排查 (接线/供电/地址冲突)。
 */
bool oledInit() {
  if (s_inited) {
    ELOG("OLED already initialized, skip");
    return true;
  }

  // 配置 I2C 引脚 (Adafruit_SSD1306 的 begin 内部调用 Wire.begin)
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);

  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C, false, false, OLED_RESET_PIN)) {
    // 地址 0x3C 失败后尝试 0x3D (部分模组使用)
    ELOG("OLED init failed @0x3C, trying 0x3D");
    if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3D, false, false, OLED_RESET_PIN)) {
      ELOG("OLED init FAILED - display unavailable");
      // 诊断: 扫描 I2C 总线, 打印发现的设备
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
  }

  // 基础显示设置
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setClip(0, 0, OLED_WIDTH, OLED_HEIGHT);  // 防止字体渲染越界
  oled.display();

  s_inited = true;
  ELOG("OLED init OK (%dx%d, FONT5X7)", OLED_WIDTH, OLED_HEIGHT);
  return true;
}

/**
 * @brief 显示启动画面
 *
 * 内容: 品牌标题、设备 ID、固件版本、提示语。
 * 用于上电自检, 让用户确认设备已正常启动。
 */
void oledShowBoot() {
  if (!s_inited) {
    ELOG("oledShowBoot: OLED not initialized");
    return;
  }

  oled.clearDisplay();
  oled.setTextSize(1);

  // 标题
  oled.setCursor(8, 8);
  oled.print("EnvMon ESP32");

  // 设备 ID
  oled.setCursor(8, LINE0_Y);
  oled.print(F("ID:"));
  oled.print(DEVICE_ID);

  // 固件版本
  oled.setCursor(8, LINE1_Y);
  oled.print(F("FW:"));
  oled.print(FIRMWARE_VERSION);

  // 提示语
  oled.setCursor(8, LINE2_Y);
  oled.print(F("Connecting..."));

  // 进度提示
  oled.setCursor(8, LINE3_Y);
  oled.print(F("please wait"));

  drawStatusBar();
  oled.display();
  markRefreshed(VIEW_BOOT);
  s_bootShownMs = millis();
}

/**
 * @brief 显示环境读数 (主界面)
 *
 * @param r  当前采样结果
 *
 * 布局:
 *   行0: 温度  T:25.3C
 *   行1: 湿度  H:55.0%
 *   行2: 气压  P:1013.2hPa
 *   行3: 噪声  N:42dB
 *
 * 无效传感器以 "--" 占位, 不会阻塞显示。
 */
void oledShowEnvironment(const SensorReading& r) {
  if (!s_inited) {
    ELOG("oledShowEnvironment: OLED not initialized");
    return;
  }
  if (!shouldRefresh(VIEW_ENV)) return;

  oled.clearDisplay();
  oled.setTextSize(1);

  // 行0: 温度
  oled.setCursor(0, LINE0_Y);
  oled.print(F("T:"));
  printValidFloat(r.temp_c, r.valid_temp);
  oled.print("C");

  // 行1: 湿度
  oled.setCursor(0, LINE1_Y);
  oled.print(F("H:"));
  printValidFloat(r.hum_pct, r.valid_hum);
  oled.print(F("%"));

  // 行2: 气压
  oled.setCursor(0, LINE2_Y);
  oled.print(F("P:"));
  printValidFloat(r.pres_hpa, r.valid_pres);
  oled.print(F("hPa"));

  // 行3: 噪声
  oled.setCursor(0, LINE3_Y);
  oled.print(F("N:"));
  printValidFloat(r.noise_db, r.valid_noise);
  oled.print(F("dB"));

  drawStatusBar();
  oled.display();
  markRefreshed(VIEW_ENV);
}

/**
 * @brief 显示报警信息
 *
 * @param causes  报警原因位掩码
 * @param state   报警状态 (ACTIVE / SNOOZED)
 *
 * 报警界面采用高对比度文字, 醒目提示用户处理。
 * 多原因时以逗号分隔列出。
 */
void oledShowAlarm(AlarmCause causes, AlarmState state) {
  if (!s_inited) {
    ELOG("oledShowAlarm: OLED not initialized");
    return;
  }
  if (!shouldRefresh(VIEW_ALARM)) return;

  oled.clearDisplay();
  oled.setTextSize(1);

  // 报警标题 (醒目)
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

  // 拼接原因文本
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

  // 当前读数 (报警上下文)
  oled.setCursor(0, LINE1_Y);
  oled.print(F("T:"));
  printValidFloat(g_state.lastReading.temp_c, g_state.lastReading.valid_temp);
  oled.print("C");

  oled.setCursor(0, LINE2_Y);
  oled.print(F("H:"));
  printValidFloat(g_state.lastReading.hum_pct, g_state.lastReading.valid_hum);
  oled.print(F("%"));

  // 提示
  oled.setCursor(0, LINE3_Y);
  oled.print(F("Press BTN to mute"));

  drawStatusBar();
  oled.display();
  markRefreshed(VIEW_ALARM);
}

/**
 * @brief 显示 OTA 升级进度
 *
 * @param percent  进度百分比 (0-100)
 *
 * 绘制进度条 + 百分比文本, 让用户确认升级未中断。
 * 负值或超界会被钳制到 0-100。
 */
void oledShowOtaProgress(int percent) {
  if (!s_inited) {
    ELOG("oledShowOtaProgress: OLED not initialized");
    return;
  }
  // OTA 进度需高频刷新, 跳过节流
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;

  oled.clearDisplay();
  oled.setTextSize(1);

  oled.setCursor(0, 0);
  oled.print(F("OTA UPGRADE"));

  // 进度条框
  const int barX = 4, barY = 24, barW = OLED_WIDTH - 8, barH = 12;
  oled.drawRect(barX, barY, barW, barH);
  // 进度条填充
  int fill = (int)((long)barW * percent / 100);
  if (fill > barW) fill = barW;
  oled.fillRect(barX + 1, barY + 1, fill, barH - 2);

  // 百分比文本
  oled.setCursor(0, LINE2_Y);
  oled.print(F("Progress:"));
  oled.print(percent);
  oled.print(F("%"));

  // 提示
  oled.setCursor(0, LINE3_Y);
  oled.print(F("DO NOT POWER OFF"));

  drawStatusBar();
  oled.display();
  markRefreshed(VIEW_OTA);
}

/**
 * @brief 显示 WiFi 连接状态
 *
 * @param connected        是否已连接
 * @param reconnectCount   重连次数
 *
 * 用于网络异常时诊断; 主循环在 WiFi 断开时调用。
 */
void oledShowWifiStatus(bool connected, int reconnectCount) {
  if (!s_inited) {
    ELOG("oledShowWifiStatus: OLED not initialized");
    return;
  }
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
