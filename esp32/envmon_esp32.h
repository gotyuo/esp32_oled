/**
 * =============================================================================
 * EnvMon ESP32 - 配置头文件 (envmon_esp32.h)
 * =============================================================================
 *
 * 设备: ESP32-S3-N16R8 (环境/生理监测终端)
 * 功能: 温湿度、气压、音频采集 + OLED 显示 + HTTP 上报 + OTA
 *
 * 此文件集中定义:
 *   - 引脚分配 (Pin Map) — 使用 D# 丝印标签
 *   - WiFi / 平台服务器 / 鉴权配置
 *   - 传感器阈值与报警参数
 *   - 共享数据结构 (SensorReading 等)
 *   - 全局状态管理
 *   - 跨文件可见的函数原型 (各驱动 .cpp 实现)
 *
 * 版本: v8.0.0  |  日期: 2026-09-28
 * =============================================================================
 */

#ifndef ENVMON_ESP32_H
#define ENVMON_ESP32_H

// =============================================================================
// 基础包含
// =============================================================================
#include <Arduino.h>

// 编译期日志开关
#ifndef ENVMON_LOG_ENABLE
#define ENVMON_LOG_ENABLE 1
#endif

#if ENVMON_LOG_ENABLE
  #define ELOG(fmt, ...)  Serial.printf("[%s] " fmt "\n", TAG, ##__VA_ARGS__)
#else
  #define ELOG(fmt, ...)  ((void)0)
#endif
#define TAG "ENVMON"

// =============================================================================
// 设备身份配置
// =============================================================================

#define DEVICE_ID       "esp32-001"
#define FIRMWARE_VERSION "8.1.3"
#define FIRMWARE_BUILD   "2026-10-06"

// WiFi 凭据
#define WIFI_SSID       "JDCwifi_6010"
#define WIFI_PASSWORD   "321654987"

// 平台服务器
#define SERVER_HOST     "172.22.22.83"
#define SERVER_PORT     12090

// 兼容旧代码引用
#define MQTT_SERVER     SERVER_HOST
#define MQTT_PORT       SERVER_PORT

// 平台登录退避间隔 (毫秒)。
// 登录失败后至少等这么久才重试。否则每 10s 上报一次 -> 每次都打
// /api/login -> 服务端 429 限流, 缓存被填满后持续丢数据。
#define LOGIN_BACKOFF_MS  60000UL

// 鉴权 Token (留空, 由 platformLogin() 自动获取)
#define AUTH_TOKEN      ""

// 登录凭据 (用于自动获取 token)
#define LOGIN_USERNAME  "admin"
#define LOGIN_PASSWORD  "gotyuo987"

// =============================================================================
// 引脚分配 (Pin Map) — ESP32-S3-N16R8
// 使用 D# 丝印标签: D1=GPIO1, D4=GPIO4, D8=GPIO8, ...
// =============================================================================

// --- OLED (SSD1306, I2C) ---
#define OLED_SDA_PIN    8     // D8  - I2C SDA (与 BMP280 共用)
#define OLED_SCL_PIN    9     // D9  - I2C SCL
#define OLED_RESET_PIN  -1

// --- DHT22 温湿度传感器 ---
#define TEMP_PIN        4     // D4  - DHT22 数据脚

// --- BMP280 气压传感器 (I2C, 与 OLED 共用) ---
#define BMP280_SDA      OLED_SDA_PIN
#define BMP280_SCL      OLED_SCL_PIN
#define BMP280_ADDRESS  0x77

// --- I2S 麦克风 (采集) ---
// ESP32-S3 上 GPIO25/32/34 不存在, 暂时禁用
// 建议接线: SD=D14(GPIO14), SCK=D15(GPIO15), WS=D16(GPIO16)
#define MIC_SD_PIN      -1    // 未连接
#define MIC_SCK_PIN     -1    // 未连接
#define MIC_WS_PIN      -1    // 未连接

// --- I2S DAC 喇叭 (MAX98357A, 输出) ---
// 接线: DOUT=D38(GPIO38), BCLK=D18(GPIO18), LRC=D39(GPIO39)
#define SPK_DOUT_PIN    38    // D38
#define SPK_BCLK_PIN    18    // D18
#define SPK_LRC_PIN     39    // D39

// --- 心电监护 AD8232 (预留, 当前为 stub) ---
// ESP32-S3 上 GPIO34/35 不存在, 暂时禁用
// 建议接线: EKG=D17(GPIO17), REF=D18(GPIO18) — 注意 D18 与喇叭 BCLK 冲突
#define EKG_PIN         -1    // 未连接
#define EKG_REF_PIN     -1    // 未连接

// --- 用户交互 ---
#define BTN_PIN         2     // D2  - 按钮 (报警确认 / 静音切换)

// =============================================================================
// 传感器阈值与报警参数
// =============================================================================

#define TEMP_MIN_C      18.0
#define TEMP_MAX_C      32.0
#define HUM_MIN_PCT     30.0
#define HUM_MAX_PCT     70.0
#define PRES_MIN_HPA    950.0
#define PRES_MAX_HPA    1060.0
#define NOISE_MAX_DB    85.0

#define ALARM_COOLDOWN_MS   30000UL
#define SENSOR_RETRY_ATTEMPTS 3
#define SENSOR_RETRY_DELAY_MS  50UL
#define SENSOR_INTERVAL_MS    2000UL
#define DISPLAY_INTERVAL_MS   1000UL
#define REPORT_INTERVAL_MS    10000UL

#define HTTP_TIMEOUT_MS   8000UL
#define HTTP_RETRY_ATTEMPTS 3
#define WIFI_CONNECT_TIMEOUT_MS 30000UL

// =============================================================================
// 共享数据结构
// =============================================================================

struct SensorReading {
  float   temp_c;
  bool    valid_temp;
  float   hum_pct;
  bool    valid_hum;
  float   pres_hpa;
  bool    valid_pres;
  float   noise_db;
  bool    valid_noise;
  uint32_t timestamp;

  bool    anyValid() const {
    return valid_temp || valid_hum || valid_pres || valid_noise;
  }
};

enum class AlarmState : uint8_t {
  NONE      = 0,
  ACTIVE    = 1,
  SNOOZED   = 2
};

enum class AlarmCause : uint8_t {
  NONE      = 0,
  TEMP_HIGH = (1 << 0),
  TEMP_LOW  = (1 << 1),
  HUM_HIGH  = (1 << 2),
  HUM_LOW   = (1 << 3),
  PRES_HIGH = (1 << 4),
  PRES_LOW  = (1 << 5),
  NOISE_HIGH= (1 << 6)
};

struct RuntimeState {
  SensorReading lastReading;
  AlarmState    alarmState   = AlarmState::NONE;
  AlarmCause    alarmCauses  = AlarmCause::NONE;
  uint32_t      alarmTriggeredAt = 0;
  uint32_t      alarmSnoozedAt   = 0;
  bool          otaInProgress    = false;
  int           otaProgressPct   = 0;
  bool          wifiConnected    = false;
  int           wifiReconnectCount = 0;
  uint32_t      wifiAttemptStartMs = 0;
  uint32_t      lastReportMs     = 0;
  uint32_t      lastDisplayMs    = 0;
  uint32_t      sensorReadCount  = 0;
  uint32_t      sensorFailCount  = 0;
  uint32_t      reportSuccessCount = 0;
  uint32_t      reportFailCount    = 0;
};

extern RuntimeState g_state;

// =============================================================================
// 驱动模块函数原型
// =============================================================================

// --- oled_driver.cpp ---
bool oledInit();
void oledShowBoot();
void oledShowEnvironment(const SensorReading& r);
void oledShowAlarm(AlarmCause causes, AlarmState state);
void oledShowOtaProgress(int percent);
void oledShowWifiStatus(bool connected, int reconnectCount);

// --- sensors.cpp ---
SensorReading readSensors();
bool calibrateSensors();
bool sensorsIsCalibrated();

// --- audio.cpp ---
bool audioInit();
void audioUpdate();
void audioAlarm(bool enable);
void audioPlayTestTone();
void audioMute();
void audioUnmute();
bool audioIsMuted();
void audioShutdown();

// --- platform_client.cpp ---
bool platformRegister();
bool platformReport(const SensorReading& r);
bool platformCheckOta(int& progressPct);
size_t platformCacheSize();
void platformCacheClear();
bool platformLogin();

// --- ekg_stub.cpp ---
bool ekgInitialize();
float ekgRead();
bool ekgParse(float* amplitude);
float ekgHeartRateBpm();
bool ekgIsSimulated();
bool ekgIsInitialized();

// =============================================================================
// 辅助函数原型
// =============================================================================
uint8_t computeAlarmCauses(const SensorReading& r);
bool    isAlarmDue(const SensorReading& r);
void    processButton();

// =============================================================================
// OTA 配置
// =============================================================================
#define OTA_ENABLED       1
#define OTA_PORT          80
#define OTA_USERNAME      "admin"
#define OTA_PASSWORD      "admin123"

#endif // ENVMON_ESP32_H
