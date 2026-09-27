# 变更记录 (CHANGELOG.md)

EnvMon ESP 固件 - 全部版本记录

格式遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.0.0/) 与语义化版本 (SemVer)。

---

## [v2.2.0] - 2026-09-20

### 概述

**血压估算功能启用**。基于 v2.1.0 的 MAX30102 PPG 波形数据，使用单路 PPG 形态学 + 心率经验公式估算 SBP/DBP (无创 cuffless BP)。

### Added (新增功能)

- **血压估算模块**: `esp8266/bp_estimator.cpp`
  - 基于单路 PPG 波形 (IR 通道 100 样本) 的形态学分析
  - 提取特征: 收缩峰幅值、收缩期上升斜率、舒张峰位置、脉搏波宽度 (PWV proxy)
  - 经验公式: SBP/DBP = 基线 (120/80 mmHg) + 特征加权项 + 心率调整项
  - 自适应突变抑制: 单次估算与上次差异 > 30 mmHg 视为异常, 保持上次值
  - 生理约束: SBP > DBP+10, SBP∈[80,180], DBP∈[50,110]

- **血压报警阈值**: `esp8266/max30102_config.h`
  - SBP < 90 或 > 150 mmHg 报警
  - DBP < 50 或 > 100 mmHg 报警

- **血压集成**: `esp8266/max30102_driver.cpp` + `sensors.cpp`
  - `bpEstimateUpdate()` 在每次心率计算后调用
  - `max30102GetBpResult()` 读取 SBP/DBP/有效性
  - SensorData 结构新增 `bp_systolic` / `bp_diastolic` 字段

- **OLED 血压显示**: `esp8266/oled_driver.cpp`
  - 底行右侧显示 SBP/DBP (例如 `120/80`)
  - 仅血压估算有效时显示

- **JSON 上报扩展**: `esp8266/platform_client.cpp`
  - `/api/ingest` 上报新增 `bp_systolic` / `bp_diastolic` 字段

### 参考

- 刘乔寿, 王森. 基于单路 PPG 信号的连续血压检测算法设计. 电子设计工程, 2019, 27(1): 63-69.
  https://gitee.com/NaN01/heart-rate-sp-o2-analyzer
- Zeitzman et al. "Blood pressure monitoring by way of photoplethysmography."
  J Biomech Eng 2014. (PMC4512231)

### 已知限制

- **非临床精度**: 单点估算 ±10-15 mmHg, 仅作辅助参考, 不能用于诊断
- **依赖 PPG 信号质量**: 用户手指按压不足、运动伪影、环境光过强会导致估算不可靠
- **首次估算依赖基线值**: 用户静息心率偏离基线 (75 bpm) 时 SBP 偏差会放大
- **冷启动**: 需要至少一次完整 100 样本 PPG 波形 (~4 秒) 才能产出 BP

---

## [v2.1.0] - 2026-09-20

### 概述

**MAX30102 血氧/心率接入**。ESP8266 设备新增 PPG 传感器, 可采集 SpO2 (%) + 心率 (bpm)。

### Added

- **MAX30102 原生驱动**: `esp8266/max30102_driver.cpp`
  - 直接 Wire.h 操作 MAX30102 寄存器 (不依赖外部库, 节省 ESP8266 内存)
  - SpO2 模式 (RED+IR 双 LED), 采样率 100Hz, 411μs 脉宽, 4096nA ADC
  - 非阻塞 `tick()`: 轮询 FIFO, 累积 100 样本 (~4 秒) 后调用算法
  - 简化版 Maxim MAXREFDES117# 算法: AC/DC 比值法 + 峰值检测
  - 输出: SpO2 (%) + 心率 (bpm) + 有效性标志

- **配置头**: `esp8266/max30102_config.h` (引脚与 SpO2/HR 阈值)
- **报警阈值**: SpO2 < 94%, HR < 50 或 > 120 bpm 报警
- **OLED 更新**: 主屏显示 SpO2/HR 大字, 温湿度小字
- **JSON 上报**: `/api/ingest` 新增 `spo2_pct` / `heart_rate` 字段

### 引脚

- MAX30102 SDA=GPIO2 (D1), SCL=GPIO1 (D10)
- 避开 ESP8266 硬件 SPI (GPIO13/14/15) 与 OLED CS/DC
- OLED DC 从 GPIO5 改到 GPIO4 (因原 GPIO5 与 MAX30102 SCL 冲突)

### 参考

- Gitee: https://gitee.com/NaN01/heart-rate-sp-o2-analyzer
- GitHub: https://github.com/sparkfun/SparkFun_MAX3010x_Sensor_Library
- 算法: Maxim MAXREFDES117# (spo2_algorithm.h, BSD)
- 文档: Maxim MAX30102 datasheet 2015 v1.3

---

## [v2.0.1] - 2026-09-20

### Fixed

- **ESP8266 构建失败** (PlatformIO 6.x src_dir 行为)
  - 重命名 env: `envmon-esp8266` → `esp8266`, `envmon-esp32` → `esp32` (匹配目录名)
  - 设置 `src_dir = esp8266` (在 [platformio] 段生效)
  - 简化 `build_src_filter`
  - 移除 `esp8266/dht_probe.ino` (避免 PlatformIO 多 .ino 冲突), 移到 `probes/`

---

## [v2.0.0] - 2026-09-09

### 概述

**完整重写版本**。从 v1.x 的 MQTT 单平台架构迁移到 HTTP 双平台架构。所有代码、配置、协议、文档均重新组织。

### Added (新增功能)

- **双平台支持**: 同时支持 ESP32 与 ESP8266
  - `esp32/` 目录: ESP32 固件 (完整功能)
  - `esp8266/` 目录: ESP8266 固件 (精简功能)
- **HTTP 协议**: 替代 v1.x 的 MQTT 协议，POST 到 `/api/ingest`
- **多传感器融合** (ESP32):
  - DHT22 温湿度传感器 (GPIO 4)
  - BMP280 气压传感器 (I2C, 与 OLED 共享总线)
  - I2S 麦克风 (GPIO 34/32/25)
  - I2S DAC 喇叭 (GPIO 25/26)
  - AD8232 心电监护预留 (GPIO 34/35)
- **OLED 显示**: SSD1306 I2C 显示 (实时数据 + 状态指示)
- **OTA 升级**: 双分区表 + 自动回滚机制
- **完整文档**: PINS / PROTOCOL / WIRING / OTA 四份文档
- **PlatformIO 配置**: 统一构建脚本

### Changed (变更)

- **协议**: MQTT → HTTP POST (见下文 "修复的 Bug")
- **目录结构**: 扁平 → `esp32/` + `esp8266/` + `docs/` 分层
- **配置**: 分散于代码中 → 集中到 `envmon_*.h` 头文件
- **设备标识**: 从 IP 推断 → 显式 `device_id` 字段
- **端口**: 5683 (MQTT over UDP) → 8080 (HTTP over TCP)

### Fixed (修复的 Bug)

#### Bug #1: 协议选择错误 (MQTT vs HTTP)

- **问题**: v1.x 使用 MQTT over UDP (端口 5683)，但平台后端已迁移到 HTTP REST
- **原因**: 开发分支未与平台端保持同步，导致设备上线后无法上报
- **修复**: v2.0.0 全面切换为 HTTP POST 到 `/api/ingest`，使用 `HTTPClient` 库
- **影响**: 设备与平台重新对接，历史数据保留

#### Bug #2: 端口错误 (5683 vs 8080)

- **问题**: 固件硬编码 MQTT 端口 5683，平台实际监听 8080
- **原因**: 配置未参数化，每次部署需修改源码
- **修复**: 配置头文件 `#define PLATFORM_PORT 8080`，编译时统一
- **影响**: 部署流程简化

#### Bug #3: 单平台耦合

- **问题**: v1.x 仅支持 ESP32，ESP8266 项目无法复用代码
- **原因**: 引脚分配与传感器驱动硬编码
- **修复**: 拆分 `esp32/` 与 `esp8266/` 目录，共享平台无关逻辑
- **影响**: ESP8266 用户可以使用精简版固件

#### Bug #4: OTA 无回滚

- **问题**: v1.x OTA 失败后设备变砖，需物理烧录恢复
- **原因**: 未使用双分区表，未实现看门狗回滚
- **修复**: v2.0.0 启用 OTA 双分区 + NVS 记录启动失败计数 + 自动回滚
- **影响**: 设备升级安全

#### Bug #5: 鉴权缺失

- **问题**: v1.x 上报无鉴权，任何设备都可冒充上报
- **修复**: v2.0.0 增加 Bearer Token 鉴权
- **影响**: 安全性提升

### Removed (移除)

- **MQTT 客户端代码**: `mqtt_client.cpp` 已删除
- **UDP 上报路径**: 不再支持 UDP 协议
- **单平台代码**: `envmon.ino` (无平台区分) 已删除
- **本地存储日志**: 改为平台端持久化存储

### Known Issues (已知问题)

1. **明文 HTTP**: 当前使用 HTTP 而非 HTTPS，Token 可能在线路中泄露。HTTPS 列入 v2.3.0 计划。
2. **心电监护未实现**: AD8232 接线已预留，但 `ekg_stub.cpp` 仅占位，无实际采集逻辑。
3. **无 BLE 配网**: WiFi 凭据需硬编码，不支持 BLE 无屏配网。列入 v2.3.0。
4. **离线缓存未实现**: 网络断开时数据丢失，未实现本地队列。
5. **GPIO 25 复用冲突**: 麦克风 BCLK 与喇叭 DAC 共用 GPIO 25，运行时需软件分时。
6. **无本地看门狗**: 异常崩溃依赖启动失败检测，运行时无实时看门狗。
7. **OTA 灰度发布**: 当前所有设备同时升级，无灰度策略。
8. **ESP8266 无气压**: ESP8266 无 BMP280 支持，上报数据缺少 `pres_hpa` 字段。
9. **ADC1 校准**: WiFi 使用时 ADC1 通道 (GPIO 34/35) 校准会失效，影响心电精度。

---

## [v1.x] - (历史版本，已归档)

### 概述

v1.x 系列使用 MQTT over UDP 上报到端口 5683，仅支持 ESP32 单平台。该架构因平台后端迁移已废弃，不再维护。

### 主要变化

- v1.0.0: 初始版本，MQTT + DHT22 + OLED
- v1.1.0: 增加 BMP280
- v1.2.0: 增加麦克风
- v1.3.0: 增加 OTA (无回滚)
- v1.4.0: 最终版本，准备迁移到 HTTP

### 已知问题 (v1.x)

- 协议与平台不一致 (Bug #1, #2)
- OTA 失败设备变砖 (Bug #4)
- 无鉴权 (Bug #5)
- 单平台耦合 (Bug #3)

---

## [Unreleased / v2.3.0] - (计划中)

### 计划新增

- [ ] **HTTPS 加密传输**: 使用 `esp_crt_bundle_attach()` 内置证书
- [ ] **BLE 配网模式**: 通过 BLE 输入 WiFi 凭据，免焊接改配置
- [ ] **离线数据缓存**: 网络断开时本地 NVS 队列，恢复后重传
- [ ] **看门狗**: 任务级硬件看门狗，防止死循环
- [ ] **心电监护 (AD8232)**: 实际数据采集与上报
- [ ] **OTA 灰度发布**: 按设备分组分批升级
- [ ] **OTA HTTPS**: 与 HTTPS 传输对齐
- [ ] **设备管理 API**: 平台端设备列表、状态查询
- [ ] **血压估算校准**: 多用户基线学习, 减少个体偏差

### 计划修复

- [ ] GPIO 25 复用冲突: 重分配引脚或实现硬件分时
- [ ] ADC1 校准: WiFi 启动后重新校准 ADC
- [ ] 启动失败阈值可配置
- [ ] ESP8266 蓝牙配网 (BLE Mesh 或 ESPNow)

---

## 版本命名约定

```
vMAJOR.MINOR.PATCH
  │     │      │
  │     │      └─ Bug 修复，向后兼容
  │     └──────── 新功能，向后兼容
  └────────────── 重大变更，可能不兼容
```

- **v2.0.0**: 重写版本，协议/架构重大变更
- **v2.0.1**: 修复 v2.0.0 中的小 bug (PlatformIO 6.x 构建)
- **v2.1.0**: MAX30102 SpO2 + 心率接入
- **v2.2.0**: 血压估算启用
- **v2.3.0**: 计划中 (HTTPS / BLE 配网 / 离线缓存等)

---

## 贡献指南

提交变更前请:

1. 更新 `CHANGELOG.md` 中的对应版本
2. 更新 `FIRMWARE_VERSION` 宏
3. 更新相关文档 (`docs/`)
4. 通过测试
5. 提交 PR
