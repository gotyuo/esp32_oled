/*
 * EnvMon ESP8266 - 血压估算校准 (v2.3.0)
 *
 * 目的:
 *   MAX30102 PPG 信号的形态学特征 (peak_amplitude, rise_slope, pwv_proxy)
 *   因人而异, 且个体基线因人而异. 直接套用全局基线 (SBP=120, DBP=80, HR=75)
 *   会引入个体偏差, 通常 10-20 mmHg.
 *
 * 本模块在设备上自动学习个体基线:
 *   1. 在启动后一段时间内累积 PPG 形态学特征 (滚动缓冲区)
 *   2. 计算移动平均值作为个体基线
 *   3. 后续估算使用个体基线替代全局基线
 *
 * 设计:
 *   - 无持久化 (重启后重新校准), 因为个体基线在 60 秒内即可重建
 *   - 内存开销: ~1.3 KB (BP_CALIB_BUFFER 12 拍特征)
 *   - 无阻塞, 无 CPU 密集计算 (仅移动平均)
 *
 * 状态机:
 *   0 拍 -> CALIB_STATE_INITIALIZING
 *   1..11 拍 -> CALIB_STATE_COLLECTING
 *   >=12 拍 -> CALIB_STATE_READY (开始使用校准基线)
 *
 * 版本: v2.3.0
 */

#ifndef BP_CALIBRATION_H
#define BP_CALIBRATION_H

// ========== 配置 ==========

// 滚动缓冲区大小 (拍数). 12 拍 * 5 特征 * 4 字节 = 240 字节.
#define BP_CALIB_BUFFER         12

// 单拍 PPG 特征 (从 ppgFeatureExtract 提取, 已归一化)
struct BpCalibFeature {
  float peak_amplitude;   // 收缩峰幅值 (归一化)
  float rise_slope;       // 收缩期上升斜率 (归一化)
  float pwv_proxy;        // 脉搏波宽度代理 (归一化)
};

// 校准状态
struct BpCalibState {
  bool     initialized;       // 模块已初始化
  int      heartbeat_count;   // 已累积的拍数
  bool     calibrated;        // 已达到完成阈值
  int      buffer_head;       // 滚动缓冲区写指针
  int      buffer_len;        // 当前缓冲区填充数 (<= BP_CALIB_BUFFER)

  // 个体基线 (移动平均)
  float    peak_amplitude_ma;  // 个体峰值基线
  float    rise_slope_ma;      // 个体斜率基线
  float    pwv_proxy_ma;       // 个体 PWV 基线

  // 校准时间戳
  unsigned long calibrated_at;  // 校准完成的毫秒时间戳
  unsigned long initialized_at; // 模块初始化的毫秒时间戳
};

// 校准状态枚举 (用于 OLED 显示)
enum BpCalibPhase {
  BP_CALIB_INITIALIZING = 0,   // 未累积数据
  BP_CALIB_COLLECTING   = 1,   // 累积中
  BP_CALIB_READY        = 2    // 校准完成, 使用个体基线
};

// ========== 公共 API ==========

// 初始化校准模块 (在 setup() 调用一次)
void bpCalibInit();

// 重置校准 (下次测量重新开始)
void bpCalibReset();

// 喂入一个心拍的特征 (在每次 PPG 形态学提取后调用)
void bpCalibFeedHeartbeat(const BpCalibFeature* feature);

// 获取当前校准基线 (未校准时返回全局默认值)
float bpCalibGetPeakAmp();   // 个体峰值基线
float bpCalibGetSlope();     // 个体斜率基线
float bpCalibGetPwv();       // 个体 PWV 基线

// 校准状态
bool bpCalibIsCalibrated();         // 是否完成校准
int  bpCalibGetHeartbeatCount();    // 已累积拍数
BpCalibPhase bpCalibGetPhase();     // 当前阶段

// 诊断
void bpCalibPrintStatus();

#endif // BP_CALIBRATION_H
