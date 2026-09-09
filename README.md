# EnvMon ESP 固件

版本: v2.0.0
日期: 2026-09-09
作者: QA Engineer (Hermes Agent)

## 项目结构

```
envmon-firmware/
├── README.md           # 本文件
├── CHANGELOG.md        # 变更记录
├── .gitignore
├── docs/
│   ├── PINS.md         # 引脚分配
│   ├── PROTOCOL.md     # 与平台的通信协议
│   ├── WIRING.md       # 硬件连接图
│   └── OTA.md          # OTA 升级流程
├── esp32/
│   ├── envmon_esp32.ino        # 主固件
│   ├── envmon_esp32.h          # 配置
│   ├── oled_driver.cpp         # OLED 显示
│   ├── sensors.cpp             # 温湿度/气压/麦克风
│   ├── audio.cpp               # 喇叭/扬声器
│   ├── platform_client.cpp     # HTTP 上报
│   └── ekg_stub.cpp            # 心电监护占位 (后期实现)
├── esp8266/
│   ├── envmon_esp8266.ino      # 主固件
│   ├── envmon_esp8266.h        # 配置
│   ├── oled_driver.cpp         # OLED 显示 (引脚不同)
│   ├── sensors.cpp             # 温湿度 (无麦克风/喇叭)
│   └── platform_client.cpp     # HTTP 上报
```

## 与平台的通信

- **协议**: HTTP POST (不用 MQTT)
- **URL**: `http://<server>:<port>/api/ingest`
- **鉴权**: `Authorization: Bearer <token>`
- **格式**: `{"device_id":"x","temp_c":36.5,"hum_pct":55.0,"pres_hpa":1013.0}`
- **详细文档**: `docs/PROTOCOL.md`

## 硬件差异

| 特性 | ESP32 | ESP8266 |
|------|-------|---------|
| OLED 引脚 | I2C: SDA=21, SCL=22 | I2C: SDA=4, SCL=5 |
| 温湿度 | DHT22 (GPIO 4) | DHT11 (GPIO 3) |
| 麦克风 | I2S | ❌ 无 |
| 喇叭 | I2S DAC (GPIO 25/26) | ❌ 无 |
| 心电监护 | 预留 GPIO 34/35 (ADC1) | ❌ 无 |
| OTA | ✅ | ✅ |

## 编译

需要 Arduino IDE 或 PlatformIO。

### Arduino IDE
1. 安装 ESP32 和 ESP8266 开发板包
2. 添加库:
   - Adafruit SSD1306
   - Adafruit GFX Library
   - DHT sensor library
   - Adafruit BMP280
   - Arduino_Ame (ESP32 麦克风)
   - AudioFS (ESP32 喇叭)
3. 打开 `esp32/envmon_esp32.ino` 或 `esp8266/envmon_esp8266.ino`
4. 修改配置文件中的 WiFi、服务器、token
5. 编译上传

### PlatformIO
见各子目录的 `platformio.ini`

## 部署流程

1. 修改配置 (WiFi、服务器地址、token、设备 ID)
2. 编译生成 .bin 文件
3. 通过 `esptool.py` 烧录或通过 Arduino IDE 上传
4. 首次启动会自动通过 `/api/ingest` 上报并注册设备
5. 后续通过 OTA 更新固件

## 后续待办

- [ ] 心电监护设备 (AD8232) 接入实现
- [ ] BLE 配置模式 (免 WiFi 配网)
- [ ] HTTPS 加密传输
- [ ] 本地数据缓存 (网络断开时)
- [ ] 看门狗和异常恢复
- [ ] 更完善的 OTA 回滚机制
