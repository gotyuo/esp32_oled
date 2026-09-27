#ifndef ENVMON_ESP8266_H
#define ENVMON_ESP8266_H

// ========== 版本号 ==========
#define FIRMWARE_VERSION     "2.3.0"
#define FIRMWARE_BUILD       "2026-09-28"

// ========== WiFi 配置 ==========
#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"

// ========== 平台配置 ==========
// ICU 监护平台地址, 见 docs/PROTOCOL.md
#define SERVER_HOST     "192.168.68.119"    // 平台服务器 IP
#define SERVER_PORT     12090               // 平台服务端口
#define INGEST_PATH     "/api/ingest"       // 数据上报端点
#define REGISTER_PATH   "/api/devices"      // 设备注册端点
#define OTA_LIST_PATH   "/api/ota/list"     // OTA 固件列表端点

// 鉴权 Token: 从平台 /api/login 获取后填入
#define AUTH_TOKEN      "REPLACE_WITH_…OKEN"
#define DEVICE_ID       "esp8266-001"
#define DEVICE_NAME     "ESP8266 ICU Monitor"

// ========== 引脚分配 (与 ESP32 不同!) ==========
// OLED (SSD1315, 硬件 SPI) - ESP8266 默认 SPI 引脚
//   CS=GPIO15, MOSI=GPIO13, SCK=GPIO14 (硬件固定)
//   DC=GPIO4 (D2) — 注: 原方案 GPIO5 与 MAX30102 SCL 冲突, 故改用 GPIO4
#define OLED_CS_PIN         15   // GPIO15 (D15)
#define OLED_DC_PIN         4    // GPIO4  (D2) — v2.1.0 改 (原 5)
#define OLED_WIDTH          128
#define OLED_HEIGHT         64
#define OLED_RESET_PIN      -1

// DHT11 温湿度
#define TEMP_PIN            12   // GPIO12 (D6)
#define DHT_TYPE            DHT11

// MAX30102 血氧/心率 (软件 I2C, v2.1.0 新增)
// 避开 ESP8266 硬件 SPI 的 GPIO13/14/15 和 OLED 的 GPIO5
#define MAX30102_SDA_PIN      2    // GPIO2 (D1)
#define MAX30102_SCL_PIN      1    // GPIO1 (D10)

// 有源蜂鸣器 / 喇叭驱动 (ESP8266 音频输出)
// 默认 GPIO16 (NodeMCU D0): 远离 GPIO0/GPIO2 启动引脚, 避免启动异常。
// 如果接线不同, 改成实际连接蜂鸣器驱动板信号脚的 GPIO。
#define BUZZER_PIN          16
#define BUZZER_ACTIVE_LOW   1    // 1 = 模块低电平驱动, 0 = 高电平驱动
#define BUZZER_ON_PIN_LEVEL (!BUZZER_ACTIVE_LOW)
#define BUZZER_OFF_PIN_LEVEL (BUZZER_ACTIVE_LOW)
#define BUZZER_ON_MS        180  // 单次报警响铃时长
#define BUZZER_OFF_MS       180  // 单次报警静音间隔
#define BUZZER_INTERVAL_MS  3000 // 报警期间循环发声间隔
#define BUZZER_WIFI_ON_MS   250  // WiFi 连接成功提示音
#define BUZZER_OTA_START_MS 250  // OTA 开始提示音
#define BUZZER_ERROR_MS     450  // 上报失败 / 错误提示音

// ESP8266 没有 BMP280 (内存限制)
// ESP8266 没有麦克风
// ESP8266 音频使用 GPIO 驱动的有源蜂鸣器 / 喇叭模块

// ========== 时间间隔 ==========
#define REPORT_INTERVAL     30000  // 30 秒上报一次
#define HEARTBEAT_INTERVAL  30000  // 30 秒心跳
#define OLED_UPDATE_MS      1000   // OLED 每秒更新
#define RETRY_COUNT         3      // 网络重试次数
#define RETRY_DELAY_MS      2000   // 重试间隔

// ========== 报警阈值 (生理参数) ==========
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
  float spo2;        // 血氧 (%) — MAX30102, v2.1.0 新增
  float heart_rate;  // 心率 (bpm) — MAX30102, v2.1.0 新增
  float bp_systolic; // 收缩压 (mmHg) — 血压估算, v2.2.0 启用
  float bp_diastolic;// 舒张压 (mmHg) — 血压估算, v2.2.0 启用
  uint32_t timestamp_ms;
  bool valid;        // 温湿度是否有效
  bool vital_valid;  // 生命体征 (SpO2/HR) 是否有效
  bool bp_calibrated;// 血压已校准 (个体基线就绪) — v2.3.0 新增
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

extern bool netConnected;

// 音频 / 喇叭驱动
void audioInit();
void audioTick();
void audioStartWiFiOn();
void audioStartBoot();
void audioStartOtaStart();
void audioStartError();
void audioSetAlarm(bool active);
bool audioAlarmActive();
bool audioBusy();

// MAX30102 血氧/心率驱动 (v2.1.0 新增)
bool max30102Init();
void max30102Tick();
void max30102GetResult(float* spo2, float* hr, bool* valid);
void max30102GetBpResult(float* sbp, float* dbp, bool* valid);
bool max30102IsInitialized();
int32_t max30102GetBufferIndex();

// 血压估算 (v2.2.0 新增; v2.3.0 启用个体基线校准)
void bpEstimateUpdate(const uint32_t* irWave, int waveLen,
                      float heart_rate,
                      float* sbp, float* dbp, bool* valid);
float bpEstimateGetSbp();
float bpEstimateGetDbp();
bool bpEstimateIsValid();

#endif // ENVMON_ESP8266_H
