#ifndef ENVMON_ESP8266_H
#define ENVMON_ESP8266_H

// ========== 版本号 ==========
#define FIRMWARE_VERSION     "2.0.0"
#define FIRMWARE_BUILD       "2026-09-09"

// ========== WiFi 配置 ==========
const char* WIFI_SSID       = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD   = "YOUR_WIFI_PASSWORD";

// ========== 平台配置 ==========
// ICU 监护平台地址, 见 docs/PROTOCOL.md
const char* SERVER_HOST     = "192.168.68.119";  // 平台服务器 IP
const int   SERVER_PORT     = 12090;              // 平台服务端口
const char* INGEST_PATH     = "/api/ingest";      // 数据上报端点
const char* REGISTER_PATH   = "/api/devices";     // 设备注册端点
const char* OTA_LIST_PATH   = "/api/ota/list";    // OTA 固件列表端点

// 鉴权 Token: 从平台 /api/login 获取后填入
const char* AUTH_TOKEN      = "REPLACE_WITH_…_TOKEN";
const char* DEVICE_ID       = "esp8266-001";
const char* DEVICE_NAME     = "ESP8266 ICU Monitor";

// ========== 引脚分配 (与 ESP32 不同!) ==========
// OLED (I2C) - ESP8266 默认 I2C 引脚
#define OLED_SDA            4    // GPIO4 (注意与 ESP32 的 21 不同)
#define OLED_SCL            5    // GPIO5 (注意与 ESP32 的 22 不同)
#define OLED_WIDTH          128
#define OLED_HEIGHT         64
#define OLED_RESET_PIN      -1

// DHT11 温湿度 (注意: DHT11 不是 DHT22)
#define TEMP_PIN            3    // GPIO3 (注意与 ESP32 的 4 不同)
#define DHT_TYPE            DHT11

// ESP8266 没有 BMP280 (内存限制)
// ESP8266 没有麦克风
// ESP8266 没有喇叭

// ========== 时间间隔 ==========
#define REPORT_INTERVAL     30000  // 30 秒上报一次
#define HEARTBEAT_INTERVAL  30000  // 30 秒心跳
#define OLED_UPDATE_MS      1000   // OLED 每秒更新
#define RETRY_COUNT         3      // 网络重试次数
#define RETRY_DELAY_MS      2000   // 重试间隔

// ========== 报警阈值 ==========
#define TEMP_LOW            35.5
#define TEMP_HIGH           37.5
#define HUM_LOW             40.0
#define HUM_HIGH            70.0

// ========== OTA 配置 ==========
#define OTA_ENABLED         true
#define OTA_PASSWORD        "admin123"
#define OTA_PORT            8266

// ========== 状态 ==========
enum State {
  STATE_INIT,
  STATE_WIFI_CONNECTING,
  STATE_WIFI_CONNECTED,
  STATE_REPORTING,
  STATE_ALARM,
  STATE_OTA,
  STATE_ERROR
};

// ========== 传感器数据结构 ==========
struct SensorData {
  float temp_c;
  float hum_pct;
  uint32_t timestamp_ms;
  bool valid;
};

// ========== 全局状态 ==========
extern State currentState;
extern SensorData currentData;
extern unsigned long lastReportTime;
extern unsigned long lastOledUpdate;
extern unsigned long lastHeartbeat;
extern bool alarmActive;
extern char alarmReason[128];

// ========== 外部函数声明 ==========
void oledInit();
void oledUpdate(const SensorData& data);
void oledShowAlarm(const char* msg);
void oledShowOTA(int progress);
void oledShowStatus(const char* msg);

bool sensorsInit();
void sensorsRead(SensorData& data);
void sensorsCalibrate();

bool netInit();
bool netReport(const SensorData& data);
bool netRegister();
void netHeartbeat();
void otaCheck();

#endif // ENVMON_ESP8266_H
