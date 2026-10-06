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
  } else {
    ELOG("platformLogin: failed (code=%d)", code);
    http.end();
  }
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

  // 如果没有 token, 先登录
  if (g_runtimeToken.isEmpty()) {
    if (!platformLogin()) {
      ELOG("httpPost: login failed, cannot proceed");
      return false;
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

static void cacheEnqueue(const SensorReading& r) {
  size_t idx = (s_cacheHead + s_cacheCount) % CACHE_MAX;
  if (s_cacheCount < CACHE_MAX) {
    s_cache[idx] = { r, millis() };
    s_cacheCount++;
  } else {
    s_cache[idx] = { r, millis() };
    if (s_cacheCount == 0) s_cacheHead = 0;
    ELOG("Cache full, dropped oldest");
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

    if (s_cacheCount > 0) {
      ELOG("WiFi recovered, draining %u cached readings", s_cacheCount);
      while (cacheDrainOne()) {
        if (g_state.reportSuccessCount % 4 == 0) break;
      }
    }
    return true;
  }

  cacheEnqueue(r);
  g_state.reportFailCount++;
  s_consecFailures++;
  ELOG("Report failed, cached (%u total, %u consec fail)",
       s_cacheCount, s_consecFailures);
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
