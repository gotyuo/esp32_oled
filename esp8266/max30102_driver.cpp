/*
 * EnvMon ESP8266 - MAX30102 血氧/心率传感器驱动
 *
 * 实现要点:
 *   - 直接使用 Wire.h 操作 MAX30102 寄存器 (不依赖外部库, 减少 ESP8266 内存占用)
 *   - SpO2 模式: RED+IR LED 同时启用, FIFO 每样本 6 字节 (3 字节 RED + 3 字节 IR)
 *   - 非阻塞 tick(): 主循环轮询 FIFO, 累积到滚动缓冲, 满 100 样本调用算法
 *   - Maxim 官方算法 (峰值检测 + 比值法) 简化实现
 *
 * 引脚: SDA=GPIO2, SCL=GPIO5
 * I2C 地址: 0x57
 *
 * 寄存器地址参考: Maxim MAX30102 datasheet (2015 v1.3)
 * 算法参考: Maxim MAXREFDES117# (BSD)
 *
 * 版本: v2.1.0
 */

#include <Arduino.h>
#include <Wire.h>

#include "envmon_esp8266.h"

// ========== MAX30102 配置 ==========
#define MAX30102_SDA_PIN    2     // GPIO2 (D1)
#define MAX30102_SCL_PIN    1     // GPIO1 (D10)
#define MAX30102_I2C_ADDR   0x57  // 7-bit I2C 地址

// 寄存器地址 (根据 Maxim MAX30102 datasheet 2015 v1.3)
#define REG_INT_STATUS_1    0x00
#define REG_INT_STATUS_2    0x01
#define REG_INT_STATUS_3    0x02
#define REG_FIFO_WR_PTR     0x04
#define REG_OVF_COUNTER     0x05
#define REG_FIFO_RD_PTR     0x06
#define REG_FIFO_DATA       0x07
#define REG_FIFO_CONFIG     0x08
#define REG_MODE_CONFIG     0x09
#define REG_SPO2_CONFIG     0x0A
#define REG_LED1_PA         0x0C  // RED LED 电流
#define REG_LED2_PA         0x0D  // IR LED 电流
#define REG_LED3_PA         0x0E  // GREEN LED 电流 (MAX30102 无)
#define REG_LED4_PA         0x0F  // PROX LED 电流
#define REG_PILOT_PA        0x10
#define REG_PART_ID         0xFF

// 模式 (MODE_CONFIG 位域)
#define MODE_RESET          0x40
#define MODE_SPO2           0x03    // RED + IR 双 LED 模式
#define MODE_MAX_HR         0x04    // 单 LED (MAX) 心率模式

// 采样配置 (SPO2_CONFIG 位域)
#define SPO2_RATE_100       0x1E
#define PULSEWIDTH_411      0x07    // 编码后

// 采样参数 (适中配置)
#define LED_BRIGHTNESS      60      // LED 电流 (0-255)
#define SAMPLE_AVERAGE      3       // FIFO_CONFIG SMP_AVE[2:0] = 3 -> 4 samples avg
#define FIFO_A_FULL         6       // FIFO_CONFIG FIFO_A_FULL[3:0] = 6
#define SAMPLE_RATE         SPO2_RATE_100
#define PULSE_WIDTH         PULSEWIDTH_411

// 缓冲与算法参数
#define SPO2_BUFFER_SIZE        100  // 100 样本 ≈ 4 秒 @ 25Hz
#define SPO2_COLLECT_INTERVAL   5000 // 每 5 秒重新计算一次

// ========== Maxim MAXREFDES117# 算法核心 ==========
// 原始算法见: https://github.com/sparkfun/SparkFun_MAX3010x_Sensor_Library/blob/master/src/spo2_algorithm.h
// 此文件实现简化版: 峰值检测 + 比值法

// SpO2 查找表 (近似公式: -45.060*ratio^2 + 30.354*ratio + 94.845)
static const uint8_t uch_spo2_table[184] = {
    95, 95, 95, 96, 96, 96, 97, 97, 97, 97, 97, 98, 98, 98, 98, 98,
    99, 99, 99, 99, 99, 99, 99, 99, 100, 100, 100, 100, 100, 100, 100, 100,
    100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 99, 99,
    99, 99, 99, 99, 99, 99, 98, 98, 98, 98, 98, 98, 97, 97, 97, 97,
    96, 96, 96, 96, 95, 95, 95, 94, 94, 94, 93, 93, 93, 92, 92, 92,
    91, 91, 90, 90, 89, 89, 89, 88, 88, 87, 87, 86, 86, 85, 85, 84,
    84, 83, 82, 82, 81, 81, 80, 80, 79, 78, 78, 77, 76, 76, 75, 74,
    74, 73, 72, 72, 71, 70, 69, 69, 68, 67, 66, 66, 65, 64, 63, 62,
    62, 61, 60, 59, 58, 57, 56, 56, 55, 54, 53, 52, 51, 50, 49, 48,
    47, 46, 45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33, 31,
    30, 29, 28, 27, 26, 25, 23, 22, 21, 20, 19, 17, 16, 15, 14, 12,
    11, 10, 9, 7, 6, 5, 3, 2, 1, 0
};

// ========== 状态 ==========
static uint32_t irBuffer[SPO2_BUFFER_SIZE];
static uint32_t redBuffer[SPO2_BUFFER_SIZE];
static int32_t  bufferIdx = 0;

static int32_t  lastSpo2 = 0;
static int8_t   lastSpo2Valid = 0;
static int32_t  lastHeartRate = 0;
static int8_t   lastHRValid = 0;

static unsigned long lastCalcTime = 0;
static bool initialized = false;

// ========== I2C 底层 ==========
static uint8_t max30102ReadReg(uint8_t reg) {
  Wire.beginTransmission(MAX30102_I2C_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  uint8_t val = 0;
  Wire.requestFrom((int)MAX30102_I2C_ADDR, 1);
  if (Wire.available()) val = Wire.read();
  return val;
}

static void max30102WriteReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MAX30102_I2C_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static bool max30102Probe() {
  Wire.beginTransmission(MAX30102_I2C_ADDR);
  uint8_t err = Wire.endTransmission();
  return (err == 0);
}

// ========== 初始化 ==========
bool max30102Init() {
  Serial.println("[MAX30102] 初始化...");

  Wire.begin(MAX30102_SDA_PIN, MAX30102_SCL_PIN);
  Wire.setClock(400000);

  if (!max30102Probe()) {
    Serial.println("[MAX30102] !! I2C 探测失败 (地址 0x57 未响应)");
    Serial.println("[MAX30102] 请检查:");
    Serial.println("[MAX30102]   1. VCC=3.3V 或 5V 模块, GND 已连接");
    Serial.println("[MAX30102]   2. SDA=GPIO2, SCL=GPIO5 接线正确");
    Serial.println("[MAX30102]   3. SDA/SCL 需 4.7kΩ 上拉到 3.3V");
    Serial.println("[MAX30102]   4. 模块可能需要 5V 供电 (LED 部分)");
    return false;
  }

  // 软复位 (MODE_CONFIG bit 6)
  max30102WriteReg(REG_MODE_CONFIG, MODE_RESET);
  delay(50);

  // 清空中断状态
  max30102WriteReg(REG_INT_STATUS_1, 0x00);
  max30102WriteReg(REG_INT_STATUS_2, 0x00);
  max30102WriteReg(REG_INT_STATUS_3, 0x00);

  // 清空 FIFO 指针
  max30102WriteReg(REG_FIFO_WR_PTR, 0x00);
  max30102WriteReg(REG_OVF_COUNTER, 0x00);
  max30102WriteReg(REG_FIFO_RD_PTR, 0x00);

  // FIFO 配置: 4 样本平均 + rollover + A_FULL=6
  uint8_t fifo_cfg = ((SAMPLE_AVERAGE & 0x07) << 5) | (1 << 4) | (FIFO_A_FULL & 0x0F);
  max30102WriteReg(REG_FIFO_CONFIG, fifo_cfg);

  // SpO2 模式 (RED + IR)
  max30102WriteReg(REG_MODE_CONFIG, MODE_SPO2);

  // SpO2 配置: 采样率 + 脉宽
  max30102WriteReg(REG_SPO2_CONFIG,
      (SAMPLE_RATE & 0x1F) |
      ((PULSE_WIDTH << 5) & 0xE0));

  // LED 电流配置 (RED + IR)
  max30102WriteReg(REG_LED1_PA, LED_BRIGHTNESS);
  max30102WriteReg(REG_LED2_PA, LED_BRIGHTNESS);

  // 再次清空 FIFO
  max30102WriteReg(REG_FIFO_WR_PTR, 0x00);
  max30102WriteReg(REG_FIFO_RD_PTR, 0x00);
  max30102WriteReg(REG_OVF_COUNTER, 0x00);

  bufferIdx = 0;
  lastSpo2Valid = 0;
  lastHRValid = 0;
  initialized = true;

  Serial.printf("[MAX30102] 初始化成功 (亮度=%d, 采样率=100Hz)\n", LED_BRIGHTNESS);
  return true;
}

// ========== 读取一个 FIFO 样本 (SpO2 模式: 3 字节 RED + 3 字节 IR) ==========
static bool max30102ReadSample(uint32_t* red, uint32_t* ir) {
  Wire.beginTransmission(MAX30102_I2C_ADDR);
  Wire.write(REG_FIFO_DATA);
  Wire.endTransmission(false);
  if (Wire.requestFrom((int)MAX30102_I2C_ADDR, 6) != 6) return false;

  uint8_t raw[6];
  for (int i = 0; i < 6; i++) raw[i] = Wire.read();

  // 18-bit ADC 值, 大端序
  *red = ((uint32_t)raw[0] << 16) | ((uint32_t)raw[1] << 8) | raw[2];
  *ir  = ((uint32_t)raw[3] << 16) | ((uint32_t)raw[4] << 8) | raw[5];
  return true;
}

// ========== Maxim MAXREFDES117# 算法: SpO2 + HR ==========
// 简化版实现: AC/DC 比值 + 峰值检测
static void maximAlgorithm(const uint32_t* irBuf, int irLen,
                           const uint32_t* redBuf,
                           int32_t* spo2, int8_t* spo2Valid,
                           int32_t* heartRate, int8_t* hrValid) {
  // Step 1: 计算 DC (直流分量)
  int32_t ir_dc = 0, red_dc = 0;
  for (int i = 0; i < irLen; i++) {
    ir_dc  += irBuf[i];
    red_dc += redBuf[i];
  }
  ir_dc  /= irLen;
  red_dc /= irLen;

  // Step 2: 计算 AC (交流分量) - 用峰值-谷值差
  int32_t ir_min = irBuf[0], ir_max = irBuf[0];
  int32_t red_min = redBuf[0], red_max = redBuf[0];
  for (int i = 1; i < irLen; i++) {
    if (irBuf[i] < ir_min) ir_min = irBuf[i];
    if (irBuf[i] > ir_max) ir_max = irBuf[i];
    if (redBuf[i] < red_min) red_min = redBuf[i];
    if (redBuf[i] > red_max) red_max = redBuf[i];
  }
  int32_t ir_ac  = (ir_max - ir_min) / 2;
  int32_t red_ac = (red_max - red_min) / 2;

  if (ir_dc <= 0 || ir_ac <= 0 || red_dc <= 0 || red_ac <= 0) {
    *spo2 = 0; *spo2Valid = 0;
    *heartRate = 0; *hrValid = 0;
    return;
  }

  // Step 3: 计算 SpO2 (比值法)
  // R = (AC_red / DC_red) / (AC_ir / DC_ir)
  float R_red = (float)red_ac / (float)red_dc;
  float R_ir  = (float)ir_ac  / (float)ir_dc;
  float ratio = R_red / R_ir;

  // 用查找表换算 (ratio * 183 = 表索引)
  int32_t idx = (int32_t)(ratio * 183.0f);
  if (idx < 0) idx = 0;
  if (idx >= 184) idx = 183;
  int32_t spo2_calc = uch_spo2_table[idx];

  if (spo2_calc < 70 || spo2_calc > 100) {
    *spo2 = 0; *spo2Valid = 0;
  } else {
    *spo2 = spo2_calc; *spo2Valid = 1;
  }

  // Step 4: 心率 - 通过 IR 信号峰值间隔估算
  // 简易峰值检测: 大于均值的局部极大值
  int32_t peakCount = 0;
  int32_t prevPeakIdx = -1;
  int32_t peakIntervalSum = 0;
  int32_t peakIntervalCount = 0;

  for (int i = 1; i < irLen - 1; i++) {
    int32_t v = irBuf[i] - ir_dc;
    if (v > ir_ac * 2 &&  // 高于 2*AC 振幅
        v > (int32_t)(irBuf[i-1] - ir_dc) &&
        v > (int32_t)(irBuf[i+1] - ir_dc)) {
      if (prevPeakIdx >= 0) {
        int32_t interval = i - prevPeakIdx;
        if (interval >= 2 && interval <= 20) {  // 滤波: 0.5-4 秒间隔
          peakIntervalSum += interval;
          peakIntervalCount++;
        }
      }
      prevPeakIdx = i;
      peakCount++;
    }
  }

  int32_t heartRate_calc = 0;
  if (peakIntervalCount > 0) {
    int32_t avgInterval = peakIntervalSum / peakIntervalCount;
    // 采样率 25Hz (算法内部降采样)
    // 心率 = 60 / (平均间隔 / 采样率) = 60 * 采样率 / 平均间隔
    heartRate_calc = (int32_t)((25.0f / (float)avgInterval) * 60.0f);
    if (heartRate_calc < 30 || heartRate_calc > 220) {
      *heartRate = 0; *hrValid = 0;
    } else {
      *heartRate = heartRate_calc; *hrValid = 1;
    }
  } else {
    *heartRate = 0; *hrValid = 0;
  }
}

// ========== 主循环 tick: 非阻塞采样 ==========
void max30102Tick() {
  if (!initialized) return;

  uint8_t wrPtr = max30102ReadReg(REG_FIFO_WR_PTR) & 0x1F;
  uint8_t rdPtr = max30102ReadReg(REG_FIFO_RD_PTR) & 0x1F;

  if (wrPtr != rdPtr) {
    int avail = (wrPtr - rdPtr + 32) % 32;
    for (int i = 0; i < avail; i++) {
      if (bufferIdx < SPO2_BUFFER_SIZE) {
        if (max30102ReadSample(&redBuffer[bufferIdx], &irBuffer[bufferIdx])) {
          bufferIdx++;
        }
      } else {
        break;
      }
    }
  }

  if (bufferIdx >= SPO2_BUFFER_SIZE) {
    if (millis() - lastCalcTime >= SPO2_COLLECT_INTERVAL) {
      maximAlgorithm(irBuffer, SPO2_BUFFER_SIZE, redBuffer,
                     &lastSpo2, &lastSpo2Valid,
                     &lastHeartRate, &lastHRValid);
      lastCalcTime = millis();
      bufferIdx = 0;

      if (lastSpo2Valid || lastHRValid) {
        Serial.printf("[MAX30102] SpO2=%d%s HR=%d%s\n",
                      lastSpo2, lastSpo2Valid ? "%" : "(invalid)",
                      lastHeartRate, lastHRValid ? "bpm" : "(invalid)");
      }
    }
  }
}

// ========== 读取结果 ==========
void max30102GetResult(float* spo2, float* hr, bool* valid) {
  *spo2 = lastSpo2Valid ? (float)lastSpo2 : 0.0f;
  *hr   = lastHRValid   ? (float)lastHeartRate : 0.0f;
  *valid = (lastSpo2Valid && lastHRValid);
}

bool max30102IsInitialized() { return initialized; }
int32_t  max30102GetBufferIndex() { return bufferIdx; }
