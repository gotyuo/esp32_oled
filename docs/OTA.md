# OTA 升级流程文档 (OTA.md)

版本: v2.0.0
适用平台: ESP32 / ESP8266 (均支持)

本文档说明 EnvMon 固件的 OTA (Over-The-Air) 升级流程：工作原理、固件构建、服务器上传、触发更新、回滚机制。

---

## 1. OTA 工作原理

### 1.1 整体架构

```
┌─────────────┐    HTTP    ┌─────────────────┐   WiFi    ┌──────────┐
│  开发工作站  │──────────▶│  OTA 服务器       │◀─────────│  ESP 设备 │
│  (构建/上传) │           │  /api/ota/firm  │           │ (运行中)  │
└─────────────┘           └─────────────────┘           └──────────┘
                                              │              │
                                              │ ① 查询版本    │
                                              │ ② 下载固件    │
                                              │ ③ 校验 + 烧录 │
                                              ▼              ▼
                                         [校验 SHA256]   [重启并应用]
```

### 1.2 设备端工作流

```
┌─────────────────────────────────────────────────────────┐
│ 1. 启动后检查 OTA 端点 (HTTP GET /api/ota/firmware)     │
│ 2. 比较远程版本号 vs 本地版本号                          │
│ 3. 若远程 > 本地:                                        │
│    a. 下载固件到 RAM (分块流式)                          │
│    b. 校验 SHA256                                        │
│    c. 写入 OTA 分区 (使用双分区表)                       │
│    d. 重启, 加载新分区                                   │
│ 4. 启动失败: 回滚到旧分区                                │
└─────────────────────────────────────────────────────────┘
```

### 1.3 双分区表

ESP32/8266 使用 OTA 分区表，固件烧入到 2 个分区：

```
┌─────────────┬─────────────┬─────────────────┐
│  Bootloader │ Partition   │  ota_0          │
│  (16 KB)    │  Table      │  (固件 A)        │
├─────────────┤ (4 KB)      ├─────────────────┤
│             │             │  ota_1          │
│             │             │  (固件 B)        │
└─────────────┴─────────────┴─────────────────┘
```

- 当前运行的固件在 `ota_0` 或 `ota_1`
- 新版本烧入到另一个空闲分区
- 通过 `ESP32.partitionTable()` 决定下次启动用哪个分区

### 1.4 校验机制

1. **SHA256**: 固件上传到服务器时一并存储 SHA256 哈希
2. 设备下载后重新计算哈希，与服务端比对
3. 不一致 → 中止更新

---

## 2. 固件构建

### 2.1 准备工具链

- **PlatformIO** (推荐) — 统一构建 ESP32 / ESP8266
- 或 **Arduino IDE** + ESP32/ESP8266 开发板包

### 2.2 修改版本信息

每次发布前，在 `envmon_*.h` 中递增版本号：

```cpp
#define FIRMWARE_VERSION "2.0.0"
#define FIRMWARE_BUILD   20260909
```

### 2.3 PlatformIO 构建

```bash
cd envmon-firmware
platformio run -e envmon-esp32     # 构建 ESP32 固件
platformio run -e envmon-esp8266   # 构建 ESP8266 固件
```

产物位置:
```
.esp32/build/*.bin    # ESP32 固件 (含合并后 boot+partition+app)
.esp8266/build/*.bin  # ESP8266 固件
```

### 2.4 Arduino IDE 构建

1. 选择开发板 (ESP32 Dev Module / NodeMCU 1.0)
2. 选择 Flash Size 与 Partition Scheme (推荐 "16M Flash (3MB APP/1.9MB FATFS)")
3. 选择 Tools > Programmers > "Built-in programmer"
4. 生成二进制文件: Sketch > Export Compiled Binary

### 2.5 生成 SHA256

```bash
# ESP32
sha256sum build/envmon-esp32.bin
# 输出: <hex>  build/envmon-esp32.bin

# ESP8266
sha256sum build/envmon-esp8266.bin
```

将 SHA256 与版本号一并记录到服务器。

---

## 3. 上传 OTA 固件到服务器

### 3.1 服务器端点 (平台侧)

平台应提供以下端点：

| 端点 | 方法 | 用途 |
|------|------|------|
| `POST /api/ota/upload` | POST | 上传新固件 ( multipart/form-data ) |
| `GET /api/ota/firmware?platform=esp32` | GET | 查询当前最新版本 (返回 JSON 元信息) |
| `GET /api/ota/download?platform=esp32` | GET | 下载固件 .bin (可选鉴权) |

### 3.2 上传固件

```bash
# 通过 curl 上传
curl -X POST "http://server:8080/api/ota/upload" \
  -H "Authorization: Bearer <admin-token>" \
  -F "platform=esp32" \
  -F "version=2.0.1" \
  -F "sha256=<hex>" \
  -F "firmware=@build/envmon-esp32.bin" \
  -F "changelog=Fix DHT22 retry bug"
```

预期响应：

```json
{
  "status": "ok",
  "version": "2.0.1",
  "platform": "esp32",
  "size_bytes": 1024000,
  "download_url": "http://server:8080/api/ota/download?platform=esp32&version=2.0.1"
}
```

### 3.3 服务器存储结构 (示例)

```
/ota/
├── esp32/
│   ├── current.json          # {"version":"2.0.1", "sha256":"...", "size":...}
│   ├── 2.0.1.bin
│   └── 2.0.0.bin             # 保留上一版本用于回滚
└── esp8266/
    ├── current.json
    ├── 2.0.1.bin
    └── 2.0.0.bin
```

---

## 4. 触发 OTA 更新

### 4.1 设备端查询 (启动时)

```cpp
#include <HTTPClient.h>
#include <Update.h>

bool checkAndApplyOTA() {
  HTTPClient http;
  http.begin("http://server:8080/api/ota/firmware?platform=esp32");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int code = http.GET();
  if (code != 200) return false;

  String json = http.getString();
  http.end();

  // 解析: {"version":"2.0.1","sha256":"...","url":"..."}
  // 比较本地 FIRMWARE_VERSION
  if (remoteVersion > FIRMWARE_VERSION) {
    return applyOTA(remoteURL, remoteSHA);
  }
  return false;
}
```

### 4.2 设备端下载 + 烧录

```cpp
bool applyOTA(String url, String expectedSHA) {
  HTTPClient http;
  http.begin(url);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int code = http.GET();
  if (code != 200) return false;

  HTTPHeaders headers;
  http.getHeaderFields(&headers);
  size_t size = http.getSize();

  if (!Update.begin(size)) {
    Update.printError(Serial);
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t written = Update.stream(*stream);
  if (written != size) {
    Update.abort();
    return false;
  }

  if (!Update.end()) {
    Update.printError(Serial);
    return false;
  }

  // 重启加载新分区
  ESP.restart();
  return false;
}
```

### 4.3 手动触发

可通过以下方式触发 OTA：

1. **自动**: 设备启动时检查 (推荐)
2. **远程指令**: 平台下发 "upgrade" 指令 → 设备立即检查
3. **物理按钮**: 长按 BOOT 按钮 5s 进入强制升级模式 (开发阶段)
4. **串口指令**: 上传串口指令 `ota` 触发检查

### 4.4 周期检查

设备每 24 小时检查一次 OTA (避免每次启动都查):

```cpp
static uint32_t lastOTACheck = 0;
const uint32_t OTA_CHECK_INTERVAL = 24 * 3600 * 1000;  // 24h (ms)

void maybeCheckOTA() {
  if (millis() - lastOTACheck < OTA_CHECK_INTERVAL) return;
  if (checkAndApplyOTA()) {
    lastOTACheck = millis();
  }
}
```

---

## 5. 回滚机制

### 5.1 自动回滚 (启动失败检测)

如果新固件在启动阶段崩溃 (看门狗超时、栈溢出、断言失败):

1. ESP 看门狗触发复位
2. 启动代码检测到 `esp_ota_get_state_partition()` 为新分区
3. 若启动计数超过阈值 (默认 3 次)，自动切换回旧分区
4. 重启加载旧固件

```cpp
// 伪代码
void setup() {
  int failCount = loadFailCount();  // 从 NVS 读取
  if (failCount >= 3) {
    // 切换回旧分区
    otaSetSelectedSlot(OTA_SLOT_SECONDARY);
    saveFailCount(0);
    ESP.restart();
  }
  saveFailCount(failCount + 1);

  // ... 正常启动逻辑 ...

  // 启动成功, 重置计数
  saveFailCount(0);
}
```

### 5.2 远程回滚

平台可通过 API 命令设备回滚：

```
POST /api/ota/rollback?device_id=envmon-001&target_version=2.0.0
```

设备收到后跳转到指定版本对应的分区。

### 5.3 手动回滚 (物理按钮)

- 短按 BOOT 按钮 1 次 → 强制回滚到上一分区
- 长按 BOOT 按钮 5s → 进入强制升级模式 (跳过 OTA 检查)
- 长按 BOOT 按钮 10s → 清除 NVS 配置 (慎用)

### 5.4 NVS 持久化存储

使用 ESP32/8266 的 NVS (Non-Volatile Storage) 保存:

| 键 | 类型 | 说明 |
|----|------|------|
| `fw.fail_count` | uint32 | 当前分区连续启动失败次数 |
| `fw.current_slot` | uint32 | 当前使用分区 (0 或 1) |
| `fw.version` | string | 当前固件版本 |

---

## 6. 常见问题

**Q: OTA 更新失败怎么办？**
A: 自动回滚到旧分区。检查服务器端日志与 SHA256 校验。

**Q: 如何确认 OTA 成功？**
A: 查看设备日志 `Serial: "OTA applied, version 2.0.1"`, 或查询平台端设备版本。

**Q: OTA 期间网络断了怎么办？**
A: 中止更新，保留旧分区，下次启动重试。

**Q: OTA 期间断电了怎么办？**
A: 新分区不完整，启动失败 → 看门狗 → 自动回滚旧分区。

**Q: 如何支持 HTTPS OTA？**
A: 当前使用 HTTP。HTTPS 支持列入 v2.1.0 计划 (见 CHANGELOG.md)。

**Q: 是否需要分批灰度发布？**
A: 当前未实现。可手动为部分设备发送升级指令实现灰度。
