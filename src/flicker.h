#pragma once
// ---------------- 闪烁度量层 ----------------
// 窗口统计 + FFT 频谱 + 各项指标计算与报告输出 + 网页快照
#include <stdint.h>
#include "config.h"

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
    float    pstLM;                // Pst (LM)
    float    freqHz;               // FFT 主频 Hz
    uint8_t  zone;                 // 0=PASS 1=FAIL 2=EXEMPT
    uint8_t  noisePass;            // 1=波动深度<1% 视为噪声直接判 PASS
    float    wmin[WEB_WAVE_POINTS];   // 末块波形包络: 每段最小值
    float    wmax[WEB_WAVE_POINTS];   // 末块波形包络: 每段最大值
    float    spec[WEB_SPEC_POINTS];   // 窗口平均频谱 (max-pool 降采样)
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
