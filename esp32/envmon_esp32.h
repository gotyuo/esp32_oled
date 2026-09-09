/**
 * =============================================================================
 * EnvMon ESP32 - 配置头文件 (envmon_esp32.h)
 * =============================================================================
 *
 * 设备: ESP32 开发板 (环境/生理监测终端)
 * 功能: 温湿度、气压、音频采集 + OLED 显示 + HTTP 上报 + OTA
 *
 * 此文件集中定义:
 *   - 引脚分配 (Pin Map)
 *   - WiFi / 平台服务器 / 鉴权配置
 *   - 传感器阈值与报警参数
 *   - 共享数据结构 (SensorReading 等)
 *   - 全局状态管理
 *   - 跨文件可见的函数原型 (各驱动 .cpp 实现)
 *
 * 依赖库:
 *   - Adafruit SSD1306 (OLED)
 *   - Adafruit GFX Library
 *   - DHT sensor library (DHT22)
 *   - Adafruit BMP280
 *   - ESP32 I2S Audio (AudioFS / I2S)
 *   - ArduinoJson
 *   - HTTPClient / WiFi
 *
 * 版本: v2.0.0  |  日期: 2026-09-09
 * =============================================================================
 */

#ifndef ENVMON_ESP32_H
#define ENVMON_ESP32_H

// =============================================================================
// 基础包含
// =============================================================================
#include <Arduino.h>

// 编译期日志开关: 设为 1 打开串口日志, 0 关闭以节省资源/流量
#ifndef ENVMON_LOG_ENABLE
#define ENVMON_LOG_ENABLE 1
#endif

// 串口输出工具宏: 仅在开启时编译, 避免生产固件打印海量日志
#if ENVMON_LOG_ENABLE
  #define ELOG(fmt, ...)  Serial.printf("[%s] " fmt "\n", TAG, ##__VA_ARGS__)
#else
  #define ELOG(fmt, ...)  ((void)0)
#endif
#define TAG "ENVMON"

// =============================================================================
// 设备身份配置 —— 每台设备需在编译前确认
// =============================================================================

// 设备唯一标识: 上报时作为 device_id 提交, 平台据此注册/绑定设备
// 命名规范: esp32-{序号}, 例: esp32-001, esp32-002
#define DEVICE_ID       "esp32-001"

// 固件版本字符串: 平台通过此字段判断是否需要触发 OTA
#define FIRMWARE_VERSION "2.0.0"
#define FIRMWARE_BUILD   "2026-09-09"

// WiFi 凭据 (修改为你的实际 WiFi)
#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"

// 平台服务器 (HTTP, 非 MQTT)
// ICU 监护平台地址, 见 docs/PROTOCOL.md
#define SERVER_HOST     "192.168.68.119"  // 平台服务器 IP
#define SERVER_PORT     12090             // 平台服务端口

// 兼容旧代码引用: 保留 MQTT_* 作为别名
#define MQTT_SERVER     SERVER_HOST
#define MQTT_PORT       SERVER_PORT

// 鉴权 Token: 从平台 /api/login 获取后填入
// 生产环境请通过 OTA/配置系统下发, 切勿硬编码长期有效凭据到代码仓库
#define AUTH_TOKEN      "REPLACE_WITH_…_TOKEN"

// =============================================================================
// 引脚分配 (Pin Map) —— 与 docs/PINS.md 保持一致
// =============================================================================

// --- OLED (SSD1306, I2C) ---
#define OLED_SDA_PIN    21    // GPIO21 - I2C SDA (与 BMP280 共用 I2C 总线)
#define OLED_SCL_PIN    22    // GPIO22 - I2C SCL
#define OLED_RESET_PIN  -1    // OLED 复位脚, -1 表示不使用外部复位

// --- DHT22 温湿度传感器 ---
#define TEMP_PIN        4     // GPIO4  - DHT22 数据脚

// --- BMP280 气压传感器 (I2C, 与 OLED 共用总线) ---
#define BMP280_SDA      OLED_SDA_PIN   // 复用 I2C SDA
#define BMP280_SCL      OLED_SCL_PIN   // 复用 I2C SCL
#define BMP280_ADDRESS  0x76          // BMP280 默认 I2C 地址

// --- I2S 麦克风 (采集) ---
#define MIC_SD_PIN      34    // GPIO34 - 数据 (SD)
#define MIC_SCK_PIN     32    // GPIO32 - 时钟 (SCK)
#define MIC_WS_PIN      25    // GPIO25 - 字选/帧选 (WS)

// --- I2S DAC 喇叭 (输出) ---
#define SPK_DOUT_PIN    26    // GPIO26 - DAC 数据输出
#define SPK_BCLK_PIN    25    // 复用 WS 作为 BCLK (典型 I2S DAC 接线)
#define SPK_LRC_PIN     27    // GPIO27 - 左声道/声道选择 (备用)

// --- 心电监护 AD8232 (预留, 当前为 stub) ---
#define EKG_PIN         35    // GPIO35 - ECG 模拟信号输入 (ADC1_CH7)
#define EKG_REF_PIN     34    // GPIO34 - 参考信号 (占位)

// --- 用户交互 ---
#define BTN_PIN         0     // GPIO0 - 按钮 (报警确认 / 静音切换)

// =============================================================================
// 传感器阈值与报警参数
// =============================================================================

// 温度 (摄氏度)
#define TEMP_MIN_C      18.0
#define TEMP_MAX_C      32.0

// 湿度 (百分比)
#define HUM_MIN_PCT     30.0
#define HUM_MAX_PCT     70.0

// 气压 (hPa)
#define PRES_MIN_HPA    950.0
#define PRES_MAX_HPA    1060.0

// 噪声 (分贝, 麦克风估算)
#define NOISE_MAX_DB    85.0

// 报警冷却: 触发后 N 秒内不重复触发 (避免报警风暴)
#define ALARM_COOLDOWN_MS   30000UL   // 30 秒

// 传感器读取重试: 单次读取失败后重试次数
#define SENSOR_RETRY_ATTEMPTS 3
#define SENSOR_RETRY_DELAY_MS  50UL

// 传感器采样周期 (毫秒)
#define SENSOR_INTERVAL_MS    2000UL

// 显示刷新周期 (毫秒)
#define DISPLAY_INTERVAL_MS   1000UL

// 上报周期 (毫秒)
#define REPORT_INTERVAL_MS    10000UL

// 网络超时 / 重试
#define HTTP_TIMEOUT_MS   8000UL
#define HTTP_RETRY_ATTEMPTS 3
#define WIFI_CONNECT_TIMEOUT_MS 30000UL

// =============================================================================
// 共享数据结构
// =============================================================================

/**
 * @brief 单次传感器采样结果
 *
 * 各传感器字段是否有效由 valid_* 标记指示。
 * 当某传感器离线/故障时, 对应数值为 NaN 且 valid 为 false,
 * 调用方需据此决定是否跳过上报或显示"--"占位。
 */
struct SensorReading {
  float   temp_c;        // 温度, 摄氏度
  bool    valid_temp;

  float   hum_pct;       // 相对湿度, %RH
  bool    valid_hum;

  float   pres_hpa;      // 气压, hPa
  bool    valid_pres;

  float   noise_db;      // 估算噪声, dB(A)
  bool    valid_noise;

  uint32_t timestamp;    // millis() 采样时刻

  /** 是否至少有一项数据有效 */
  bool    anyValid() const {
    return valid_temp || valid_hum || valid_pres || valid_noise;
  }
};

/**
 * @brief 报警状态枚举
 */
enum class AlarmState : uint8_t {
  NONE      = 0,   // 无报警
  ACTIVE    = 1,   // 报警激活 (正在鸣响)
  SNOOZED   = 2    // 报警已静音/暂缓
};

/**
 * @brief 报警触发原因 (可叠加)
 */
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

/**
 * @brief 全局运行时状态
 *
 * 由主循环 (envmon_esp32.ino) 维护, 各驱动模块通过
 * extern 引用读写, 实现跨文件状态共享。
 */
struct RuntimeState {
  // 最新一次有效采样
  SensorReading lastReading;

  // 报警
  AlarmState    alarmState   = AlarmState::NONE;
  AlarmCause    alarmCauses  = AlarmCause::NONE;
  uint32_t      alarmTriggeredAt = 0;     // 报警首次触发时刻
  uint32_t      alarmSnoozedAt   = 0;     // 静音时刻

  // OTA
  bool          otaInProgress    = false;
  int           otaProgressPct   = 0;      // 0-100

  // 网络
  bool          wifiConnected    = false;
  int           wifiReconnectCount = 0;
  uint32_t      lastReportMs     = 0;

  // 显示
  uint32_t      lastDisplayMs    = 0;

  // 计数统计
  uint32_t      sensorReadCount  = 0;
  uint32_t      sensorFailCount  = 0;
  uint32_t      reportSuccessCount = 0;
  uint32_t      reportFailCount    = 0;
};

// 全局状态实例 (定义在 .ino, 此处声明)
extern RuntimeState g_state;

// =============================================================================
// 驱动模块函数原型 (实现在各 .cpp)
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
void audioUpdate();                 // 主循环周期调用, 处理报警/测试音相位
void audioAlarm(bool enable);
void audioPlayTestTone();
void audioMute();
void audioUnmute();
bool audioIsMuted();
void audioShutdown();               // OTA 前释放音频资源 (可选)

// --- platform_client.cpp ---
bool platformRegister();
bool platformReport(const SensorReading& r);
bool platformCheckOta(int& progressPct);
size_t platformCacheSize();   // 离线缓存条数 (诊断)
void platformCacheClear();    // 强制清空离线缓存

// --- ekg_stub.cpp ---
bool ekgInitialize();
float ekgRead();
bool ekgParse(float* amplitude);
float ekgHeartRateBpm();   // 当前心率 (bpm), stub 返回模拟值
bool ekgIsSimulated();     // 是否模拟模式
bool ekgIsInitialized();   // 初始化状态

// =============================================================================
// 辅助函数原型
// =============================================================================
uint8_t computeAlarmCauses(const SensorReading& r);
bool    isAlarmDue(const SensorReading& r);
void    processButton();

#endif // ENVMON_ESP32_H
