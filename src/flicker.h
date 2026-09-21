#pragma once
// ---------------- 闪烁度量层 ----------------
// 窗口统计 + FFT 频谱 + 各项指标计算与报告输出
#include <stdint.h>

// 清零报告窗口 (统计数据 + 频谱累加)
void flickerResetWindow(void);

// 处理一块 FFT_SIZE 点样本: 更新时域统计, FFT 并累加频谱
void flickerProcessBlock(const float *samples);

// 输出统计报告并清零窗口
// overflow = 报告窗口内累计的 DMA 溢出次数 (>0 时打印)
void flickerReport(uint32_t overflow);
