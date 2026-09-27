/*
 * EnvMon ESP8266 - 血压估算校准 (v2.3.0) 实现
 *
 * 核心算法: 移动平均 (rolling mean)
 *   在固定大小缓冲区中维护最近 N 拍的 PPG 特征,
 *   每次喂入一拍就重新计算移动平均作为个体基线.
 *   时间复杂度 O(N), N=12, 单拍处理 < 5 μs.
 */

#include <Arduino.h>

#include "bp_calibration.h"
#include "envmon_esp8266.h"   // for FIRMWARE_VERSION string

// ========== 静态数据 ==========
static BpCalibState s_state = {0};

// 滚动缓冲区: 存储最近 BP_CALIB_BUFFER 拍的特征
static BpCalibFeature s_buffer[BP_CALIB_BUFFER] = {0};

// 全局默认基线 (校准未完成时使用, 与 bp_estimator.cpp 的 BASELINE_* 对应)
static const float DEFAULT_PEAK_AMP  = 1.0f;
static const float DEFAULT_SLOPE     = 1.0f;
static const float DEFAULT_PWV       = 0.15f;

// ========== 实现 ==========

void bpCalibInit() {
  s_state.initialized = true;
  s_state.heartbeat_count = 0;
  s_state.calibrated = false;
  s_state.buffer_head = 0;
  s_state.buffer_len = 0;
  s_state.peak_amplitude_ma = DEFAULT_PEAK_AMP;
  s_state.rise_slope_ma = DEFAULT_SLOPE;
  s_state.pwv_proxy_ma = DEFAULT_PWV;
  s_state.calibrated_at = 0;
  s_state.initialized_at = millis();
}

void bpCalibReset() {
  bpCalibInit();
}

void bpCalibFeedHeartbeat(const BpCalibFeature* feature) {
  if (!s_state.initialized || feature == nullptr) {
    return;
  }

  // 写入滚动缓冲区
  s_buffer[s_state.buffer_head] = *feature;
  s_state.buffer_head = (s_state.buffer_head + 1) % BP_CALIB_BUFFER;
  if (s_state.buffer_len < BP_CALIB_BUFFER) {
    s_state.buffer_len++;
  }
  s_state.heartbeat_count++;

  // 重新计算移动平均
  float sum_amp = 0, sum_slope = 0, sum_pwv = 0;
  for (int i = 0; i < s_state.buffer_len; i++) {
    sum_amp   += s_buffer[i].peak_amplitude;
    sum_slope += s_buffer[i].rise_slope;
    sum_pwv   += s_buffer[i].pwv_proxy;
  }
  int n = s_state.buffer_len;
  s_state.peak_amplitude_ma = sum_amp   / (float)n;
  s_state.rise_slope_ma     = sum_slope / (float)n;
  s_state.pwv_proxy_ma      = sum_pwv   / (float)n;

  // 检查是否达到完成阈值
  if (!s_state.calibrated && s_state.buffer_len >= BP_CALIB_BUFFER) {
    s_state.calibrated = true;
    s_state.calibrated_at = millis();
    Serial.printf("[BP-CALIB] 校准完成 (用了 %d 拍): "
                  "peak=%.3f slope=%.3f pwv=%.3f\n",
                  s_state.buffer_len,
                  s_state.peak_amplitude_ma,
                  s_state.rise_slope_ma,
                  s_state.pwv_proxy_ma);
  }
}

float bpCalibGetPeakAmp() {
  return s_state.initialized ? s_state.peak_amplitude_ma : DEFAULT_PEAK_AMP;
}

float bpCalibGetSlope() {
  return s_state.initialized ? s_state.rise_slope_ma : DEFAULT_SLOPE;
}

float bpCalibGetPwv() {
  return s_state.initialized ? s_state.pwv_proxy_ma : DEFAULT_PWV;
}

bool bpCalibIsCalibrated() {
  return s_state.calibrated;
}

int bpCalibGetHeartbeatCount() {
  return s_state.heartbeat_count;
}

BpCalibPhase bpCalibGetPhase() {
  if (!s_state.initialized) return BP_CALIB_INITIALIZING;
  if (s_state.calibrated)   return BP_CALIB_READY;
  if (s_state.heartbeat_count > 0) return BP_CALIB_COLLECTING;
  return BP_CALIB_INITIALIZING;
}

void bpCalibPrintStatus() {
  Serial.printf("[BP-CALIB] v%s | initialized=%s count=%d/%d calibrated=%s\n",
                FIRMWARE_VERSION,
                s_state.initialized ? "y" : "n",
                s_state.heartbeat_count,
                BP_CALIB_BUFFER,
                s_state.calibrated ? "y" : "n");
  if (s_state.calibrated) {
    Serial.printf("[BP-CALIB]   baseline: peak=%.3f slope=%.3f pwv=%.3f\n",
                  s_state.peak_amplitude_ma,
                  s_state.rise_slope_ma,
                  s_state.pwv_proxy_ma);
    Serial.printf("[BP-CALIB]   calibrated_at=%lu ms (%.1fs after init)\n",
                  (unsigned long)s_state.calibrated_at,
                  (s_state.calibrated_at - s_state.initialized_at) / 1000.0f);
  }
}
