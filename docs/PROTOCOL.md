# 通信协议文档 (PROTOCOL.md)

版本: v2.0.0
协议: **HTTP POST** (非 MQTT)

本文档定义 EnvMon 固件与平台后端的通信协议。固件周期性将传感器数据 POST 到 `/api/ingest` 端点。

---

## 1. 端点定义

| 项目 | 值 |
|------|-----|
| 方法 | `POST` |
| 路径 | `/api/ingest` |
| 完整 URL | `http://<server>:<port>/api/ingest` |
| Content-Type | `application/json` |
| 鉴权 | `Authorization: Bearer <token>` |
| 默认超时 | 10 s |
| 重试策略 | 指数退避，最多 3 次 |

### 1.1 默认配置 (在 `.h` 中)

```cpp
#define PLATFORM_SERVER "192.168.1.100"   // 平台服务器 IP
#define PLATFORM_PORT   8080              // 平台服务器端口
#define DEVICE_ID       "envmon-001"      // 设备唯一 ID
#define DEVICE_TOKEN    "<your-token>"    // Bearer Token
#define REPORT_INTERVAL 60                // 上报周期 (秒)
```

---

## 2. 请求格式

### 2.1 请求头

```
POST /api/ingest HTTP/1.1
Host: <server>:<port>
Authorization: Bearer <token>
Content-Type: application/json
Content-Length: <n>
Connection: close
```

### 2.2 请求体 (JSON)

| 字段 | 类型 | 必填 | 范围 | 说明 |
|------|------|------|------|------|
| `device_id` | string | ✅ | 1-64 字符 | 设备唯一标识 |
| `temp_c` | number (float) | ✅ | -40 ~ 85 | 摄氏温度 |
| `hum_pct` | number (float) | ✅ | 0 ~ 100 | 相对湿度 |
| `pres_hpa` | number (float) | ESP32 必填 | 300 ~ 1100 | 气压 (hPa) |
| `ts` | number (int64) | 可选 | Unix ms | 上报时间戳 |

> **ESP8266 说明**: ESP8266 无 BMP280 气压传感器，请求中可省略 `pres_hpa` 字段。

### 2.3 请求示例

```json
{
  "device_id": "envmon-001",
  "temp_c": 23.45,
  "hum_pct": 55.0,
  "pres_hpa": 1013.25,
  "ts": 1725945600123
}
```

### 2.4 curl 示例

```bash
curl -X POST "http://192.168.1.100:8080/api/ingest" \
  -H "Authorization: Bearer your-token" \
  -H "Content-Type: application/json" \
  -d '{
    "device_id": "envmon-001",
    "temp_c": 23.45,
    "hum_pct": 55.0,
    "pres_hpa": 1013.25
  }'
```

---

## 3. 响应格式

### 3.1 成功响应 (HTTP 200)

```json
{
  "status": "ok",
  "device_id": "envmon-001",
  "received_at": 1725945600123,
  "next_report_in": 60
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `status` | string | `"ok"` 表示接收成功 |
| `device_id` | string | 已接收的设备 ID |
| `received_at` | number | 服务端接收时间戳 (ms) |
| `next_report_in` | number | 建议下次上报间隔 (秒) |

### 3.2 客户端错误响应

固件遇到非 2xx 响应时按错误码分支处理 (见 §4)。

---

## 4. 错误码说明

| HTTP 状态码 | 含义 | 固件处理 |
|-------------|------|----------|
| 200 | 成功 | 更新下次上报时间，继续 |
| 400 | 请求格式错误 | 输出调试日志，**不重试** (修复代码) |
| 401 | 鉴权失败 (Token 无效) | 输出错误日志，**不重试**，需重新烧录配置 |
| 403 | Token 无权限 | 输出错误日志，**不重试** |
| 404 | 端点不存在 | 输出错误日志，**不重试** (检查 URL/端口) |
| 408 | 请求超时 | 进入重试流程 |
| 429 | 请求过于频繁 | 等待 60s 后重试 |
| 500 | 服务端内部错误 | 指数退避重试 |
| 502/503/504 | 网关错误/服务不可用 | 指数退避重试 |
| - | 网络错误 (DNS/连接失败) | 进入离线模式，本地缓存 |

### 4.1 重试策略

```
attempt = 1
backoff = 2^attempt 秒 (1s, 2s, 4s, 8s, 16s)
max_retries = 3
```

超过最大重试次数 → 进入离线缓存 (待网络恢复后重传)。

### 4.2 错误响应体格式

服务端应返回：

```json
{
  "status": "error",
  "code": "AUTH_FAILED",
  "message": "Invalid or expired token"
}
```

| `code` | 含义 |
|--------|------|
| `AUTH_FAILED` | Token 无效或过期 |
| `BAD_REQUEST` | JSON 格式错误 / 字段缺失 |
| `DEVICE_NOT_FOUND` | 设备未注册 |
| `RATE_LIMITED` | 上报过于频繁 |
| `INTERNAL_ERROR` | 服务端异常 |

---

## 5. 鉴权机制

### 5.1 Bearer Token

- 平台为每台设备分配唯一 Token
- 固件在配置文件中硬编码 Token (`#define DEVICE_TOKEN`)
- 每次请求必须带 `Authorization: Bearer <token>` 头

### 5.2 Token 安全注意事项

- ⚠️ 当前实现为明文 HTTP，Token 可能在线路中被嗅探
- 后续版本将支持 HTTPS + mTLS (见 CHANGELOG.md)
- 生产环境务必在网络层隔离 ESP 设备

---

## 6. 设备注册

- 首次成功上报后，服务端根据 `device_id` 自动注册设备
- 服务端记录：设备 ID、Token、首次上报时间、固件版本 (可选)
- 平台前端可查看每个设备的历史数据

---

## 7. 上报周期

- 默认周期: 60 秒
- 服务端可通过响应的 `next_report_in` 字段动态调整
- 固件在两次上报之间睡眠以省电 (ESP32 支持 Light Sleep)

```cpp
// 主循环伪代码
while (true) {
  read_sensors();
  display_on_oled();
  post_to_platform();          // HTTP POST
  delay(next_report_in * 1000);
}
```

---

## 8. 完整示例代码

### 8.1 ESP32 (Arduino 风格)

```cpp
#include <HTTPClient.h>

void postToPlatform(float temp, float hum, float pres) {
  HTTPClient http;
  http.begin("http://192.168.1.100:8080/api/ingest");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer your-token");

  String body = String() + "{"
    "\"device_id\":\"envmon-001\","
    "\"temp_c\":"    + String(temp, 2) + ","
    "\"hum_pct\":"   + String(hum, 2)  + ","
    "\"pres_hpa\":"  + String(pres, 2) + "}";

  int code = http.POST(body);
  if (code == 200) {
    Serial.println("[OK] ingest accepted");
  } else {
    Serial.printf("[ERR] ingest HTTP %d: %s\n", code, http.errorToString(code).c_str());
  }
  http.end();
}
```

### 8.2 Python (服务端测试)

```python
import requests, time

resp = requests.post(
    "http://192.168.1.100:8080/api/ingest",
    headers={"Authorization": "Bearer your-token"},
    json={
        "device_id": "envmon-001",
        "temp_c": 23.45,
        "hum_pct": 55.0,
        "pres_hpa": 1013.25,
        "ts": int(time.time() * 1000),
    },
    timeout=10,
)
print(resp.status_code, resp.json())
```

---

## 9. 常见问题

**Q: 上报失败怎么办？**
A: 固件输出串口日志，进入重试流程。若网络断开，进入离线缓存 (后续实现)。

**Q: Token 过期怎么办？**
A: 收到 401 后不重试，需重新烧录配置。

**Q: 为什么不用 MQTT？**
A: v2.0.0 简化为 HTTP，便于服务端集成；MQTT 已废弃 (见 CHANGELOG.md)。

**Q: ESP8266 没有气压传感器怎么办？**
A: 省略 `pres_hpa` 字段，服务端按可选字段处理。
