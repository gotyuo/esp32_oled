/**
 * =============================================================================
 * EnvMon ESP32 - 心电监护占位驱动 (ekg_stub.cpp)
 * =============================================================================
 *
 * 状态: STUB (占位实现, 尚未接入真实 AD8232 硬件)
 *
 * 预留硬件:
 *   - EKG_PIN   = GPIO35  (ECG 模拟信号输入, ADC1_CH7)
 *   - EKG_REF_PIN = GPIO34 (参考信号, 占位)
 *
 * 提供:
 *   - ekgInitialize()  初始化 (设置 ADC, 当前仅打印日志)
 *   - ekgRead()        读取一次 ECG 采样值 (当前返回模拟正弦波)
 *   - ekgParse()       从原始采样提取幅值 (当前直通)
 *
 * 模拟数据说明:
 *   在真实 AD8232 接入前, ekgRead() 返回一段合成的类心电波形,
 *   便于联调显示 / 上报 / 报警链路。波形由正弦 + 基线漂移构成,
 *   幅值范围 0-1.0 mV (归一化)。
 *
 * 真实实现待办 (见 README.md 后续待办):
 *   - 接入 AD8232 (需要模拟前端供电与滤波电路)
 *   - 实现 QRS 波群检测 (R 峰)
 *   - 心率计算与基线漂移消除
 *   - ADC 采样率与抗混叠滤波
 *
 * 版本: v2.0.0
 * =============================================================================
 */

#include "envmon_esp32.h"

// =============================================================================
// 模块内部状态
// =============================================================================
namespace {
  bool   s_inited = false;       // 初始化标志
  bool   s_simulated = true;     // 当前为模拟模式 (非真实硬件)

  // 模拟波形生成状态
  uint32_t s_sampleCount = 0;    // 采样计数 (用于波形相位)
  const float s_samplePeriodSec = 0.001f;  // 1ms 采样周期 (1kHz)
  const float s_heartbeatPeriodSec = 0.8f; // 心率 ~75bpm (0.8s/拍)

  // 连续失败计数 (模拟模式下不会失败, 为真实实现预留)
  uint32_t s_failStreak = 0;
}

/**
 * @brief 生成模拟 ECG 波形采样
 *
 * 使用一个简化的 PQRST 模型:
 *   - 基线: 缓慢正弦漂移
 *   - QRS 峰: 周期性尖锐脉冲 (主波)
 *   - T 波: 较小缓峰
 *
 * 输出范围约 -0.1 ~ 1.0 mV (归一化到 mV 单位)。
 */
static float generateSimulatedEcg() {
  s_sampleCount++;

  // 归一化相位 (0.0 ~ 1.0, 一个心跳周期内)
  float t = (s_sampleCount * s_samplePeriodSec) / s_heartbeatPeriodSec;
  float phase = t - floorf(t);  // 取模

  float v = 0.0f;

  // 基线漂移 (低频正弦)
  v += 0.02f * sinf(2.0f * PI * (t * 0.2f));

  // P 波 (心房内除极, 小缓峰, 位于相位 0.1-0.2)
  if (phase >= 0.1f && phase <= 0.25f) {
    float p = (phase - 0.1f) / 0.15f;
    v += 0.05f * sinf(PI * p);
  }

  // QRS 波群 (主峰, 位于相位 0.30-0.42)
  if (phase >= 0.30f && phase <= 0.42f) {
    float q = (phase - 0.30f) / 0.12f;
    // Q 负向小下陷
    if (q < 0.2f) {
      v -= 0.08f * sinf(PI * (q / 0.2f));
    } else if (q < 0.6f) {
      // R 主峰 (尖峰)
      float r = (q - 0.2f) / 0.4f;
      v += 0.9f * sinf(PI * r);
    } else {
      // S 负向小下陷
      float s = (q - 0.6f) / 0.4f;
      v -= 0.1f * sinf(PI * s);
    }
  }

  // T 波 (心室复极, 缓峰, 位于相位 0.5-0.7)
  if (phase >= 0.5f && phase <= 0.75f) {
    float tr = (phase - 0.5f) / 0.25f;
    v += 0.15f * sinf(PI * tr);
  }

  // 加入微小噪声 (模拟真实信号)
  v += (analogReadRandom() & 0x7) * 0.001f;

  // 限幅
  if (v > 1.2f) v = 1.2f;
  if (v < -0.3f) v = -0.3f;
  return v;
}

// =============================================================================
// 对外接口
// =============================================================================

/**
 * @brief 初始化 ECG 模块
 *
 * @return true 初始化成功 (含模拟模式); false 真实硬件初始化失败
 *
 * 当前为 stub: 仅设置 ADC 引脚模式并打印状态。
 * 真实实现将接入 AD8232, 配置 ADC 采样率与模拟前端。
 */
bool ekgInitialize() {
  if (s_inited) {
    ELOG("ECG already initialized, skip");
    return true;
  }

  // 配置 ADC 输入引脚 (输入阻抗高, 模拟信号)
  analogReadResolution(12);  // 12-bit 分辨率
  analogSetPinAttenuation(EKG_PIN, ADC_11db);  // 较高量程, 适应心电小信号

  pinMode(EKG_PIN, INPUT);
  pinMode(EKG_REF_PIN, INPUT);

  s_inited = true;
  s_simulated = true;
  s_sampleCount = 0;

  ELOG("ECG init OK (STUB / simulated mode)");
  ELOG("  EKG_PIN=%d EKG_REF_PIN=%d  [real AD8232 not yet connected]",
       EKG_PIN, EKG_REF_PIN);
  ELOG("  NOTE: returning simulated waveform for integration testing");

  return true;
}

/**
 * @brief 读取一次 ECG 采样
 *
 * @return 采样值 (单位 mV), 模拟模式下为合成波形
 *
 * 失败时返回 0.0f 并递增失败计数。
 * 调用方需配合 ekgParse() 提取幅值/心率。
 *
 * 采样周期建议 1kHz (每 1ms 调用一次), 由主循环调度。
 */
float ekgRead() {
  if (!s_inited) {
    ELOG("ekgRead: ECG not initialized");
    return 0.0f;
  }

  float value;
  if (s_simulated) {
    value = generateSimulatedEcg();
    s_failStreak = 0;
  } else {
    // === 真实硬件实现 (待接入 AD8232) ===
    // AD8232 输出为单端模拟信号, 通过 GPIO35 的 ADC 读取
    // int raw = analogRead(EKG_PIN);          // 0-4095
    // value = mapFloat(raw, 0, 4095, -0.5f, 1.5f);  // 映射到 mV
    // ...
    // 当前未实现, 回退到模拟
    value = generateSimulatedEcg();
    ELOG("ekgRead: real impl not ready, using simulated");
  }

  return value;
}

/**
 * @brief 从原始采样提取幅值 (用于显示/报警)
 *
 * @param amplitude  输出: 当前幅值 (mV)
 *
 * @return true  成功提取; false 失败
 *
 * 当前 stub: 直接返回 ekgRead() 的瞬时值作为幅值。
 * 真实实现将进行:
 *   - 带通滤波 (0.5-40Hz, 心电有效频带)
 *   - R 峰检测 (阈值 + 导数判据)
 *   - 包络/峰值整流得到幅值
 */
bool ekgParse(float* amplitude) {
  if (!s_inited || amplitude == nullptr) {
    return false;
  }

  // 当前: 瞬时幅值 (绝对值)
  float v = ekgRead();
  *amplitude = (v < 0) ? -v : v;

  // 若幅值持续过低, 视为无信号 (电极脱落)
  static float s_lowStreak = 0.0f;
  if (*amplitude < 0.01f) {
    s_lowStreak += s_samplePeriodSec;
    if (s_lowStreak > 5.0f) {  // 5 秒无信号
      ELOG("ECG: no signal for >5s (lead off?)");
      *amplitude = 0.0f;
      return false;
    }
  } else {
    s_lowStreak = 0.0f;
  }

  return true;
}

/**
 * @brief 计算当前心率 (bpm) —— 供真实实现使用
 *
 * 当前 stub 返回设定值 (75bpm), 便于联调。
 * 真实实现将通过 R-R 间期计算。
 */
float ekgHeartRateBpm() {
  if (!s_inited) return 0.0f;
  return 60.0f / s_heartbeatPeriodSec;  // 模拟: ~75bpm
}

/**
 * @brief 查询是否处于模拟模式 (供诊断)
 */
bool ekgIsSimulated() {
  return s_simulated;
}

/**
 * @brief 查询初始化状态
 */
bool ekgIsInitialized() {
  return s_inited;
}
