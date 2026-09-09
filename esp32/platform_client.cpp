/**
 * =============================================================================
 * EnvMon ESP32 - 平台 HTTP 客户端 (platform_client.cpp)
 * =============================================================================
 *
 * 协议: HTTP POST (非 MQTT, 见 docs/PROTOCOL.md)
 * URL : http://<MQTT_SERVER>:<MQTT_PORT>/api/ingest
 * 鉴权: Authorization: Bearer <AUTH_TOKEN>
 *
 * 提供:
 *   - platformRegister()    设备注册 (首次上电上报固件信息)
 *   - platformReport(r)     上报一次传感器采样
 *   - platformCheckOta()    查询是否有可用固件更新
 *
 * 错误处理:
 *   - 网络超时: HTTP_TIMEOUT_MS
 *   - 重试: HTTP_RETRY_ATTEMPTS (指数退避)
 *   - 鉴权失败: 打印并返回 false, 不无限重试
 *   - 统计: 成功/失败次数计入 g_state
 *
 * 依赖: WiFi.h, HTTPClient.h, ArduinoJson
 *
 * 版本: v2.0.0
 * =============================================================================
 */

#include "envmon_esp32.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// =============================================================================
// 模块内部状态
// =============================================================================
namespace {
  // 上报端点路径 (与 docs/PROTOCOL.md 一致)
  const char* INGEST_PATH  = "/api/ingest";
  const char* OTA_LIST_PATH = "/api/ota/list";  // 平台无 /api/ota/check, 用 list
  const char* REGISTER_PATH = "/api/devices";   // 平台注册端点

  // 请求头常量
  const char* HEADER_AUTH    = "Authorization";
  const char* HEADER_JSON    = "Content-Type";
  const char* CONTENT_JSON   = "application/json";

  // 连续失败计数 (用于判断是否需要进入离线缓存模式)
  uint32_t s_consecFailures = 0;

  // 离线缓存: 网络断开时暂存待上报数据 (FIFO)
  // 使用环形缓冲, 最多缓存 CACHE_MAX 条
  struct CacheEntry {
    SensorReading reading;
    uint32_t      enqueuedMs;
  };
  static const size_t CACHE_MAX = 16;
  CacheEntry  s_cache[CACHE_MAX];
  size_t      s_cacheHead = 0;   // 写指针
  size_t      s_cacheCount = 0;  // 当前缓存条数

  // 注册状态
  bool s_registered = false;
}

// =============================================================================
// 内部工具
// =============================================================================

/**
 * @brief 构造完整的请求 URL (scheme://host:port/path)
 */
static String buildUrl(const char* path) {
  return String("http://") + MQTT_SERVER + ":" + String(MQTT_PORT) + String(path);
}

/**
 * @brief 构造鉴权头值
 */
static String buildAuthToken() {
  return String("Bearer ") + AUTH_TOKEN;
}

/**
 * @brief 将 SensorReading 序列化为 JSON 字符串
 *
 * 只序列化有效字段, 避免上报 NaN / 无效数据。
 * 附加 device_id 与 ts 供平台去重/排序。
 */
static String serializeReading(const SensorReading& r) {
  StaticJsonDocument<256> doc;
  doc["device_id"] = DEVICE_ID;
  doc["fw_ver"]    = FIRMWARE_VERSION;
  doc["ts"]        = r.timestamp;

  if (r.valid_temp)  doc["temp_c"]   = roundf(r.temp_c * 10.0f) / 10.0f;
  if (r.valid_hum)   doc["hum_pct"]  = roundf(r.hum_pct * 10.0f) / 10.0f;
  if (r.valid_pres)  doc["pres_hpa"] = roundf(r.pres_hpa * 10.0f) / 10.0f;
  if (r.valid_noise) doc["noise_db"] = roundf(r.noise_db * 10.0f) / 10.0f;

  // 报警状态 (供平台侧联动)
  doc["alarm"]      = (uint8_t)g_state.alarmState;
  doc["alarm_causes"] = (uint8_t)g_state.alarmCauses;

  String out;
  serializeJson(doc, out);
  return out;
}

/**
 * @brief 通用 HTTP POST 请求 (带重试与超时)
 *
 * @param path    请求路径
 * @param payload JSON 请求体
 * @param bodyOut 可选, 返回服务端响应体
 *
 * @return true  2xx 成功; false 失败
 *
 * 重试策略: 前 N 次失败均重试, 采用固定间隔退避。
 * 鉴权失败 (401/403) 不重试 (重试无意义), 直接返回 false。
 */
static bool httpPost(const char* path, const String& payload, String* bodyOut) {
  if (!g_state.wifiConnected) {
    ELOG("httpPost: WiFi not connected");
    return false;
  }

  String url = buildUrl(path);
  bool ok = false;

  for (uint8_t attempt = 1; attempt <= HTTP_RETRY_ATTEMPTS; attempt++) {
    HTTPClient http;
    http.setTimeout((uint32_t)HTTP_TIMEOUT_MS);
    http.setConnectTimeout((uint32_t)HTTP_TIMEOUT_MS);

    http.begin(url);
    http.addHeader(HEADER_AUTH, buildAuthToken());
    http.addHeader(HEADER_JSON, CONTENT_JSON);

    int code = http.POST(payload);
    if (code > 0) {
      // 2xx 成功
      if (code >= 200 && code < 300) {
        if (bodyOut) bodyOut->reserve(http.getSize());
        int len = http.getSize();
        WiFiClient* stream = http.getStreamPtr();
        if (len > 0) {
          for (int i = 0; i < len; i++) {
            int c = stream->read();
            if (c < 0) break;
            bodyOut->concat((char)c);
          }
        }
        ELOG("HTTP POST %s OK (code=%d)", path, code);
        ok = true;
        break;
      } else if (code == 401 || code == 403) {
        // 鉴权失败: 不重试
        ELOG("HTTP POST %s AUTH FAILED (code=%d)", path, code);
        String body;
        int len = http.getSize();
        if (len > 0) {
          WiFiClient* stream = http.getStreamPtr();
          for (int i = 0; i < len; i++) {
            int c = stream->read();
            if (c < 0) break;
            body.concat((char)c);
          }
        }
        ELOG("  response: %s", body.c_str());
        http.end();
        return false;
      } else {
        // 5xx / 4xx (非鉴权): 可重试
        ELOG("HTTP POST %s failed (code=%d, attempt=%u/%u)",
             path, code, attempt, HTTP_RETRY_ATTEMPTS);
      }
    } else {
      // 传输层失败 (连接超时等)
      ELOG("HTTP POST %s transport failed (err=%d, attempt=%u/%u)",
           path, code, attempt, HTTP_RETRY_ATTEMPTS);
    }

    http.end();

    // 指数退避: 1s, 2s, 4s ...
    if (attempt < HTTP_RETRY_ATTEMPTS) {
      delay(1000UL * (1UL << (attempt - 1)));
    }
  }

  return ok;
}

/**
 * @brief 将采样入队缓存 (网络断开时使用)
 */
static void cacheEnqueue(const SensorReading& r) {
  size_t idx = (s_cacheHead + s_cacheCount) % CACHE_MAX;
  if (s_cacheCount < CACHE_MAX) {
    s_cache[idx] = { r, millis() };
    s_cacheCount++;
  } else {
    // 缓存满: 覆盖最旧的一条 (环形)
    s_cache[idx] = { r, millis() };
    if (s_cacheCount == 0) s_cacheHead = 0;
    ELOG("Cache full, dropped oldest");
  }
}

/**
 * @brief 从缓存取出最旧一条并上报; 成功则出队
 *
 * @return true 上报成功 (已出队); false 失败 (保留)
 */
static bool cacheDrainOne() {
  if (s_cacheCount == 0) return false;

  const SensorReading& r = s_cache[s_cacheHead].reading;
  String payload = serializeReading(r);
  if (httpPost(INGEST_PATH, payload, nullptr)) {
    // 出队: 头指针前进
    s_cacheHead = (s_cacheHead + 1) % CACHE_MAX;
    s_cacheCount--;
    g_state.reportSuccessCount++;
    ELOG("Cache drained 1 (%u remaining)", s_cacheCount);
    return true;
  }
  return false;
}

// =============================================================================
// 对外接口
// =============================================================================

/**
 * @brief 设备注册
 *
 * 首次上电调用, 上报设备 ID 与固件版本, 平台据此绑定设备。
 * 注册失败不阻塞运行 (仍可上报), 但需关注平台侧设备列表。
 *
 * @return true 注册成功; false 失败
 */
bool platformRegister() {
  if (s_registered) {
    ELOG("Already registered, skip");
    return true;
  }

  if (!g_state.wifiConnected) {
    ELOG("platformRegister: WiFi not connected");
    return false;
  }

  // 注册请求体
  StaticJsonDocument<192> doc;
  doc["device_id"] = DEVICE_ID;
  doc["fw_ver"]    = FIRMWARE_VERSION;
  doc["hw_model"]  = "ESP32-ENV";
  doc["capabilities"] = "temp,humidity,pressure,mic";

  String payload;
  serializeJson(doc, payload);

  String resp;
  if (httpPost(REGISTER_PATH, payload, &resp)) {
    s_registered = true;
    // 解析响应 (可选: 获取平台分配的设备 token)
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, resp)) {
      const char* msg = doc["message"];
      if (msg) ELOG("Register response: %s", msg);
    }
    ELOG("Device registered: %s", DEVICE_ID);
    return true;
  } else {
    ELOG("Device registration FAILED (will continue running)");
    return false;
  }
}

/**
 * @brief 上报一次传感器采样
 *
 * @param r  采样结果
 *
 * @return true 上报成功; false 失败 (可能已缓存)
 *
 * 行为:
 *   - 数据全无效 (anyValid()==false): 直接跳过, 不计入失败
 *   - WiFi 断开: 入队缓存, 返回 false (但不算上报失败)
 *   - 网络正常: 直接 POST; 失败则入队缓存
 */
bool platformReport(const SensorReading& r) {
  // 全无效数据无需上报
  if (!r.anyValid()) {
    ELOG("platformReport: no valid data, skip");
    return false;
  }

  // WiFi 断开: 缓存
  if (!g_state.wifiConnected) {
    cacheEnqueue(r);
    ELOG("WiFi down, reading cached (%u total)", s_cacheCount);
    g_state.reportFailCount++;
    s_consecFailures++;
    return false;
  }

  String payload = serializeReading(r);
  if (httpPost(INGEST_PATH, payload, nullptr)) {
    g_state.reportSuccessCount++;
    g_state.lastReportMs = millis();
    s_consecFailures = 0;
    // 上报成功时尝试清空缓存 (网络恢复后追补历史数据)
    if (s_cacheCount > 0) {
      ELOG("WiFi recovered, draining %u cached readings", s_cacheCount);
      while (cacheDrainOne()) {
        // 限制单次追补数量, 避免阻塞主循环过久
        if (g_state.reportSuccessCount % 4 == 0) break;
      }
    }
    return true;
  }

  // 上报失败: 缓存 + 计数
  cacheEnqueue(r);
  g_state.reportFailCount++;
  s_consecFailures++;
  ELOG("Report failed, cached (%u total, %u consec fail)",
       s_cacheCount, s_consecFailures);
  return false;
}

/**
 * @brief 查询是否有可用固件更新
 *
 * @param progressPct  输出: 当前 OTA 进度百分比 (无更新时为 0)
 *
 * @return true  有可用更新 (需配合 OTA 流程下载); false 无更新或查询失败
 *
 * 请求 OTA_PATH, 解析响应中是否包含新版本。
 * 当前实现仅做版本检查, 实际 OTA 下载由 esp_http_ota 配合完成。
 */
bool platformCheckOta(int& progressPct) {
  progressPct = 0;

  if (!g_state.wifiConnected) {
    return false;
  }

  // 平台无 /api/ota/check, 改用 GET /api/ota/list 获取可用固件列表
  // 然后对比版本号判断是否需要升级
  String url = buildUrl(OTA_LIST_PATH);
  
  HTTPClient http;
  http.setTimeout((uint32_t)HTTP_TIMEOUT_MS);
  http.setConnectTimeout((uint32_t)HTTP_TIMEOUT_MS);
  http.begin(url);
  http.addHeader(HEADER_AUTH, buildAuthToken());
  
  int code = http.GET();
  if (code != 200) {
    ELOG("OTA list failed (HTTP %d)", code);
    http.end();
    return false;
  }
  
  // 解析响应, 检查是否有比当前版本更新的固件
  String resp = http.getString();
  http.end();
  
  DynamicJsonDocument dResp(512);
  if (deserializeJson(dResp, resp)) {
    ELOG("OTA list parse error");
    return false;
  }
  
  // 遍历固件列表, 找最新版本
  JsonArray images = dResp["images"];
  if (images == nullptr || images.size() == 0) {
    ELOG("No OTA images available");
    return false;
  }
  
  // 简化: 取最后一个 (假设按版本排序)
  JsonObject lastImg = images[images.size() - 1];
  const char* newVer = lastImg["version"];
  
  if (newVer && strcmp(newVer, FIRMWARE_VERSION) > 0) {
    ELOG("OTA available: %s -> %s", FIRMWARE_VERSION, newVer);
    g_state.otaInProgress = true;
    progressPct = 0;
    return true;
  }
  
  ELOG("No OTA available (current=%s, latest=%s)", FIRMWARE_VERSION, newVer);
  return false;
}

/**
 * @brief 获取缓存条数 (供诊断)
 */
size_t platformCacheSize() {
  return s_cacheCount;
}

/**
 * @brief 强制清空缓存 (用户确认无需追补时调用)
 */
void platformCacheClear() {
  s_cacheCount = 0;
  s_cacheHead  = 0;
  ELOG("Cache cleared");
}
