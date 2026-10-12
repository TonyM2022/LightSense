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
#define PST_BIN_MAX          16                   // Pst 求和上限 bin (78.1 Hz, 加权覆盖至 80 Hz)
#define SVM_BIN_MIN          17                   // SVM 求和下限 bin (83.0 Hz, IEC TR 63158 频段 80~2000 Hz)
#define SVM_BIN_MAX          409                  // SVM 求和上限 bin (1997.1 Hz)

// 综合判定前置光照检查 (v1.1): 窗口均值 ADC 码超出量程 → 光照异常, Pst/SVM 数据无效
// 过暗: DC 基准过小噪声底虚高; 过亮: 接近 12bit 满量程 (4095) 可能削顶引入假谐波
#define ADC_DARK_LIMIT       400                  // 窗口均值 < 400 判光线过暗
#define ADC_BRIGHT_LIMIT     3000                 // 窗口均值 > 3000 判光线太亮

// ---------------- WiFi 网页仪表 ----------------
#define WIFI_AP_SSID         "LightSense"         // AP 热点名
#define WIFI_AP_PASS         "lightsense"         // AP 密码 (至少 8 位)
#define WEB_WAVE_POINTS      256                  // 波形包络段数 (每段 16 点取 min/max)
#define WEB_SPEC_POINTS      256                  // 频谱显示点数 (对数频带 max-pool, 4.9 Hz~5 kHz)

// ---------------- LED PWM 控制 ----------------
#define LED_PIN              21                   // D8 = GPIO21 → 220~330Ω → LED → GND
#define LED_PWM_CHANNEL      0                    // LEDC 通道 0 (与 ADC/WiFi 无冲突)
#define LED_PWM_RESOLUTION   10                   // 10bit 占空比 (1024 级, 最高支持 ~78 kHz)
