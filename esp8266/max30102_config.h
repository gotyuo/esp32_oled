#ifndef MAX30102_CONFIG_H
#define MAX30102_CONFIG_H

// ========== MAX30102 引脚 (I2C) ==========
// 避开 ESP8266 硬件 SPI 的 GPIO13/14/15 和 OLED 的 GPIO5
#define MAX30102_SDA_PIN    2
#define MAX30102_SCL_PIN    1

// ========== 报警阈值 (生理参数) ==========
#define SPO2_LOW             94     // SpO2 < 94% 报警
#define HR_LOW               50     // 心率 < 50 报警
#define HR_HIGH              120    // 心率 > 120 报警
// 血压 (mmHg)
#define SBP_LOW              90     // 收缩压 < 90 报警 (低血压)
#define SBP_HIGH             150    // 收缩压 > 150 报警 (高血压)
#define DBP_LOW              50     // 舒张压 < 50 报警
#define DBP_HIGH             100    // 舒张压 > 100 报警

// 血压估算开关 (v2.2.0 启用)
#define BP_ESTIMATE_ENABLED  1

#endif // MAX30102_CONFIG_H
