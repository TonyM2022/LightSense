#pragma once
// ---------------- 闪烁度量层 ----------------
// 窗口统计 + FFT 频谱 + 各项指标计算与报告输出 + 网页快照
#include <stdint.h>
#include "config.h"

// 综合判定码: 光照量程检查 (v1.1 前置否决) + Pst^LM ≤ 1 + SVM ≤ 0.4 (v0.9 起, 两页面共用)
#define FV_PASS            0    // 合规
#define FV_FAIL_M          1    // 超标: 波动深度 (仅旧记录, v0.8 及以前口径, 新逻辑不产出)
#define FV_FAIL_PST        2    // 超标: Pst^LM > 1
#define FV_FAIL_BOTH       3    // 超标: Pst^LM + SVM 双超
#define FV_EXEMPT          4    // 高频豁免 (仅旧记录, 新逻辑不产出)
#define FV_FAIL_SVM        5    // 超标: SVM > 0.4
#define FV_DARK            6    // 光线过暗 (窗口均值 ADC 码 < ADC_DARK_LIMIT, Pst/SVM 数据无效)
#define FV_BRIGHT          7    // 光线太亮 (窗口均值 ADC 码 > ADC_BRIGHT_LIMIT, Pst/SVM 数据无效)

// 综合判定: 光照量程前置否决 (vavg < 400 过暗 / > 3000 过亮, 见 config.h) +
//           Pst^LM ≤ 1 (IEC TR 61547-1) + SVM ≤ 0.4 (IEC TR 63158 / EU 2024)
// 波动深度与主频仅展示不判定; 码值 1/4 保留旧记录兼容
uint8_t flickerVerdict(float pst, float svm, float vavg);

// 网页仪表快照: flickerReport() 在每个报告窗口结束时填充一次
typedef struct {
    uint32_t seq;                  // 快照序号 (窗口计数, 从 1 开始)
    uint32_t samples;              // 窗口内样本数
    uint32_t overflow;             // 窗口内 DMA 溢出次数
    uint16_t vmax;                 // 峰值 (ADC 码 0~4095)
    uint16_t vmin;                 // 谷值
    float    vavg;                 // 均值
    float    flickerPercent;       // 波动深度 % (Percent Flicker)
    float    flickerIndex;         // Flicker Index
    float    pstLM;                // Pst (LM), 1s 窗值 (无平滑)
    float    svm;                  // SVM 频闪可见度 (IEC TR 63158, 80~2000 Hz, 1s 窗值)
    float    freqHz;               // FFT 主频 Hz
    uint8_t  zone;                 // FV_* 综合判定码
    float    wmin[WEB_WAVE_POINTS];   // 末块波形包络: 每段最小值
    float    wmax[WEB_WAVE_POINTS];   // 末块波形包络: 每段最大值
    float    spec[WEB_SPEC_POINTS];   // 窗口平均频谱 (对数频带 max-pool, 4.9 Hz~5 kHz)
} FlickerSnapshot;

// 清零报告窗口 (统计数据 + 频谱累加)
void flickerResetWindow(void);

// 处理一块 FFT_SIZE 点样本: 更新时域统计, FFT 并累加频谱
void flickerProcessBlock(const float *samples);

// 输出统计报告并清零窗口 (同时填充网页快照)
// overflow = 报告窗口内累计的 DMA 溢出次数 (>0 时打印)
void flickerReport(uint32_t overflow);

// 获取最新快照指针 (由 flickerReport 填充, 主循环在报告后交给网页层广播)
const FlickerSnapshot *flickerSnapshot(void);
