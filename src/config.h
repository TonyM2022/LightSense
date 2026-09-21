#pragma once
// ---------------- 全局配置 ----------------
#include "driver/adc.h"

#define ADC_PIN              1                    // GPIO1 = ADC1_CH0
#define ADC_CHANNEL          ADC1_CHANNEL_0
#define SAMPLE_RATE_HZ       20000                // 20 ksps
#define FFT_SIZE             4096                 // 缓存点数 (2 的幂)
#define BYTES_PER_SAMPLE     4                    // S3 DMA 每样本 4 字节
#define DMA_BUF_BYTES        (FFT_SIZE * BYTES_PER_SAMPLE)
#define BLOCKS_PER_REPORT    5                    // 5 块 ≈ 1.024 s 输出一次
#define FFT_BIN_HZ           ((float)SAMPLE_RATE_HZ / (float)FFT_SIZE)
#define PST_BIN_MAX          7                    // Pst 求和上限 bin (34.2 Hz, 加权表至 35 Hz)
