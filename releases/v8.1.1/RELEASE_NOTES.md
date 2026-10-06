# EnvMon ESP32-S3 固件 v8.1.1

**构建日期**: 2026-10-06
**Git 提交**: `217bb58`
**上一版**: v8.1.0 (白屏修复)

---

## 本版本修复

### 1. WiFi 密码错误(真正的黑屏根因之一)

固件里 `WIFI_PASSWORD` 写的是 `gotyuo987`,但路由器的真实密码是 `321654987`。
WiFi 连不上 → 设备卡在连接/重启循环 → 屏幕表现为黑屏。

```
WIFI_PASSWORD  gotyuo987  →  321654987
```

修好后实测:
```
WiFi 已连接 IP: 172.22.22.164 RSSI: -15 dBm
```

注意 `LOGIN_PASSWORD` 仍是 `gotyuo987` —— 那是**平台**登录密码,不是路由器密码,
两者不相关。

### 2. 平台登录死循环触发 429 限流

旧版 `platformReport()` 每 10s 上报一次,每次都先调 `platformLogin()`,
失败后立刻重试。一分钟就能打 6 次 `/api/login`,触发服务端 429 限流,
缓存被填满后持续丢数据,串口日志每 10s 刷一条。

**改动**:

- `platformLogin()` 失败后退避 60s(`LOGIN_BACKOFF_MS`),期间静默返回
- `httpPost()` 登录退避期间不再打 `login failed` 日志
- `platformReport()` 失败日志节流:第 1/10/20… 次及缓存溢出时才打印

实测 25 秒内仅 1 条失败日志(之前同时间约 15 条)。

### 3. 登录失败原因可诊断

原来只打 `failed (code=401)`,看不出来是哪一类失败。现在打印状态码 + 响应体 + 分类:

```
platformLogin: failed (code=-1, network/timeout, body=)
platformLogin: failed (code=401, credentials rejected, body={"detail":"..."})
platformLogin: failed (code=429, rate limited, body=)
```

`-1` = 网络不可达/超时,`401` = 凭证错,`429` = 限流。下次不用连 SSH 查服务端日志。

---

## 接线 (SSD1306 OLED, I2C)

| OLED | ESP32-S3 | D 丝印 |
|------|----------|--------|
| VCC  | 3.3V     | 3V3    |
| GND  | GND      | GND    |
| SDA  | GPIO8    | **D8** |
| SCL  | GPIO9    | **D9** |

## 喇叭 (MAX98357A, I2S DAC)

| MAX98357A | ESP32-S3 | D 丝印 |
|-----------|----------|--------|
| DIN       | GPIO38   | **D38** |
| BCLK      | GPIO18   | **D18** |
| LRC       | GPIO39   | **D39** |
| VCC       | 3.3V     | 3V3    |
| GND       | GND      | GND    |

## 传感器

| 器件 | 引脚 |
|------|------|
| DHT22 | GPIO4 (**D4**) |
| BMP280 | I2C 0, 0x77(与 OLED 共用, SDA=D8 SCL=D9) |

## 烧录

```bash
cd /home/hotyuo/envmon-firmware
/vol1/pio-venv/bin/pio run -e esp32 --target upload --upload-port /dev/ttyACM0
```

## 校验

```
$ sha256sum firmware/envmon-esp32-v8.1.1.bin
6aef82c72733ddb5d55da7ad114e7d6a23497a02de927ab06ef4df570db2f554
```

## 已知问题

- **DHT22 读失败**(streak 持续增长)。温度湿度采不到,与屏幕无关,独立问题。
- **平台 `172.22.22.83:12090` 不可达**(`code=-1, network/timeout`)。
  设备 WiFi 正常但平台连不上,需服务侧确认该端口是否还在监听。
