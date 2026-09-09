/*
 * 心电监护占位驱动 (ESP32)
 * 
 * 预留引脚:
 * - ADC: GPIO34 (ADC1_CH6, 与麦克风共用)
 * - REF: GPIO35 (ADC1_CH7)
 * 
 * 后期接入 AD8232 心电监护模块时实现
 * 
 * 当前状态: 返回模拟数据，用于测试和占位
 */

#include "Arduino.h"
#include "envmon_esp32.h"

bool ekgInitialized = false;

// 模拟心率数据
float simulatedBPM = 72.0;
unsigned long lastECGSample = 0;

// ECG 采样数据结构
struct ECGSample {
  int adc_value;      // ADC 原始值
  float bpm;          // 心率
  bool valid;         // 数据有效
  unsigned long timestamp_ms;
};

// 全局 ECG 状态
ECGSample currentECG = {0, 0, false, 0};

void ekgInit() {
  if (!EKG_ENABLE) {
    Serial.println("[EKG] 未启用，跳过初始化");
    ekgInitialized = false;
    return;
  }
  
  // 配置 ADC 引脚
  pinMode(EKG_ADC_PIN, INPUT);
  pinMode(EKG_REF_PIN, INPUT);
  
  // 设置 ADC 衰减
  analogSetAttenuation(ADC_11db);
  
  // 设置 ADC 分辨率 (12 位)
  analogSetWidth(12);
  
  ekgInitialized = true;
  Serial.println("[EKG] 初始化成功 (占位)");
}

// 读取 ECG 采样值
int ekgRead() {
  if (!EKG_ENABLE || !ekgInitialized) {
    // 返回模拟数据
    return generateSimulatedECG();
  }
  
  // 实际读取 ADC
  int value = analogRead(EKG_ADC_PIN);
  
  currentECG.adc_value = value;
  currentECG.timestamp_ms = millis();
  currentECG.valid = true;
  
  return value;
}

// 获取当前心率
float ekgGetBPM() {
  if (!EKG_ENABLE || !ekgInitialized) {
    return simulatedBPM;
  }
  
  // 实际计算需要多个采样点
  // 这里返回模拟值
  return simulatedBPM;
}

// 获取 ECG 采样数据
ECGSample ekgGetCurrent() {
  return currentECG;
}

// 生成模拟 ECG 数据
int generateSimulatedECG() {
  // 模拟 PQRST 波形
  unsigned long now = millis();
  unsigned long period = 60000.0 / simulatedBPM;  // 心跳周期 (毫秒)
  
  unsigned long phase = (now % period) * 1.0 / period;  // 0.0 - 1.0
  
  int value = 0;
  
  if (phase < 0.1) {
    // P 波
    value = 2048 + 200 * sin(phase * 3.14 / 0.1);
  } else if (phase < 0.15) {
    // Q 波
    value = 2048 - 300 * sin((phase - 0.1) * 3.14 / 0.05);
  } else if (phase < 0.2) {
    // R 波 (尖峰)
    value = 2048 + 800 * sin((phase - 0.15) * 3.14 / 0.05);
  } else if (phase < 0.25) {
    // S 波
    value = 2048 - 400 * sin((phase - 0.2) * 3.14 / 0.05);
  } else if (phase < 0.4) {
    // T 波
    value = 2048 + 150 * sin((phase - 0.25) * 3.14 / 0.15);
  } else {
    // 基线
    value = 2048;
  }
  
  currentECG.adc_value = value;
  currentECG.bpm = simulatedBPM;
  currentECG.timestamp_ms = now;
  currentECG.valid = true;
  
  return value;
}

// 设置模拟心率 (用于测试)
void ekgSetSimulatedBPM(float bpm) {
  simulatedBPM = bpm;
  Serial.printf("[EKG] 模拟心率: %.1f BPM\n", bpm);
}

// 重置 ECG 状态
void ekgReset() {
  currentECG = {0, 0, false, 0};
  Serial.println("[EKG] 状态已重置");
}

// 打印 ECG 状态
void ekgPrintStatus() {
  Serial.printf("[EKG] 启用: %s\n", EKG_ENABLE ? "是" : "否");
  Serial.printf("       初始化: %s\n", ekgInitialized ? "是" : "否");
  Serial.printf("       当前心率: %.1f BPM\n", simulatedBPM);
  Serial.printf("       最新采样: %d\n", currentECG.adc_value);
  Serial.printf("       时间戳: %lu ms\n", currentECG.timestamp_ms);
}
