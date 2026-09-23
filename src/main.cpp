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
      - Zone         : 综合判定 (GB 40070-2021 波动深度 + Pst^LM ≤ 1, 与教室测量同口径)
                       限值: 0.1% (f<=10) | 0.01f (10~90) | 0.032f (90~3125) | 豁免 (>3125)
                       结果: PASS=合规 FAIL-M=波动深度超标 FAIL-PST=Pst超标
                             FAIL-BOTH=双项超标 EXEMPT=高频豁免; m<1% 视为噪声直接 PASS
      - Pst (LM)     : IEC TR 61547-1 短时闪烁严重度谱估计
                       Pst = sqrt(Σ (m_k·H(f_k))²), m_k=2|X_k|/|X_0| (Hann 归一化),
                       H 为 IEC 61000-4-15 人眼加权曲线 (8.8Hz 处归一化为 1)
                       注意: 受 4.88Hz 频率分辨率限制, <15Hz 频段精度有限
  4. WiFi 网页仪表 (webdash 层):
      - S3 开 AP 热点 (SSID 见 config.h), 浏览器访问 http://192.168.4.1
      - 三页面按钮切换: 频闪仪 / LED 控制 / 教室测量
      - WebSocket 每报告窗口推送 JSON 快照: 波形包络 / 频谱 / 指标 / 教室测量进度
  5. LED 闪烁频率控制 (led_pwm 层, D8 = GPIO21):
      - LEDC 硬件 PWM: 常灭 / 常亮 / 50Hz / 100Hz / 500Hz / 1kHz / 5kHz, 闪烁模式占空比 50%
      - 网页 LED 控制页切换, 与测量互不干扰 (可照射自测)
  6. 教室灯光测量 (classroom 层):
      - 多灯具依次测量, 每次固定 29 窗口 ≈ 30s, 可打标签
      - 聚合: 波动深度均值/峰值, Flicker Index, Pst^LM 均值/峰值, 主频
      - 判定: 波动深度均值 vs GB 40070 表4 限值 + Pst^LM 均值 ≤ 1
      - 记录 NVS 持久化, 网页可导出 CSV

  说明:
  - GPIO1 = ADC1_CH0, 12bit (0~4095), 11dB 衰减 (量程约 0~3.1V)
  - 20ksps 由 SAR ADC 数字控制器硬件时钟产生, 节拍精度优于软件定时器中断
  - 频率分辨率 = 20000 / 4096 ≈ 4.88 Hz

  分层:
  - config.h   : 全局配置
  - adc_dma.*  : 驱动层 (ADC DMA 初始化, 整块读取, TYPE2 帧解析, 诊断)
  - led_pwm.*  : 驱动层 (LEDC PWM 输出, 模式切换)
  - flicker.*  : 度量层 (窗口统计, FFT, 指标计算, 报告输出, 网页快照)
  - classroom.*: 度量层扩展 (教室多灯具 30s 测量, GB 判定, NVS 记录)
  - webdash.*  : 网页层 (AP 热点, Web 服务器, WebSocket JSON 推送, 控制路由)
  - main.cpp   : 编排 (setup/loop)
*/

#include <Arduino.h>
#include "config.h"
#include "adc_dma.h"
#include "led_pwm.h"
#include "flicker.h"
#include "classroom.h"
#include "webdash.h"

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

    ledPwmBegin();                        // LED PWM 输出 (默认常亮, 网页可切换)

    classroomBegin();                     // 教室测量: 加载 NVS 历史记录

    // WiFi 网页仪表 (失败仅告警, 串口功能不受影响)
    if (!webdashBegin())
    {
        Serial.println("WARN: WiFi web dashboard start failed.");
    }

    if (!adcDmaBegin())
    {
        while (true) delay(1000);     // 初始化失败, 停机
    }
}

// ---------------- 主循环: 取块 → 处理 → 定期报告/推送 ----------------
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

        const FlickerSnapshot *snap = flickerSnapshot();
        classroomOnWindow(snap);          // 教室测量: 逐窗口累加
        webdashBroadcast(snap);           // 推送到浏览器
    }
}
