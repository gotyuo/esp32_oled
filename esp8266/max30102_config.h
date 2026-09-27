#ifndef MAX30102_CONFIG_H
#define MAX30102_CONFIG_H

// ========== MAX30102 引脚 (I2C) ==========
// ESP8266 引脚冲突说明:
//   - SDA=GPIO2, SCL=GPIO5 避开 SPI (13/15) 和 GPIO0/3 (启动/串口)
//   - GPIO5 是 ESP8266 的 SCK 引脚, 但我们不使用 ESP8266 SPI 总线
#define MAX30102_SDA_PIN    2
#define MAX30102_SCL_PIN    5

// ========== 报警阈值 (生理参数) ==========
#define SPO2_LOW             94     // SpO2 < 94% 报警
#define HR_LOW               50     // 心率 < 50 报警
#define HR_HIGH              120    // 心率 > 120 报警

// 血压估算开关 (v2.2.0 启用)
#ifndef BP_ESTIMATE_ENABLED
#define BP_ESTIMATE_ENABLED  1
#endif

#endif // MAX30102_CONFIG_H
