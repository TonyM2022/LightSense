/*
  ESP32-S3 Flicker Meter V2

  功能:
  1. ADC DMA 连续采样 (IDF adc_digi 驱动, 硬件时钟定节拍)
      - 采样率 : 20 ksps
      - 缓存   : 4096 点/块
  2. FFT 频谱分析 (arduinoFFT 库, 4096 点, Hann 窗, 去直流, 峰值插值提频)
  3. 串口输出统计 (每 5 块 ≈ 1.024 s 一次):
      Samples / Max / Min / Average / Flicker % / Flicker Index / Pst(LM) / Flicker Frequency / Zone
      - Flicker %    : 相对峰谷幅度 (IEEE 1789 Percent Flicker = 国标波动深度)
      - Flicker Index: 均值以上面积/总面积 (IEEE 1789, 时域)
      - Zone         : GB 40070-2021 波动深度合规判定
                       限值: 0.1% (f<=10) | 0.01f (10~90) | 0.032f (90~3125) | 豁免 (>3125)
                       结果: PASS=合规 FAIL=超标 EXEMPT=高频豁免; m<1% 视为噪声直接 PASS
      - Pst (LM)     : IEC TR 61547-1 短时闪烁严重度谱估计
                       Pst = sqrt(Σ (m_k·H(f_k))²), m_k=2|X_k|/|X_0| (Hann 归一化),
                       H 为 IEC 61000-4-15 人眼加权曲线 (8.8Hz 处归一化为 1)
                       注意: 受 4.88Hz 频率分辨率限制, <15Hz 频段精度有限

  说明:
  - GPIO1 = ADC1_CH0, 12bit (0~4095), 11dB 衰减 (量程约 0~3.1V)
  - 20ksps 由 SAR ADC 数字控制器硬件时钟产生, 节拍精度优于软件定时器中断
  - 频率分辨率 = 20000 / 4096 ≈ 4.88 Hz

  分层:
  - config.h   : 全局配置
  - adc_dma.*  : 驱动层 (ADC DMA 初始化, 整块读取, TYPE2 帧解析, 诊断)
  - flicker.*  : 度量层 (窗口统计, FFT, 指标计算, 报告输出)
  - main.cpp   : 编排 (setup/loop)
*/

#include <Arduino.h>
#include "config.h"
#include "adc_dma.h"
#include "flicker.h"

static float blockBuf[FFT_SIZE];      // 整块样本缓冲 (16KB)
static uint32_t blockIndex = 0;       // 已处理块计数

void setup()
{
    Serial.begin(115200);
    delay(500);

    Serial.println();
    Serial.println("ESP32-S3 Flicker Meter V2");
    Serial.print("ADC DMA : ");
    Serial.print(SAMPLE_RATE_HZ / 1000);
    Serial.println(" ksps, ADC1_CH0 (GPIO1)");
    Serial.print("FFT     : ");
    Serial.print(FFT_SIZE);
    Serial.print(" points, resolution ");
    Serial.print(FFT_BIN_HZ, 3);
    Serial.println(" Hz");
    Serial.print("Report  : every ");
    Serial.print(BLOCKS_PER_REPORT);
    Serial.print(" blocks (");
    Serial.print((float)(FFT_SIZE * BLOCKS_PER_REPORT) / (float)SAMPLE_RATE_HZ, 3);
    Serial.println(" s)");
    Serial.println();

    flickerResetWindow();

    if (!adcDmaBegin())
    {
        while (true) delay(1000);     // 初始化失败, 停机
    }
}

// ---------------- 主循环: 取块 → 处理 → 定期报告 ----------------
void loop()
{
    if (!adcDmaReadBlock(blockBuf))
    {
        return;                       // 本块失败 (溢出/超时), 丢弃
    }

    flickerProcessBlock(blockBuf);

    if (++blockIndex % BLOCKS_PER_REPORT == 0)
    {
        flickerReport(adcDmaTakeOverflow());
    }
}
