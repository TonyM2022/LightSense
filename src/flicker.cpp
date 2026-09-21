// ---------------- 闪烁度量层实现 ----------------
// 指标:
//   Flicker %    : 相对峰谷幅度 (IEEE 1789 Percent Flicker = 国标波动深度)
//   Flicker Index: 均值以上面积/总面积 (IEEE 1789, 时域)
//   Pst (LM)     : IEC TR 61547-1 谱估计 Pst = sqrt(Σ (m_k·H(f_k))²)
//   Freq / Zone  : FFT 主频 + GB 40070-2021 波动深度合规判定
// 网页快照: 每个报告窗口填充最新波形包络/平均频谱/指标, 供 webdash 层推送
#include <Arduino.h>
#include <arduinoFFT.h>
#include "config.h"
#include "flicker.h"

// FFT 缓存
static float vReal[FFT_SIZE];                              // 实部 / 幅度谱
static float vImag[FFT_SIZE];                              // 虚部
static float specAccum[FFT_SIZE / 2 + 1];                  // 报告窗口内频谱累加
static ArduinoFFT<float> FFT(vReal, vImag, FFT_SIZE, (float)SAMPLE_RATE_HZ);

// 窗口统计
static uint16_t winMax = 0;                                // 窗口内最大值
static uint16_t winMin = 4095;                             // 窗口内最小值
static uint32_t winSum = 0;                                // 窗口内总和
static uint32_t winCount = 0;                              // 窗口内样本数
static float winAboveMean = 0.0f;                          // 均值以上面积和 (Flicker Index)
static uint8_t winBlocks = 0;                              // 窗口内已处理块数 (频谱平均)

// 末块波形包络: 每 WAVE_SEG 点取 min/max (4096 点 → WEB_WAVE_POINTS 段)
#define WAVE_SEG (FFT_SIZE / WEB_WAVE_POINTS)
static float lastWmin[WEB_WAVE_POINTS];
static float lastWmax[WEB_WAVE_POINTS];

// 网页快照 (flickerReport 填充)
static FlickerSnapshot snap;

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

// ---------------- GB 40070-2021 波动深度合规判定 ----------------
// 输入: f = 主频 (Hz), m = 波动深度/Percent Flicker (%)
// 限值 (表4): 0.1% (f<=10) | 0.01f (10~90) | 0.032f (90~3125) | >3125 免除考核
// 返回: 0=合规 1=超标 2=高频豁免
static uint8_t gb40070Zone(float f, float m)
{
    if (f > 3125.0f)
        return 2;

    float limit = (f <= 10.0f)  ? 0.1f
                : (f <= 90.0f)  ? 0.01f  * f
                :                 0.032f * f;
    if (m <= limit)
        return 0;

    return 1;
}

void flickerResetWindow(void)
{
    winMax = 0;
    winMin = 4095;
    winSum = 0;
    winCount = 0;
    winAboveMean = 0.0f;
    winBlocks = 0;
    for (uint32_t k = 0; k <= FFT_SIZE / 2; k++)
        specAccum[k] = 0.0f;
}

void flickerProcessBlock(const float *samples)
{
    // 1. 时域统计 + 波形包络降采样 (每 WAVE_SEG 点取 min/max)
    uint32_t sum = 0;
    uint16_t bMax = 0;
    uint16_t bMin = 4095;
    uint16_t segMin = 4095;
    uint16_t segMax = 0;
    uint32_t seg = 0;
    for (uint32_t i = 0; i < FFT_SIZE; i++)
    {
        uint16_t v = (uint16_t)samples[i];
        if (v > bMax) bMax = v;
        if (v < bMin) bMin = v;
        if (v > segMax) segMax = v;
        if (v < segMin) segMin = v;
        sum += v;
        vReal[i] = (float)v;

        if ((i % WAVE_SEG) == WAVE_SEG - 1)      // 段末: 保存包络点
        {
            lastWmin[seg] = (float)segMin;
            lastWmax[seg] = (float)segMax;
            seg++;
            segMin = 4095;
            segMax = 0;
        }
    }
    winBlocks++;

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

void flickerReport(uint32_t overflow)
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

    // GB 40070-2021 波动深度合规判定 (基于主频 + 整体波动深度)
    snap.noisePass = (flickerPercent < 1.0f) ? 1 : 0;
    snap.zone = snap.noisePass ? 0 : gb40070Zone(freq, flickerPercent);
    const char *zoneStr;
    if (snap.noisePass)
    {
        zoneStr = "PASS (<1%)";
    }
    else
    {
        switch (snap.zone)
        {
            case 0:  zoneStr = "PASS";   break;
            case 1:  zoneStr = "FAIL";   break;
            default: zoneStr = "EXEMPT"; break;
        }
    }
    Serial.print("Zone     : GB40070 ");
    Serial.println(zoneStr);

    if (overflow > 0)
    {
        Serial.print("Overflow : ");
        Serial.println(overflow);
    }

    Serial.println("------------------");
    Serial.println();

    // 填充网页快照 (重置窗口前完成)
    snap.seq++;
    snap.samples = winCount;
    snap.overflow = overflow;
    snap.vmax = winMax;
    snap.vmin = winMin;
    snap.vavg = (float)winSum / (float)winCount;
    snap.flickerPercent = flickerPercent;
    snap.flickerIndex = flickerIndex;
    snap.pstLM = pstLM;
    snap.freqHz = freq;
    for (uint32_t g = 0; g < WEB_WAVE_POINTS; g++)
    {
        snap.wmin[g] = lastWmin[g];
        snap.wmax[g] = lastWmax[g];
    }
    // 频谱: 窗口平均后按组 max-pool 降采样 (跳过 DC bin 0)
    uint8_t nb = (winBlocks > 0) ? winBlocks : 1;
    for (uint32_t g = 0; g < WEB_SPEC_POINTS; g++)
    {
        float m = 0.0f;
        for (uint32_t b = 0; b < (FFT_SIZE / 2) / WEB_SPEC_POINTS; b++)
        {
            uint32_t k = 1 + g * ((FFT_SIZE / 2) / WEB_SPEC_POINTS) + b;
            if (k > FFT_SIZE / 2)
                break;
            float a = specAccum[k] / (float)nb;
            if (a > m) m = a;
        }
        snap.spec[g] = m;
    }

    flickerResetWindow();
}

const FlickerSnapshot *flickerSnapshot(void)
{
    return &snap;
}
