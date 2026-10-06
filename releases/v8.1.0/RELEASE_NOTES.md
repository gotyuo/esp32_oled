# EnvMon ESP32-S3 固件 v8.1.0

**构建日期**: 2026-10-06
**Git 标签**: `v8.1.0` (commit `9995d10`)
**回退点**: `/home/hotyuo/envmon-firmware/.rollback/pre-nonblocking-20261006-180730`

---

## 本版本修复:OLED 黑屏

### 现象
设备启动后 OLED 屏幕不亮(黑屏)。

### 根因
旧版 `setup()` 的顺序是:

```
oledInit()        ← 屏幕点亮
connectWiFi()     ← 阻塞等待 WiFi, 最长 300 秒
calibrateSensors()  ← WiFi 连上之前永远执行不到
audioInit()
ekgInitialize()
```

WiFi 连不上时,设备卡在 `connectWiFi()` 里 300 秒不动,传感器/音频/ECG 全部不初始化。屏幕虽然点亮过一次,但停在启动画面不刷新,加上反复重启,表现为黑屏。

### 修复
`connectWiFi()` 改为**非阻塞启动**,新增 `updateWiFi()` 状态机由 `loop()` 轮询:

```
oledInit()              ← 1. 屏幕先亮
connectWiFi()  [非阻塞]  ← 2. WiFi 后台启动, 立即返回
calibrateSensors()      ← 3. WiFi 连上之前, 传感器就已初始化
audioInit()             ← 4. 喇叭也初始化
ekgInitialize()
                    ↓  (WiFi 连上后, loop 内一次性)
setupOTA() / platformRegister()
```

- WiFi 连接超时 300s → 30s (`WIFI_CONNECT_TIMEOUT_MS`),超时干净重启重试
- OTA 与平台注册延迟到 WiFi 首次连上时执行
- 屏幕在 WiFi 等待期间持续刷新状态画面

### 验证(串口日志, 实测)
```
 EnvMon ESP32 Firmware v8.1.0
 Build: 2026-10-06
[ENVMON] OLED init OK (128x64, FONT5X7, contrast=255)   ← 屏幕先亮
启动 WiFi 连接 (非阻塞): JDCwifi_6010
[ENVMON] BMP280 init OK @0x77                             ← WiFi 未连上也已初始化
[ENVMON] DHT22 begin (pin 4)
[ENVMON] Audio I2S DAC init OK (DOUT=38 BCLK=18 LRC=39, 16000Hz)
[ENVMON] ECG init OK (STUB / simulated mode)
....                                                       ← WiFi 后台重试
```
40 秒窗口内观察到 30 秒超时触发的干净重启,确认看门狗路径正常。

### 顺带修复
- 移除 `platformio.ini` 中重复的 `-DFIRMWARE_VERSION` / `-DFIRMWARE_BUILD` 命令行定义。
  命令行 `-D` 会覆盖 `envmon_esp32.h` 里的定义,导致源码里的版本号是死代码,
  之前一直显示 8.0.0。现在版本号只有一处来源(`esp32/envmon_esp32.h:46`)。

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
| BMP280 | I2C 0, 地址 0x77 (与 OLED 共用总线, SDA=D8 SCL=D9) |

## 烧录

```bash
cd /home/hotyuo/envmon-firmware
/vol1/pio-venv/bin/pio run -e esp32 --target upload --upload-port /dev/ttyACM0
```

## 校验

```
$ sha256sum firmware/envmon-esp32-v8.1.0.bin
1e23ae93468e754a9ce5def7169cc72561a8abfd471f96570de648272a291863
```
