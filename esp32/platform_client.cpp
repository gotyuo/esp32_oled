/**
 * =============================================================================
 * EnvMon ESP32 - 平台 HTTP 客户端 (platform_client.cpp)
 * =============================================================================
 *
 * 协议: HTTP POST
 * 鉴权: 自动登录获取 token (platformLogin)
 *
 * 版本: v8.0.0
 * =============================================================================
 */

#include "envmon_esp32.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

namespace {
  const char* INGEST_PATH  = "/api/ingest";
  const char* OTA_LIST_PATH = "/api/ota/list";
  const char* REGISTER_PATH = "/api/devices";
  const char* LOGIN_PATH    = "/api/login";

  const char* HEADER_AUTH    = "Authorization";
  const char* HEADER_JSON    = "Content-Type";
  const char* CONTENT_JSON   = "application/json";

  uint32_t s_consecFailures = 0;

  // 运行时 token (从 /api/login 获取)
  String g_runtimeToken = "";

  struct CacheEntry {
    SensorReading reading;
    uint32_t      enqueuedMs;
  };
  static const size_t CACHE_MAX = 16;
  CacheEntry  s_cache[CACHE_MAX];
  size_t      s_cacheHead = 0;
  size_t      s_cacheCount = 0;

  bool s_registered = false;
}

static String buildUrl(const char* path) {
  return String("http://") + MQTT_SERVER + ":" + String(MQTT_PORT) + String(path);
}

static String buildAuthToken() {
  return String("Bearer ") + g_runtimeToken;
}

static String serializeReading(const SensorReading& r) {
  StaticJsonDocument<256> doc;
  doc["device_id"] = DEVICE_ID;
  doc["fw_ver"]    = FIRMWARE_VERSION;
  doc["ts"]        = r.timestamp;

  if (r.valid_temp)  doc["temp_c"]   = roundf(r.temp_c * 10.0f) / 10.0f;
  if (r.valid_hum)   doc["hum_pct"]  = roundf(r.hum_pct * 10.0f) / 10.0f;
  if (r.valid_pres)  doc["pres_hpa"] = roundf(r.pres_hpa * 10.0f) / 10.0f;
  if (r.valid_noise) doc["noise_db"] = roundf(r.noise_db * 10.0f) / 10.0f;

  doc["alarm"]      = (uint8_t)g_state.alarmState;
  doc["alarm_causes"] = (uint8_t)g_state.alarmCauses;

  String out;
  serializeJson(doc, out);
  return out;
}

/**
 * @brief 自动登录获取 token
 *
 * @return true 登录成功; false 失败
 */
bool platformLogin() {
  if (!g_state.wifiConnected) {
    ELOG("platformLogin: WiFi not connected");
    return false;
  }

  // 登录退避: 失败后至少等 LOGIN_BACKOFF_MS 才重试。
  // 否则每 10s 上报一次 -> 每次都打 /api/login -> 服务端 429 限流,
  // 缓存被填满后持续丢数据。
  static uint32_t s_lastLoginMs = 0;
  uint32_t now = millis();
  if (s_lastLoginMs && (now - s_lastLoginMs) < LOGIN_BACKOFF_MS) {
    return false;   // 静默: 未到重试间隔
  }
  s_lastLoginMs = now;

  StaticJsonDocument<192> doc;
  doc["username"] = LOGIN_USERNAME;
  doc["password"] = LOGIN_PASSWORD;

  String payload;
  serializeJson(doc, payload);

  String url = buildUrl(LOGIN_PATH);
  HTTPClient http;
  http.setTimeout((uint32_t)HTTP_TIMEOUT_MS);
  http.setConnectTimeout((uint32_t)HTTP_TIMEOUT_MS);

  http.begin(url);
  http.addHeader(HEADER_JSON, CONTENT_JSON);

  int code = http.POST(payload);
  if (code == 200) {
    String resp = http.getString();
    http.end();

    DynamicJsonDocument dResp(256);
    if (!deserializeJson(dResp, resp)) {
      const char* token = dResp["token"];
      if (token) {
        g_runtimeToken = token;
        ELOG("platformLogin: token acquired (%d chars)", token[0] ? strlen(token) : 0);
        return true;
      }
    }
    ELOG("platformLogin: no token in response: %s", resp.c_str());
    return false;
  }

  // 失败时打印响应体: 平台返回的错误信息(如 {"detail":"..."})能直接说明
  // 401 的原因, 不用连 SSH 查服务端日志。
  // code 要区分开: -1 = 网络/超时(平台不可达), 401 = 凭证错, 429 = 限流。
  String resp = http.getString();
  const char* why = (code < 0) ? "network/timeout"
                   : (code == 401) ? "credentials rejected"
                   : (code == 429) ? "rate limited"
                   : "other";
  ELOG("platformLogin: failed (code=%d, %s, body=%s)", code, why, resp.c_str());
  http.end();
  return false;
}

/**
 * @brief 通用 HTTP POST 请求 (带重试与超时)
 *
 * @param bodyOut 可选, 返回服务端响应体 (可为 nullptr)
 */
static bool httpPost(const char* path, const String& payload, String* bodyOut) {
  if (!g_state.wifiConnected) {
    ELOG("httpPost: WiFi not connected");
    return false;
  }

  // 如果没有 token, 先登录。
  // platformLogin() 内部带退避: 失败后 60s 内直接返回 false 而不打日志,
  // 否则退避期间每 10s 上报一次都会刷一条 "login failed", 串口日志被刷屏,
  // 反而看不清真正的故障码。
  if (g_runtimeToken.isEmpty()) {
    if (!platformLogin()) {
      return false;   // 静默: 登录失败原因由 platformLogin() 自己打(带退避节流)
    }
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
      if (code >= 200 && code < 300) {
        if (bodyOut) bodyOut->reserve(http.getSize());
        int len = http.getSize();
        WiFiClient* stream = http.getStreamPtr();
        if (len > 0 && stream) {
          for (int i = 0; i < len; i++) {
            int c = stream->read();
            if (c < 0) break;
            if (bodyOut) bodyOut->concat((char)c);
          }
        }
        ELOG("HTTP POST %s OK (code=%d)", path, code);
        ok = true;
        break;
      } else if (code == 401 || code == 403) {
        // Token 过期或无效, 尝试重新登录
        ELOG("HTTP POST %s AUTH FAILED (code=%d), re-login", path, code);
        g_runtimeToken = "";
        if (!platformLogin()) {
          ELOG("Re-login failed");
          http.end();
          return false;
        }
        // 重试一次
        attempt--;
      } else {
        ELOG("HTTP POST %s failed (code=%d, attempt=%u/%u)",
             path, code, attempt, HTTP_RETRY_ATTEMPTS);
      }
    } else {
      ELOG("HTTP POST %s transport failed (err=%d, attempt=%u/%u)",
           path, code, attempt, HTTP_RETRY_ATTEMPTS);
    }

    http.end();

    if (attempt < HTTP_RETRY_ATTEMPTS) {
      delay(1000UL * (1UL << (attempt - 1)));
    }
  }

  return ok;
}

static void cacheEnqueue(const SensorReading& r, bool* droppedOldest) {
  size_t idx = (s_cacheHead + s_cacheCount) % CACHE_MAX;
  if (s_cacheCount < CACHE_MAX) {
    s_cache[idx] = { r, millis() };
    s_cacheCount++;
    if (droppedOldest) *droppedOldest = false;
  } else {
    s_cache[idx] = { r, millis() };
    if (s_cacheCount == 0) s_cacheHead = 0;
    if (droppedOldest) *droppedOldest = true;
  }
}

static bool cacheDrainOne() {
  if (s_cacheCount == 0) return false;

  const SensorReading& r = s_cache[s_cacheHead].reading;
  String payload = serializeReading(r);
  if (httpPost(INGEST_PATH, payload, nullptr)) {
    s_cacheHead = (s_cacheHead + 1) % CACHE_MAX;
    s_cacheCount--;
    g_state.reportSuccessCount++;
    ELOG("Cache drained 1 (%u remaining)", s_cacheCount);
    return true;
  }
  return false;
}

bool platformRegister() {
  if (s_registered) {
    ELOG("Already registered, skip");
    return true;
  }

  if (!g_state.wifiConnected) {
    ELOG("platformRegister: WiFi not connected");
    return false;
  }

  // 先登录获取 token
  if (g_runtimeToken.isEmpty()) {
    if (!platformLogin()) {
      ELOG("Registration failed: cannot login");
      return false;
    }
  }

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

bool platformReport(const SensorReading& r) {
  if (!r.anyValid()) {
    ELOG("platformReport: no valid data, skip");
    return false;
  }

  if (!g_state.wifiConnected) {
    bool droppedOldest = false;
    cacheEnqueue(r, &droppedOldest);
    g_state.reportFailCount++;
    s_consecFailures++;
    // 节流: WiFi 断开期间每 10s 上报一次, 只在第 1/10/20... 次打日志。
    if (s_consecFailures == 1 || s_consecFailures % 10 == 0 || droppedOldest) {
      ELOG("WiFi down, reading cached (%u total, %u consec fail)",
           s_cacheCount, s_consecFailures);
    }
    return false;
  }

  String payload = serializeReading(r);
  if (httpPost(INGEST_PATH, payload, nullptr)) {
    g_state.reportSuccessCount++;
    g_state.lastReportMs = millis();
    s_consecFailures = 0;

    if (s_cacheCount > 0) {
      ELOG("WiFi recovered, draining %u cached readings", s_cacheCount);
      while (cacheDrainOne()) {
        if (g_state.reportSuccessCount % 4 == 0) break;
      }
    }
    return true;
  }

  bool droppedOldest = false;
  cacheEnqueue(r, &droppedOldest);
  g_state.reportFailCount++;
  s_consecFailures++;
  // 日志节流: 第 1 次、第 10/20/30... 次打一条, 缓存刚满时打一条。
  // 注意: droppedOldest 一旦缓存满就恒为 true, 不能直接 OR 进条件,
  // 否则条件每次都成立, 等于没节流。这里只在"刚满"的边缘打一次。
  static bool s_overflowLogged = false;
  bool overflowEdge = droppedOldest && !s_overflowLogged;
  s_overflowLogged = s_overflowLogged || droppedOldest;
  if (s_consecFailures == 1 || s_consecFailures % 10 == 0 || overflowEdge) {
    ELOG("Report failed, cached (%u total, %u consec fail%s)",
         s_cacheCount, s_consecFailures, droppedOldest ? ", cache overflow" : "");
  }
  return false;
}

bool platformCheckOta(int& progressPct) {
  progressPct = 0;

  if (!g_state.wifiConnected) return false;

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

  String resp = http.getString();
  http.end();

  DynamicJsonDocument dResp(512);
  if (deserializeJson(dResp, resp)) {
    ELOG("OTA list parse error");
    return false;
  }

  JsonArray images = dResp["images"];
  if (images == nullptr || images.size() == 0) {
    ELOG("No OTA images available");
    return false;
  }

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

size_t platformCacheSize() { return s_cacheCount; }

void platformCacheClear() {
  s_cacheCount = 0;
  s_cacheHead  = 0;
  ELOG("Cache cleared");
}
