#pragma once
// ---------------- ADC DMA 驱动层 ----------------
// IDF adc_digi 连续采样驱动封装: 20 ksps 硬件节拍, 每块 FFT_SIZE 点
#include <stdint.h>

// 初始化 ADC DMA (失败时打印错误并返回 false)
bool adcDmaBegin(void);

// 阻塞读取一整块 FFT_SIZE 点样本 (TYPE2 帧解析为 0~4095 浮点)
// 返回 false 表示本块失败 (DMA 溢出或读取超时), 调用方直接放弃本块
bool adcDmaReadBlock(float *out);

// 取出并清零累计的 DMA 溢出计数 (用于统计报告中显示)
uint32_t adcDmaTakeOverflow(void);
