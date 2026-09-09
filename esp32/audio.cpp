/**
 * =============================================================================
 * EnvMon ESP32 - 音频系统驱动 (audio.cpp)
 * =============================================================================
 *
 * 硬件: I2S DAC 喇叭 (如 MAX98357A / 板载 I2S DAC)
 * 引脚: DOUT=GPIO26, BCLK=GPIO25, LRC=GPIO27 (备用)
 * 库  : I2S 驱动 + AudioFS (测试音) + Tone 兼容接口
 *
 * 提供:
 *   - audioInit()         初始化 I2S DAC
 *   - audioAlarm(enable)  报警音开关 (周期性蜂鸣)
 *   - audioPlayTestTone() 开机自检测试音
 *   - audioMute()/Unmute()/IsMuted() 静音控制
 *
 * 实现说明:
 *   - 报警音通过 I2S DAC 输出方波/正弦波, 使用 Tone() 兼容语义
 *     (Tone(freq, duration) / noTone())。ESP32 的内置 Tone() 仅支持
 *     特定 GPIO, 故本模块自行实现 I2S 版本的 tone 输出。
 *   - 静音通过全局标志位控制, 不销毁通道 (避免重启 I2S 抖动)。
 *
 * 版本: v2.0.0
 * =============================================================================
 */

#include "envmon_esp32.h"

#include <driver/i2s.h>

// =============================================================================
// 音频参数
// =============================================================================

#define AUDIO_SAMPLE_RATE    16000   // 采样率 (Hz)
#define AUDIO_BITS_PER_SAMPLE 16     // 位深
#define AUDIO_PORT           I2S_NUM_1   // 使用 I2S_NUM_1, 与麦克风 (NUM_0) 区分

// 报警音参数
#define ALARM_FREQ_HZ        1000   // 报警音调 (Hz)
#define ALARM_DUTY_MS        1000   // 响 1 秒
#define ALARM_GAP_MS         500    // 停 0.5 秒
#define ALARM_VOLUME         0.5    // 音量 (0.0-1.0)

// 测试音参数
#define TEST_FREQ_HZ         440    // 440Hz 标准音
#define TEST_DURATION_MS     1500

// 报警音波形生成用的相位增量 (单声道, 32767 满幅正弦)
#define WAVE_PEAK            30000  // 振幅上限 (避免削波)

// =============================================================================
// 模块内部状态
// =============================================================================
namespace {
  bool     s_inited       = false;   // I2S 初始化成功
  bool     s_muted        = false;   // 静音标志
  bool     s_alarmActive  = false;   // 报警音激活标志

  // 报警音时序
  uint32_t s_alarmPhaseMs = 0;       // 当前处于 响/停 阶段的计时起点
  bool     s_alarmToneOn  = false;   // 当前阶段是否发声

  // 测试音时序
  uint32_t s_testStartMs  = 0;
  bool     s_testActive   = false;

  // 生成正弦波用的相位累加器
  float    s_phase        = 0.0f;
  float    s_phaseInc     = 0.0f;
}

// =============================================================================
// 内部: 正弦波生成 (用于报警音与测试音)
//
// 使用相位累加器生成连续正弦, 避免查表开销。
// =============================================================================
static void toneSetFreq(int hz) {
  s_phaseInc = (2.0f * (float)PI * (float)hz) / (float)AUDIO_SAMPLE_RATE;
  if (s_phaseInc > 0.5f) s_phaseInc = 0.5f;  // 防止过采样混叠
  s_phase = 0.0f;
}

static int16_t toneNextSample() {
  s_phase += s_phaseInc;
  if (s_phase >= 2.0f * (float)PI) s_phase -= 2.0f * (float)PI;
  float sample = sinf(s_phase) * WAVE_PEAK * ALARM_VOLUME;
  return (int16_t)sample;
}

// =============================================================================
// 内部: I2S DAC 通道管理
// =============================================================================
static bool audioSetupChannel() {
  // 避免重复初始化
  i2s_del_channel(s_inited ? (i2s_port_t)AUDIO_PORT : (i2s_port_t)-1);

  i2s_config_t cfg = {};
  cfg.rx_mode      = I2S_MODE_DISABLED;
  cfg.tx_mode      = I2S_MODE_MONO;
  cfg.use_apll     = false;
  cfg.duplex_mode  = I2S_MODE_TX;
  cfg.i2s_port     = (i2s_port_t)AUDIO_PORT;
  cfg.sample_rate  = AUDIO_SAMPLE_RATE;
  cfg.bits_per_sample = (i2s_bit_width_t)AUDIO_BITS_PER_SAMPLE;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.comm_format    = I2S_COMM_FORMAT_I2S;
  cfg.intr_alloc     = I2S_INT_ALLOC_MIN;
  cfg.auto_clear     = false;

  esp_err_t err = i2s_init_channel(&cfg);
  if (err != ESP_OK) {
    ELOG("Audio I2S init failed: %s", esp_err_to_name(err));
    return false;
  }

  i2s_pin_config_t pin = {};
  pin.bclk_io_num   = SPK_BCLK_PIN;
  pin.ws_io_num     = SPK_LRC_PIN;
  pin.data_out_num  = SPK_DOUT_PIN;
  pin.data_in_num   = I2S_PIN_NO_CHANGE;

  err = i2s_set_pin((i2s_port_t)AUDIO_PORT, &pin);
  if (err != ESP_OK) {
    ELOG("Audio I2S pin config failed: %s", esp_err_to_name(err));
    return false;
  }

  err = i2s_channel_enable((i2s_port_t)AUDIO_PORT, false);
  if (err != ESP_OK) {
    ELOG("Audio I2S enable failed: %s", esp_err_to_name(err));
    return false;
  }

  ELOG("Audio I2S DAC init OK (DOUT=%d BCLK=%d LRC=%d)",
       SPK_DOUT_PIN, SPK_BCLK_PIN, SPK_LRC_PIN);
  return true;
}

/**
 * @brief 持续填充 I2S 缓冲, 输出当前应发声的波形或静音 (0)
 *
 * 在后台任务中调用, 保证 I2S 缓冲不欠载 (避免爆音)。
 */
static void audioWriteBuffer(size_t bufSamples, int16_t** buf,
                             volatile bool* stopped) {
  size_t written = 0;
  while (*stopped == false) {
    int16_t* out = buf[written % 2];
    // 判断当前是否应发声
    bool shouldTone = false;
    if (s_alarmActive && s_alarmToneOn && !s_muted) shouldTone = true;
    else if (s_testActive && !s_muted) shouldTone = true;

    for (size_t i = 0; i < bufSamples; i++) {
      out[i] = shouldTone ? toneNextSample() : 0;
    }

    size_t bytesW = 0;
    esp_err_t err = i2s_channel_write((i2s_port_t)AUDIO_PORT, out,
                                      bufSamples * sizeof(int16_t),
                                      &bytesW, portMAX_DELAY);
    if (err != ESP_OK) {
      ELOG("Audio write failed: %s", esp_err_to_name(err));
      return;
    }
    written++;
  }
}

// 后台音频任务句柄
static TaskHandle_t s_audioTask = nullptr;
static volatile bool s_audioTaskStop = false;

static void audioTaskFunc(void*) {
  // 双缓冲
  int16_t* bufA = (int16_t*)heap_caps_malloc(2048 * sizeof(int16_t),
                                             MALLOC_CAP_SPIRAM);
  int16_t* bufB = (int16_t*)heap_caps_malloc(2048 * sizeof(int16_t),
                                             MALLOC_CAP_SPIRAM);
  // SPIRAM 不足则退回堆
  if (bufA == nullptr) bufA = (int16_t*)malloc(2048 * sizeof(int16_t));
  if (bufB == nullptr) bufB = (int16_t*)malloc(2048 * sizeof(int16_t));
  if (bufA == nullptr || bufB == nullptr) {
    ELOG("Audio task: out of memory");
    free(bufA); free(bufB);
    return;
  }

  int16_t* buf[2] = { bufA, bufB };
  audioWriteBuffer(2048, buf, &s_audioTaskStop);

  free(bufA); free(bufB);
  s_audioTask = nullptr;  // 任务自清理后通知
  vTaskDelete(nullptr);
}

// =============================================================================
// 对外接口
// =============================================================================

/**
 * @brief 初始化音频系统
 *
 * @return true  初始化成功; false 失败
 *
 * 初始化内容: I2S DAC 通道 + 后台音频输出任务。
 * 失败时喇叭不可用, 但不阻塞其他功能。
 */
bool audioInit() {
  if (s_inited) {
    ELOG("Audio already initialized, skip");
    return true;
  }

  if (!audioSetupChannel()) {
    s_inited = false;
    return false;
  }

  toneSetFreq(ALARM_FREQ_HZ);  // 预设报警音频率

  // 启动后台音频任务 (优先级中等, 不抢占主循环)
  BaseType_t ok = xTaskCreatePinnedToCore(audioTaskFunc, "audio_tx",
      4096, nullptr, 3, &s_audioTask, 1);  // 核心 1, 避免与 WiFi 冲突
  if (ok != pdPASS) {
    ELOG("Audio task creation failed (noTask=%d)", ok);
    s_inited = false;
    return false;
  }

  s_inited = true;
  ELOG("Audio system ready");
  return true;
}

/**
 * @brief 开关报警音 (周期性蜂鸣)
 *
 * @param enable  true 开始/恢复报警音; false 停止
 *
 * 报警音以 "响 ALARM_DUTY_MS - 停 ALARM_GAP_MS" 循环,
 * 通过后台任务按时间片切换 s_alarmToneOn。
 * 静音时 (s_muted) 不发声, 但报警状态仍保留。
 */
void audioAlarm(bool enable) {
  if (!s_inited) {
    ELOG("audioAlarm: audio not initialized");
    return;
  }

  if (enable && !s_alarmActive) {
    s_alarmActive  = true;
    s_alarmPhaseMs = millis();
    s_alarmToneOn  = true;
    toneSetFreq(ALARM_FREQ_HZ);
    ELOG("Alarm tone ON (freq=%dHz)", ALARM_FREQ_HZ);
  } else if (!enable && s_alarmActive) {
    s_alarmActive = false;
    s_alarmToneOn = false;
    ELOG("Alarm tone OFF");
  }
}

/**
 * @brief 播放开机自检测试音
 *
 * 持续 TEST_DURATION_MS 毫秒的 TEST_FREQ_HZ 单音。
 * 期间不影响其他状态; 完成后自动停止。
 */
void audioPlayTestTone() {
  if (!s_inited) {
    ELOG("audioPlayTestTone: audio not initialized");
    return;
  }
  s_testActive  = true;
  s_testStartMs = millis();
  toneSetFreq(TEST_FREQ_HZ);
  ELOG("Test tone playing (%dHz, %ums)", TEST_FREQ_HZ, TEST_DURATION_MS);
}

/**
 * @brief 静音: 抑制所有声音输出, 但保留报警/测试状态
 */
void audioMute() {
  s_muted = true;
  ELOG("Audio MUTED");
}

/**
 * @brief 取消静音: 恢复声音输出
 */
void audioUnmute() {
  s_muted = false;
  ELOG("Audio UNMUTED");
}

/**
 * @brief 查询当前是否静音
 */
bool audioIsMuted() {
  return s_muted;
}

/**
 * @brief 音频调度更新 (由主循环周期调用)
 *
 * 处理:
 *   - 报警音的 响/停 相位切换
 *   - 测试音超时自动停止
 *
 * 此函数需在每个主循环调用, 以保持后台任务状态与当前时刻一致。
 */
void audioUpdate() {
  if (!s_inited) return;

  // 报警音相位切换
  if (s_alarmActive) {
    uint32_t now = millis();
    uint32_t elapsed = now - s_alarmPhaseMs;
    if (s_alarmToneOn && elapsed >= ALARM_DUTY_MS) {
      s_alarmToneOn = false;       // 进入停顿阶段
      s_alarmPhaseMs = now;
    } else if (!s_alarmToneOn && elapsed >= ALARM_GAP_MS) {
      s_alarmToneOn = true;        // 进入发声阶段
      s_alarmPhaseMs = now;
    }
  }

  // 测试音超时
  if (s_testActive) {
    uint32_t elapsed = millis() - s_testStartMs;
    if (elapsed >= TEST_DURATION_MS) {
      s_testActive = false;
      toneSetFreq(ALARM_FREQ_HZ);   // 恢复默认频率
      ELOG("Test tone finished");
    }
  }
}

/**
 * @brief 停止音频系统 (可选, 用于 OTA 前释放资源)
 */
void audioShutdown() {
  if (!s_inited) return;
  s_alarmActive = false;
  s_testActive  = false;
  s_audioTaskStop = true;
  // 等待任务退出 (最多 500ms)
  uint32_t start = millis();
  while (s_audioTask != nullptr && (millis() - start) < 500) {
    delay(10);
  }
  i2s_del_channel((i2s_port_t)AUDIO_PORT);
  s_inited = false;
  ELOG("Audio system shut down");
}
