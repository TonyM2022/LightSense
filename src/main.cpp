/*
  ESP32-S3 Flicker Meter V2

  功能:
  1. ADC DMA 连续采样 (IDF adc_digi 驱动, 硬件时钟定节拍)
      - 采样率 : 20 ksps
      - 缓存   : 4096 点/块
  2. FFT 频谱分析 (arduinoFFT 库, 4096 点, Hann 窗, 去直流, 峰值插值提频)
  3. 串口输出统计 (每 5 块 ≈ 1.024 s 一次):
      Samples / Max / Min / Average / Flicker % / Flicker Index / Pst(LM) / Flicker Frequency
      - Flicker %    : 相对峰谷幅度 (IEEE 1789 Percent Flicker)
      - Flicker Index: 均值以上面积/总面积 (IEEE 1789, 时域)
      - Pst (LM)     : IEC TR 61547-1 短时闪烁严重度谱估计
                       Pst = sqrt(Σ (m_k·H(f_k))²), m_k=2|X_k|/|X_0| (Hann 归一化),
                       H 为 IEC 61000-4-15 人眼加权曲线 (8.8Hz 处归一化为 1)
                       注意: 受 4.88Hz 频率分辨率限制, <15Hz 频段精度有限

  说明:
  - GPIO1 = ADC1_CH0, 12bit (0~4095), 11dB 衰减 (量程约 0~3.1V)
  - 20ksps 由 SAR ADC 数字控制器硬件时钟产生, 节拍精度优于软件定时器中断
  - 频率分辨率 = 20000 / 4096 ≈ 4.88 Hz
*/

#include <Arduino.h>
#include <arduinoFFT.h>
#include "driver/adc.h"

// ---------------- 配置 ----------------
#define ADC_PIN              1                    // GPIO1 = ADC1_CH0
#define ADC_CHANNEL          ADC1_CHANNEL_0
#define SAMPLE_RATE_HZ       20000                // 20 ksps
#define FFT_SIZE             4096                 // 缓存点数 (2 的幂)
#define BYTES_PER_SAMPLE     4                    // S3 DMA 每样本 4 字节
#define DMA_BUF_BYTES        (FFT_SIZE * BYTES_PER_SAMPLE)
#define BLOCKS_PER_REPORT    5                    // 5 块 ≈ 1.024 s 输出一次
#define REPORT_SAMPLES       (FFT_SIZE * BLOCKS_PER_REPORT)
#define FFT_BIN_HZ           ((float)SAMPLE_RATE_HZ / (float)FFT_SIZE)
#define PST_BIN_MAX          7                    // Pst 求和上限 bin (34.2 Hz, 加权表至 35 Hz)

// ---------------- 采样与 FFT 缓存 ----------------
static uint32_t dmaBuf[FFT_SIZE * BYTES_PER_SAMPLE / 4];   // DMA 读取缓冲 (16KB)
static float vReal[FFT_SIZE];                              // FFT 实部 / 幅度谱
static float vImag[FFT_SIZE];                              // FFT 虚部
static float specAccum[FFT_SIZE / 2 + 1];                  // 报告窗口内频谱累加
static ArduinoFFT<float> FFT(vReal, vImag, FFT_SIZE, (float)SAMPLE_RATE_HZ);

// ---------------- 统计变量 ----------------
static uint16_t winMax = 0;                                // 窗口内最大值
static uint16_t winMin = 4095;                             // 窗口内最小值
static uint32_t winSum = 0;                                // 窗口内总和
static uint32_t winCount = 0;                              // 窗口内样本数
static float winAboveMean = 0.0f;                          // 窗口内均值以上面积和 (Flicker Index)
static uint32_t blockIndex = 0;                            // 已处理块计数
static uint32_t overflowCnt = 0;                           // DMA 溢出计数
static uint32_t dbgBlocks = 0;                             // 诊断: 成功处理的块数
static uint32_t dbgTimeout = 0;                            // 诊断: 读取超时次数
static uint32_t dbgShort = 0;                              // 诊断: 不完整块次数
static uint32_t lastDbgMs = 0;                             // 诊断: 上次输出时间

// ---------------- 处理一块 4096 点数据 ----------------
static void processBlock(void)
{
    const adc_digi_output_data_t *frames = (const adc_digi_output_data_t *)dmaBuf;

    // 1. 解析 DMA 数据并更新时域统计
    uint32_t sum = 0;
    uint16_t bMax = 0;
    uint16_t bMin = 4095;
    for (uint32_t i = 0; i < FFT_SIZE; i++)
    {
        uint16_t v = frames[i].type2.data;   // 12bit 有效数据
        if (v > bMax) bMax = v;
        if (v < bMin) bMin = v;
        sum += v;
        vReal[i] = (float)v;
    }

    if (bMax > winMax) winMax = bMax;
    if (bMin < winMin) winMin = bMin;
    winSum += sum;
    winCount += FFT_SIZE;

    // 2. Flicker Index 累加: 均值以上面积 (IEEE 1789, 时域)
    //    窗口含大量整周期时无需对齐周期边界
    float mean = (float)sum / (float)FFT_SIZE;
    for (uint32_t i = 0; i < FFT_SIZE; i++)
    {
        float d = vReal[i] - mean;
        if (d > 0.0f) winAboveMean += d;
    }

    // 3. 去直流 + 加 Hann 窗 + FFT (arduinoFFT 库)
    for (uint32_t i = 0; i < FFT_SIZE; i++)
        vImag[i] = 0.0f;
    FFT.dcRemoval();
    FFT.windowing(FFTWindow::Hann, FFTDirection::Forward);
    FFT.compute(FFTDirection::Forward);
    FFT.complexToMagnitude();

    // 4. 幅度谱累加到报告窗口
    for (uint32_t k = 0; k <= FFT_SIZE / 2; k++)
    {
        specAccum[k] += vReal[k];
    }
}

// ---------------- IEC 61000-4-15 人眼闪烁加权 ----------------
// 锚点: Table 2 正弦调制 Pst=1 可见阈值 ΔV/V (%) → H(f) = 0.25 / thr(f), 8.8Hz 处 H=1
static float flickerWeight(float f)
{
    const float thrF[] = {1.0f, 2.0f, 4.0f, 6.0f, 8.8f, 10.0f,
                          12.0f, 16.0f, 20.0f, 25.0f, 35.0f};
    const float thrM[] = {1.43f, 0.90f, 0.55f, 0.397f, 0.25f, 0.262f,
                          0.312f, 0.482f, 0.70f, 1.14f, 2.58f};
    const uint8_t NPTS = 11;
    const float thrPeak = 0.25f;

    if (f < thrF[0]) f = thrF[0];
    if (f > thrF[NPTS - 1]) return 0.0f;   // >35 Hz: 人眼不敏感

    for (uint8_t i = 0; i < NPTS - 1; i++)
    {
        if (f <= thrF[i + 1])
        {
            float t = (logf(f) - logf(thrF[i])) /
                      (logf(thrF[i + 1]) - logf(thrF[i]));
            float thr = expf(logf(thrM[i]) +
                             t * (logf(thrM[i + 1]) - logf(thrM[i])));
            return thrPeak / thr;
        }
    }
    return 0.0f;
}

// ---------------- 输出统计报告 ----------------
static void reportWindow(void)
{
    // Flicker%: 相对峰谷幅度 (IEEE 1789 定义)
    float flickerPercent = 0.0f;
    if ((winMax + winMin) > 0)
    {
        flickerPercent =
            100.0f * (float)(winMax - winMin) /
            (float)(winMax + winMin);
    }

    // Flicker Index (IEEE 1789): 均值以上面积 / 总面积
    float flickerIndex = (winSum > 0) ? (winAboveMean / (float)winSum) : 0.0f;

    // 在平均频谱中搜索主频峰值 (库内置对数域抛物线插值)
    float freq = 0.0f;
    if (winCount > 0)
    {
        FFT.setArrays(specAccum, vImag, FFT_SIZE);
        freq = FFT.majorPeak();
        FFT.setArrays(vReal, vImag, FFT_SIZE);
    }

    // Pst^LM (IEC TR 61547-1 谱估计): Pst = sqrt(Σ (m_k·H(f_k))²)
    // m_k = 2·|X_k|/|X_0| (Hann 窗归一化的相对调制幅度), H 为人眼加权曲线
    float pstLM = 0.0f;
    if (specAccum[0] > 0.0f && winCount > 0)
    {
        for (uint32_t k = 1; k <= PST_BIN_MAX; k++)
        {
            float mk = 2.0f * specAccum[k] / specAccum[0];
            float wk = flickerWeight((float)k * FFT_BIN_HZ);
            pstLM += (mk * wk) * (mk * wk);
        }
        pstLM = sqrtf(pstLM);
    }

    Serial.println();
    Serial.println("------------------");

    Serial.print("Samples  : ");
    Serial.println(winCount);

    Serial.print("Max      : ");
    Serial.println(winMax);

    Serial.print("Min      : ");
    Serial.println(winMin);

    Serial.print("Average  : ");
    Serial.println((float)winSum / (float)winCount, 1);

    Serial.print("Flicker  : ");
    Serial.print(flickerPercent, 2);
    Serial.println(" %");

    Serial.print("FlickerIx: ");
    Serial.println(flickerIndex, 3);

    Serial.print("Pst (LM) : ");
    Serial.println(pstLM, 3);

    Serial.print("Freq     : ");
    Serial.print(freq, 2);
    Serial.println(" Hz");

    if (overflowCnt > 0)
    {
        Serial.print("Overflow : ");
        Serial.println(overflowCnt);
        overflowCnt = 0;
    }

    Serial.println("------------------");
    Serial.println();

    // 清零重新统计
    winMax = 0;
    winMin = 4095;
    winSum = 0;
    winCount = 0;
    winAboveMean = 0.0f;
    for (uint32_t k = 0; k <= FFT_SIZE / 2; k++)
        specAccum[k] = 0.0f;
}

// ---------------- 初始化 ----------------
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

    // 频谱累加缓冲清零
    for (uint32_t k = 0; k <= FFT_SIZE / 2; k++)
        specAccum[k] = 0.0f;

    // ADC DMA 驱动初始化
    adc_digi_init_config_t initCfg = {};
    initCfg.max_store_buf_size = DMA_BUF_BYTES * 2;   // 驱动内部环形缓冲
    initCfg.conv_num_each_intr = 4096;                // 每次中断 4096 字节
    initCfg.adc1_chan_mask = (1 << ADC_CHANNEL);
    initCfg.adc2_chan_mask = 0;

    esp_err_t err = adc_digi_initialize(&initCfg);
    if (err != ESP_OK)
    {
        Serial.print("adc_digi_initialize failed: 0x");
        Serial.println(err, HEX);
        while (true) delay(1000);
    }

    adc_digi_pattern_config_t pattern = {};
    pattern.atten     = ADC_ATTEN_DB_12;   // 12dB 衰减 (即旧称 11dB), 量程约 0~3.1V
    pattern.channel   = ADC_CHANNEL;
    pattern.unit      = 0;   // DMA 模式 unit 为 0 基索引: 0=ADC1, 1=ADC2 (S3 仅支持 ADC1 DMA)
    pattern.bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;   // DMA 模式填实际位宽 12 (非枚举值)

    adc_digi_configuration_t ctrlCfg = {};
    ctrlCfg.conv_limit_en  = true;
    ctrlCfg.conv_limit_num = 255;
    ctrlCfg.pattern_num    = 1;
    ctrlCfg.adc_pattern    = &pattern;
    ctrlCfg.sample_freq_hz = SAMPLE_RATE_HZ;
    ctrlCfg.conv_mode      = ADC_CONV_SINGLE_UNIT_1;
    ctrlCfg.format         = ADC_DIGI_OUTPUT_FORMAT_TYPE2;

    err = adc_digi_controller_configure(&ctrlCfg);
    if (err != ESP_OK)
    {
        Serial.print("adc_digi_controller_configure failed: 0x");
        Serial.println(err, HEX);
        while (true) delay(1000);
    }

    err = adc_digi_start();
    if (err != ESP_OK)
    {
        Serial.print("adc_digi_start failed: 0x");
        Serial.println(err, HEX);
        while (true) delay(1000);
    }

    Serial.println("ADC DMA started.");
}

// ---------------- 主循环: 累积读取一个完整块 ----------------
void loop()
{
    static bool chunkLogged = false;

    // 累积读取 4096 样本 (16384 字节)
    // 兼容按中断块分片返回数据的驱动, 短读不再整块丢弃
    uint32_t got = 0;
    while (got < DMA_BUF_BYTES)
    {
        uint32_t outLen = 0;
        esp_err_t err = adc_digi_read_bytes((uint8_t *)dmaBuf + got,
                                            DMA_BUF_BYTES - got, &outLen, 200);

        if (err == ESP_ERR_INVALID_STATE)
        {
            overflowCnt++;        // DMA 溢出: 数据丢失, 重新开始本块
            got = 0;
            break;
        }
        if (err != ESP_OK || outLen == 0)
        {
            dbgTimeout++;
            got = 0;
            break;
        }
        if (!chunkLogged)
        {
            chunkLogged = true;
            Serial.print("DBG first chunk = ");
            Serial.print(outLen);
            Serial.println(" bytes");
        }
        got += outLen;
    }

    // 周期诊断输出 (每 2 s)
    if (millis() - lastDbgMs >= 2000)
    {
        lastDbgMs = millis();
        Serial.print("DBG blocks=");
        Serial.print(dbgBlocks);
        Serial.print(" timeout=");
        Serial.print(dbgTimeout);
        Serial.print(" short=");
        Serial.print(dbgShort);
        Serial.print(" ovr=");
        Serial.println(overflowCnt);
    }

    if (got < DMA_BUF_BYTES)
    {
        if (got > 0) dbgShort++;
        return;
    }

    // 首块诊断: 打印解析出的前 4 个样本值, 验证 TYPE2 帧解析
    if (dbgBlocks == 0)
    {
        const adc_digi_output_data_t *fr = (const adc_digi_output_data_t *)dmaBuf;
        Serial.print("DBG raw[0..3] = ");
        for (int i = 0; i < 4; i++)
        {
            Serial.print(fr[i].type2.data);
            Serial.print(' ');
        }
        Serial.println();
    }

    processBlock();
    blockIndex++;
    dbgBlocks++;

    if (blockIndex % BLOCKS_PER_REPORT == 0)
    {
        reportWindow();
    }
}
