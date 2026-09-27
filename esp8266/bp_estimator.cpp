/*
 * EnvMon ESP8266 - 血压估算 (v2.2.0)
 *
 * 重要说明:
 *   MAX30102 不能直接测量血压 (它是 PPG 传感器, 不含压力传感)。
 *   本文件基于 PPG 波形特征 + 心率经验公式估算血压, 属于无创估算 (cuffless)。
 *   精度: ±10-15 mmHg (单点), 不适合临床诊断, 仅作辅助参考。
 *
 * 算法思路 (简化版, 参考 Maxim / DHT 论文):
 *   1. 在 IR 通道 PPG 信号上做形态学分析
 *   2. 提取收缩峰、舒张峰、脉搏波宽度 (PWV proxy)
 *   3. 用经验公式结合心率计算 SBP/DBP
 *
 * 参考:
 *   - 刘乔寿, 王森. 基于单路 PPG 信号的连续血压检测算法设计. 电子设计工程, 2019, 27(1): 63-69.
 *     https://gitee.com/NaN01/heart-rate-sp-o2-analyzer
 *   - Zeitzman et al. "Blood pressure monitoring by way of photoplethysmography."
 *     J Biomech Eng 2014.
 *   - PMC4512231: Cuff-Free BP using PTT and Heart Rate
 *
 * 版本: v2.2.0
 */

#include <Arduino.h>

#include "envmon_esp8266.h"
#include "bp_calibration.h"

// ========== 经验公式参数 (可调) ==========
// 基线值 (健康成人静息参考)
#define BASELINE_SBP           120    // mmHg
#define BASELINE_DBP           80     // mmHg
#define BASELINE_HR            75     // bpm

// 形态学系数 (来自 Zeitzman 2014 单路 PPG 简化)
#define SBP_PEAK_COEFF         0.15   // 收缩峰幅值贡献
#define SBP_SLOPE_COEFF        0.50   // 上升斜率贡献
#define DBP_SHAPE_COEFF        0.30   // 形态因子贡献
#define DBP_HR_COEFF           0.4    // 心率敏感度 (每偏离基线心率 1 bpm 调整量)

// 平滑与阈值
#define BP_VALID_MIN_HR        50     // 心率低于此值, BP 估算不可靠
#define BP_VALID_MAX_HR        150    // 心率高于此值, BP 估算不可靠
#define BP_MAX_DRIFT           30     // 单次估算与上次差异超过此值视为异常, 丢弃

// ========== 形态学特征 ==========
struct PpgFeature {
  float peak_amplitude;   // 峰值幅值 (相对 AC 振幅, 0-1 归一化)
  float rise_slope;       // 收缩期上升斜率 (相对归一化)
  float dicrotic_pos;     // 舒张峰位置 (相对于主峰, 0-1 归一化)
  float pwv_proxy;        // 脉搏波宽度代理 (主峰到舒张峰的距离, 归一化)
  bool valid;
};

static PpgFeature lastFeature;
static float lastSbp = 0, lastDbp = 0;
static bool lastBpValid = false;

// ========== PPG 形态学分析 ==========
/*
 * 输入: IR 通道原始 PPG 波形 (100 样本)
 * 输出: 提取形态学特征
 */
bool ppgFeatureExtract(const uint32_t* irWave, int len, PpgFeature* out) {
  if (len < 10) { out->valid = false; return false; }

  // Step 1: 减去 DC, 保留 AC 分量
  int32_t dc = 0;
  for (int i = 0; i < len; i++) dc += irWave[i];
  dc /= len;

  // 找到 AC 振幅 (用于归一化)
  int32_t ac_min = irWave[0], ac_max = irWave[0];
  for (int i = 1; i < len; i++) {
    if (irWave[i] < ac_min) ac_min = irWave[i];
    if (irWave[i] > ac_max) ac_max = irWave[i];
  }
  int32_t ac_amp = (ac_max - ac_min) / 2;
  if (ac_amp < 100) { out->valid = false; return false; }  // 信号太弱

  // Step 2: 找主收缩峰位置
  int peak_idx = 0;
  int32_t peak_val = irWave[0] - dc;
  for (int i = 1; i < len; i++) {
    int32_t v = irWave[i] - dc;
    if (v > peak_val) {
      peak_val = v;
      peak_idx = i;
    }
  }

  // Step 3: 计算上升斜率 (收缩峰前 5 样本的梯度)
  int slope_start = peak_idx - 5;
  if (slope_start < 0) slope_start = 0;
  int slope_end = peak_idx;
  if (slope_end <= slope_start) { out->valid = false; return false; }

  int32_t rise_val = 0;
  for (int i = slope_start; i < slope_end; i++) {
    rise_val += (irWave[i+1] - irWave[i]);
  }
  rise_val /= (slope_end - slope_start);

  // Step 4: 找舒张峰 (dicrotic notch 后的次峰, 在主峰后)
  int dicrotic_idx = -1;
  int32_t dicrotic_val = 0;
  for (int i = peak_idx + 3; i < len; i++) {
    int32_t v = irWave[i] - dc;
    if (v > dicrotic_val && v < peak_val * 0.8) {  // 舒张峰通常低于主峰的 80%
      dicrotic_val = v;
      dicrotic_idx = i;
    }
  }

  // Step 5: 计算特征 (归一化)
  out->peak_amplitude = (float)peak_val / (float)ac_amp;
  if (out->peak_amplitude > 1.5f) out->peak_amplitude = 1.5f;  // 截断

  out->rise_slope = (float)rise_val / (float)ac_amp;
  if (out->rise_slope > 2.0f) out->rise_slope = 2.0f;

  if (dicrotic_idx > peak_idx) {
    out->dicrotic_pos = (float)(dicrotic_idx - peak_idx) / (float)len;
    out->pwv_proxy = (float)(dicrotic_idx - peak_idx) / (float)len;
  } else {
    out->dicrotic_pos = 0.3f;  // 默认值
    out->pwv_proxy = 0.15f;    // 默认值
  }

  out->valid = true;
  return true;
}

// ========== 血压估算 (v2.2.0 全局基线) ==========
/*
 * 基于 PPG 形态学 + 心率经验公式估算血压
 * 返回: true 估算有效, false 不可靠
 */
bool estimateBpCalibrated(const PpgFeature& feature, float heart_rate,
                          float b_amp, float b_slope, float b_pwv,
                          float* sbp, float* dbp);  // 前向声明 (v2.3.0)

bool estimateBp(const PpgFeature& feature, float heart_rate,
                float* sbp, float* dbp) {
  return estimateBpCalibrated(feature, heart_rate,
                              1.0f, 1.0f, 0.15f, sbp, dbp);
}

// ========== 血压估算 (v2.3.0 个体基线) ==========
/*
 * 同 estimateBp, 但用个体基线 (b_amp/b_slope/b_pwv) 替代全局默认值
 * (1.0, 1.0, 0.15). 个体基线由 bp_calibration 模块学习得到.
 */
bool estimateBpCalibrated(const PpgFeature& feature, float heart_rate,
                          float b_amp, float b_slope, float b_pwv,
                          float* sbp, float* dbp) {
  if (!feature.valid || heart_rate < BP_VALID_MIN_HR ||
      heart_rate > BP_VALID_MAX_HR) {
    *sbp = 0; *dbp = 0;
    return false;
  }

  // 防止除零 / NaN (v2.3.0 新增防御)
  if (b_amp < 0.5f)  b_amp   = 0.5f;
  if (b_slope < 0.5f) b_slope = 0.5f;
  if (b_pwv < 0.05f) b_pwv   = 0.05f;

  // Step 1: SBP 估算 (相对个体基线的偏移)
  float sbp_calc = BASELINE_SBP
      + SBP_PEAK_COEFF * (feature.peak_amplitude - b_amp) * 100.0f   // 峰值偏移
      + SBP_SLOPE_COEFF * (feature.rise_slope - b_slope) * 10.0f     // 上升斜率
      - DBP_HR_COEFF * (heart_rate - BASELINE_HR) * 0.5f;            // 心率调整

  // Step 2: DBP 估算
  float dbp_calc = BASELINE_DBP
      - DBP_SHAPE_COEFF * (feature.pwv_proxy - b_pwv) * 50.0f        // 脉搏波宽
      + DBP_HR_COEFF * (heart_rate - BASELINE_HR) * 0.3f;            // 心率调整

  // Step 3: 合理性截断 (正常血压范围)
  if (sbp_calc < 80) sbp_calc = 80;
  if (sbp_calc > 180) sbp_calc = 180;
  if (dbp_calc < 50) dbp_calc = 50;
  if (dbp_calc > 110) dbp_calc = 110;

  // 确保 SBP > DBP (生理约束)
  if (sbp_calc <= dbp_calc + 10) {
    sbp_calc = dbp_calc + 20;
  }

  // Step 4: 与上次比较, 抑制突变
  if (lastBpValid) {
    if (fabs(sbp_calc - lastSbp) > BP_MAX_DRIFT ||
        fabs(dbp_calc - lastDbp) > BP_MAX_DRIFT) {
      // 差异过大, 不更新 (保持上次值)
      *sbp = lastSbp; *dbp = lastDbp;
      return true;
    }
  }

  // Step 5: 保存
  lastSbp = sbp_calc;
  lastDbp = dbp_calc;
  lastBpValid = true;

  *sbp = sbp_calc;
  *dbp = dbp_calc;
  return true;
}

// ========== 主入口 (v2.3.0) ==========
/*
 * 在每次心率计算后调用
 * irWave: 100 样本 IR 通道 PPG 波形
 * heart_rate: 已算出的心率
 * base_peak_amp / base_slope / base_pwv: 个体基线 (NULL 时使用全局默认值)
 * 返回: 估算的 SBP/DBP
 *
 * v2.3.0: 每次估算成功后, 自动把本拍的形态学特征喂入 bp_calibration
 * 模块, 让其滚动累积个体基线. 这样校准无需调用方手动喂数据.
 */
void bpEstimateUpdate(const uint32_t* irWave, int waveLen,
                      float heart_rate,
                      float* sbp, float* dbp, bool* valid,
                      const float* base_peak_amp,
                      const float* base_slope,
                      const float* base_pwv) {
  *sbp = 0; *dbp = 0; *valid = false;

  PpgFeature feature;
  if (!ppgFeatureExtract(irWave, waveLen, &feature)) {
    return;
  }

  lastFeature = feature;

  // v2.3.0: 个体基线 (来自 bp_calibration 模块); NULL 时退回全局默认值
  float b_amp   = base_peak_amp ? *base_peak_amp : bpCalibGetPeakAmp();
  float b_slope = base_slope    ? *base_slope    : bpCalibGetSlope();
  float b_pwv   = base_pwv      ? *base_pwv      : bpCalibGetPwv();

  float sbp_val, dbp_val;
  if (estimateBpCalibrated(feature, heart_rate, b_amp, b_slope, b_pwv,
                            &sbp_val, &dbp_val)) {
    *sbp = sbp_val;
    *dbp = dbp_val;
    *valid = true;

    // v2.3.0: 自动把本拍特征喂入校准模块 (始终进行, 让校准在后台累积)
    BpCalibFeature cf = {
      feature.peak_amplitude,
      feature.rise_slope,
      feature.pwv_proxy
    };
    bpCalibFeedHeartbeat(&cf);

    Serial.printf("[BP] SBP=%.0f DBP=%.0f (HR=%.0f, peak=%.2f/%.2f, slope=%.2f/%.2f)%s\n",
                  sbp_val, dbp_val, heart_rate,
                  feature.peak_amplitude, b_amp,
                  feature.rise_slope, b_slope,
                  bpCalibIsCalibrated() ? " [calibrated]" : " [calibrating]");
  }
}

// 兼容旧签名 (v2.2.0 调用方): 无基线参数, 使用全局默认值
void bpEstimateUpdate(const uint32_t* irWave, int waveLen,
                      float heart_rate,
                      float* sbp, float* dbp, bool* valid) {
  bpEstimateUpdate(irWave, waveLen, heart_rate, sbp, dbp, valid,
                   nullptr, nullptr, nullptr);
}

// ========== 诊断 ==========
float bpEstimateGetSbp() { return lastSbp; }
float bpEstimateGetDbp() { return lastDbp; }
bool bpEstimateIsValid() { return lastBpValid; }
