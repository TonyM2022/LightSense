// ---------------- 闪烁度量层实现 ----------------
// 指标:
//   Flicker %    : 相对峰谷幅度 (IEEE 1789 Percent Flicker = 国标波动深度)
//   Flicker Index: 均值以上面积/总面积 (IEEE 1789, 时域)
//   Pst (LM)     : IEC TR 61547-1 谱估计 Pst = sqrt(Σ (m_k·H(f_k))²)
//   Freq / Zone  : FFT 主频 + 综合判定 (GB 40070 波动深度 + Pst^LM ≤ 1)
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

// ---------------- 综合判定 (GB 40070-2021 + Pst^LM) ----------------
// 与教室测量同一口径: 波动深度 vs 表4 限值 (按主频) + Pst^LM ≤ 1,
// m<1% 视为噪声直接合规; >3125 Hz 豁免波动深度考核 (Pst 仍判)
uint8_t flickerVerdict(float freq, float m, float pst)
{
    if (m < 1.0f)
        return FV_PASS;                 // 噪声门限

    bool pstOK = (pst <= 1.0f);
    if (freq > 3125.0f)
        return pstOK ? FV_EXEMPT : FV_FAIL_PST;

    float limit = (freq <= 10.0f)  ? 0.1f
                : (freq <= 90.0f)  ? 0.01f  * freq
                :                    0.032f * freq;
    bool mOK = (m <= limit);

    if (mOK && pstOK)   return FV_PASS;
    if (!mOK && pstOK)  return FV_FAIL_M;
    if (mOK && !pstOK)  return FV_FAIL_PST;
    return FV_FAIL_BOTH;
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

    // 在平均频谱中搜索主频峰值 (bin 1 起, 抛物线插值细分)
    // 注: 不用 FFT.setArrays()+majorPeak() —— setArrays 每次报告都
    //     delete[]/new[] 未初始化的窗系数缓存, 而 windowing() 会按旧标志
    //     直接复用该缓存, 堆上垃圾被当作 Hann 系数 (FFT 输出全 NaN 的根因)
    float freq = 0.0f;
    if (winCount > 0)
    {
        uint32_t kMax = 1;
        float mMax = specAccum[1];
        for (uint32_t k = 2; k < FFT_SIZE / 2; k++)
        {
            if (specAccum[k] > mMax)
            {
                mMax = specAccum[k];
                kMax = k;
            }
        }
        if (mMax > 0.0f)
        {
            float ym1 = specAccum[kMax - 1];
            float yp1 = specAccum[kMax + 1];
            float den = ym1 - 2.0f * mMax + yp1;
            float delta = (den < -1e-12f) ? 0.5f * (ym1 - yp1) / den
                                          : 0.0f;
            if (delta > 0.5f)
                delta = 0.5f;
            if (delta < -0.5f)
                delta = -0.5f;
            freq = ((float)kMax + delta) * FFT_BIN_HZ;
        }
    }
    // 纯噪声/无信号保护
    if (isnan(freq) || freq < 0.0f)
        freq = 0.0f;

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

    // 综合判定: GB 40070-2021 波动深度 + Pst^LM ≤ 1 (与教室测量口径一致)
    snap.zone = flickerVerdict(freq, flickerPercent, pstLM);
    static const char *const zoneStr[] = {"PASS", "FAIL-M", "FAIL-PST", "FAIL-BOTH", "EXEMPT"};
    Serial.print("Zone     : GB+Pst ");
    Serial.println(zoneStr[snap.zone]);

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
    // 频谱: 窗口平均后按对数频带 max-pool 降采样 (跳过 DC bin 0, 显示上限 5 kHz)
    // 正向映射 bin k -> 组 g = N*ln(k)/ln(KMAX): 组号线性 = 对数线性 (与前端刻度一致);
    // 低频端多组共享同一 bin, 空组延续前一组的值 (阶梯平台), 高频端每组约 27 bin 取 max
    uint8_t nb = (winBlocks > 0) ? winBlocks : 1;
    const uint32_t kLogMax = (uint32_t)(5000.0f / FFT_BIN_HZ);
    const float N_OVER_LOGR = (float)WEB_SPEC_POINTS / logf((float)kLogMax);
    int32_t gPrev = -1;
    for (uint32_t k = 1; k <= kLogMax; k++)
    {
        int32_t g = (int32_t)(logf((float)k) * N_OVER_LOGR);
        if (g > WEB_SPEC_POINTS - 1)
            g = WEB_SPEC_POINTS - 1;
        float a = specAccum[k] / (float)nb;
        if (g > gPrev)
        {
            for (int32_t q = gPrev + 1; q < g; q++)   // 无 bin 覆盖的组: 延续前值
                snap.spec[q] = (gPrev >= 0) ? snap.spec[gPrev] : 0.0f;
            snap.spec[g] = a;
            gPrev = g;
        }
        else if (a > snap.spec[g])
        {
            snap.spec[g] = a;
        }
    }
    while (gPrev + 1 < (int32_t)WEB_SPEC_POINTS)      // 兜底: 末尾空组
        snap.spec[++gPrev] = snap.spec[gPrev];

    flickerResetWindow();
}

const FlickerSnapshot *flickerSnapshot(void)
{
    return &snap;
}
