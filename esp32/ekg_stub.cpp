/**
 * =============================================================================
 * EnvMon ESP32 - 心电监护占位驱动 (ekg_stub.cpp)
 * =============================================================================
 *
 * 状态: STUB (占位实现, 尚未接入真实 AD8232 硬件)
 * EKG_PIN = -1, EKG_REF_PIN = -1 (ESP32-S3 上 GPIO34/35 不存在)
 *
 * 版本: v8.0.0
 * =============================================================================
 */

#include "envmon_esp32.h"

namespace {
  bool   s_inited = false;
  bool   s_simulated = true;
  uint32_t s_sampleCount = 0;
  const float s_samplePeriodSec = 0.001f;
  const float s_heartbeatPeriodSec = 0.8f;
  uint32_t s_failStreak = 0;
}

static float generateSimulatedEcg() {
  s_sampleCount++;
  float t = (s_sampleCount * s_samplePeriodSec) / s_heartbeatPeriodSec;
  float phase = t - floorf(t);

  float v = 0.0f;
  v += 0.02f * sinf(2.0f * PI * (t * 0.2f));

  // P 波
  if (phase >= 0.1f && phase <= 0.25f) {
    float p = (phase - 0.1f) / 0.15f;
    v += 0.05f * sinf(PI * p);
  }

  // QRS 波群
  if (phase >= 0.30f && phase <= 0.42f) {
    float q = (phase - 0.30f) / 0.12f;
    if (q < 0.2f) {
      v -= 0.08f * sinf(PI * (q / 0.2f));
    } else if (q < 0.6f) {
      float r = (q - 0.2f) / 0.4f;
      v += 0.9f * sinf(PI * r);
    } else {
      float s = (q - 0.6f) / 0.4f;
      v -= 0.1f * sinf(PI * s);
    }
  }

  // T 波
  if (phase >= 0.5f && phase <= 0.75f) {
    float tr = (phase - 0.5f) / 0.25f;
    v += 0.15f * sinf(PI * tr);
  }

  // 微小噪声 (使用 millis() 伪随机, ESP32 无 analogReadRandom())
  v += ((millis() & 0x7)) * 0.001f;

  if (v > 1.2f) v = 1.2f;
  if (v < -0.3f) v = -0.3f;
  return v;
}

bool ekgInitialize() {
  if (s_inited) {
    ELOG("ECG already initialized, skip");
    return true;
  }

  s_inited = true;
  s_simulated = true;
  s_sampleCount = 0;

  ELOG("ECG init OK (STUB / simulated mode)");
  ELOG("  EKG_PIN=%d EKG_REF_PIN=%d  [real AD8232 not yet connected]",
       EKG_PIN, EKG_REF_PIN);
  ELOG("  NOTE: returning simulated waveform for integration testing");
  return true;
}

float ekgRead() {
  if (!s_inited) {
    ELOG("ekgRead: ECG not initialized");
    return 0.0f;
  }
  return generateSimulatedEcg();
}

bool ekgParse(float* amplitude) {
  if (!s_inited || amplitude == nullptr) return false;

  float v = ekgRead();
  *amplitude = (v < 0) ? -v : v;

  static float s_lowStreak = 0.0f;
  if (*amplitude < 0.01f) {
    s_lowStreak += s_samplePeriodSec;
    if (s_lowStreak > 5.0f) {
      ELOG("ECG: no signal for >5s (lead off?)");
      *amplitude = 0.0f;
      return false;
    }
  } else {
    s_lowStreak = 0.0f;
  }
  return true;
}

float ekgHeartRateBpm() {
  if (!s_inited) return 0.0f;
  return 60.0f / s_heartbeatPeriodSec;
}

bool ekgIsSimulated() { return s_simulated; }
bool ekgIsInitialized() { return s_inited; }
