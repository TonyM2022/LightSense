// ---------------- 闪烁度量层实现 ----------------
// 指标:
//   Flicker %    : 相对峰谷幅度 (IEEE 1789 Percent Flicker = 国标波动深度)
//   Flicker Index: 均值以上面积/总面积 (IEEE 1789, 时域)
//   Pst (LM)     : IEC TR 61547-1 谱估计 Pst = sqrt(Σ (m_k·H(f_k))²)
//                  m_k = 2|X_k|/|X_DC| (相对平均光水平), H = 1/可见阈值, 加权 4.9~80 Hz
//                  显示值经跨窗 EMA 频谱平滑 (≈8s, 周期分量无损), 抑制噪声读数抖动
//   Pst 10min    : 滚动 586 窗能量 (Pst²) 均值开方, 标准 10min 观察窗口径结论值
//   SVM          : IEC TR 63158 频闪可见度 SVM = (Σ (C_m/T(f_m))^3.7)^(1/3.7), 80~2000 Hz
//                  T(f) = 2.865e-5·f^1.543 + 0.225 (50% 人群可见阈); 同一经 EMA 频谱平滑
//   Freq / Zone  : FFT 主频 (仅展示) + 综合判定 (Pst^LM ≤ 1 + SVM ≤ 0.4, v0.9 起; 波动深度仅展示)
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

// 滚动 10min Pst^LM 累积: 每报告窗存能量 E = pstLM², 结论值 = sqrt(能量均值)
// (等效对整个 10min 观察窗做能量平均, 平滑单窗瞬时值的跳动)
static float pstRing[PST10_WINDOWS];                       // 每窗能量环形缓冲 (~2.3KB)
static uint16_t pst10Idx = 0;                              // 环形写指针
static uint32_t pst10Count = 0;                            // 已累积窗口数 (0~PST10_WINDOWS)

// 跨窗频谱平滑 (bins 1..SVM_BIN_MAX, 覆盖 Pst 与 SVM 两个频段): EMA 系数 1/8 ≈ 8s 时间常数,
// 周期调制分量 (相干平均) 不受影响, 噪声估计方差降 ~8 倍 → 1s 读数抖动约降 65%
#define PST_SMOOTH_SHIFT 3
static float specSmooth[SVM_BIN_MAX + 1];                  // 平滑频谱 (bin 0 未用)
static bool  specSmoothInit = false;                       // 上电首窗直接载入

// 末块波形包络: 每 WAVE_SEG 点取 min/max (4096 点 → WEB_WAVE_POINTS 段)
#define WAVE_SEG (FFT_SIZE / WEB_WAVE_POINTS)
static float lastWmin[WEB_WAVE_POINTS];
static float lastWmax[WEB_WAVE_POINTS];

// 网页快照 (flickerReport 填充)
static FlickerSnapshot snap;

// ---------------- IEC 61000-4-15 人眼闪烁加权 ----------------
// 锚点: Table 2 正弦调制 Pst=1 可见阈值 ΔV/V (%), H(f) = 1/thr(f) (m 为相对调制幅度)
// → 8.8Hz 处 thr=0.25%, H=400: 单独 0.25% 正弦调制贡献 Pst=1 (标定点)
// >35 Hz: 阈值按闪烁仪 35Hz 二阶低通滚降外推 (thr ∝ f²), >80 Hz 人眼不响应 (权重 0)
static float flickerWeight(float f)
{
    const float thrF[] = {1.0f, 2.0f, 4.0f, 6.0f, 8.8f, 10.0f,
                          12.0f, 16.0f, 20.0f, 25.0f, 35.0f};
    const float thrM[] = {1.43f, 0.90f, 0.55f, 0.397f, 0.25f, 0.262f,
                          0.312f, 0.482f, 0.70f, 1.14f, 2.58f};
    const uint8_t NPTS = 11;
    const float thr35 = 2.58f;

    if (f < thrF[0]) f = thrF[0];
    if (f > 80.0f) return 0.0f;            // >80 Hz: 超出人眼闪烁融合范围

    if (f > thrF[NPTS - 1])                // 35~80 Hz: thr ∝ f² (二阶低通滚降)
        return 100.0f / (thr35 * (f / thrF[NPTS - 1]) * (f / thrF[NPTS - 1]));

    for (uint8_t i = 0; i < NPTS - 1; i++)
    {
        if (f <= thrF[i + 1])
        {
            float t = (logf(f) - logf(thrF[i])) /
                      (logf(thrF[i + 1]) - logf(thrF[i]));
            float thr = expf(logf(thrM[i]) +
                             t * (logf(thrM[i + 1]) - logf(thrM[i])));
            return 100.0f / thr;
        }
    }
    return 0.0f;
}

// 由频谱计算 Pst^LM 谱估计 (详见 flickerReport)
// 逐"分量簇"加权而非逐 bin: Hann 主瓣跨 ~4 bin, 单频若不在 bin 中心会泄漏到
// 相邻 bin, 逐 bin 独立平方和会把同一分量重复计入 (50Hz=bin10.24 高估 ~30%);
// 取主瓣 3bin 幅度和 ÷2 恢复真实分量幅度 (Hann 主瓣三bin和恒 ≈ 2|X_peak|,
// bin 中心/偏心误差 <7%), 抛物线插值细分真实频率 (与主频搜索同式) 后单次加权
static float pstFromSpectrum(const float *spec, float dcRef)
{
    float s = 0.0f;
    uint32_t k = 1;
    while (k <= PST_BIN_MAX)
    {
        if (spec[k] >= spec[k - 1] && spec[k] >= spec[k + 1])   // 局部极大 → 一个分量
        {
            float amp = (spec[k - 1] + spec[k] + spec[k + 1]) * 0.5f;
            float den = spec[k - 1] - 2.0f * spec[k] + spec[k + 1];
            float delta = (den < -1e-12f) ? 0.5f * (spec[k - 1] - spec[k + 1]) / den
                                          : 0.0f;
            if (delta > 0.5f)
                delta = 0.5f;
            if (delta < -0.5f)
                delta = -0.5f;
            float mk = 2.0f * amp / dcRef;
            float wk = flickerWeight(((float)k + delta) * FFT_BIN_HZ);
            s += (mk * wk) * (mk * wk);
            k += 3;                              // 跳过本主瓣, 避免泄漏项重复计入
        }
        else
        {
            k++;
        }
    }
    return sqrtf(s);
}

// SVM 频闪可见阈函数 (IEC TR 63158): T(f) = 2.865e-5·f^1.543 + 0.225
// 单位为相对调制幅度, 50% 人群检出概率; 随频率单调上升 (高频越不敏感)
static float svmThreshold(float f)
{
    return 2.865e-5f * powf(f, 1.543f) + 0.225f;
}

// 由频谱计算 SVM (IEC TR 63158): SVM = (Σ (C_m/T(f_m))^3.7)^(1/3.7), 80~2000 Hz
// 分量提取与 pstFromSpectrum 相同 (主瓣簇 3bin 幅度和÷2 + 抛物线插值定频),
// 仅求和指数 (3.7 次 Minkowski 范数, 强分量主导) 与频段 (SVM_BIN_MIN~MAX) 不同
static float svmFromSpectrum(const float *spec, float dcRef)
{
    float s = 0.0f;
    uint32_t k = SVM_BIN_MIN;
    while (k <= SVM_BIN_MAX)
    {
        if (spec[k] >= spec[k - 1] && spec[k] >= spec[k + 1])   // 局部极大 → 一个分量
        {
            float amp = (spec[k - 1] + spec[k] + spec[k + 1]) * 0.5f;
            float den = spec[k - 1] - 2.0f * spec[k] + spec[k + 1];
            float delta = (den < -1e-12f) ? 0.5f * (spec[k - 1] - spec[k + 1]) / den
                                          : 0.0f;
            if (delta > 0.5f)
                delta = 0.5f;
            if (delta < -0.5f)
                delta = -0.5f;
            float f = ((float)k + delta) * FFT_BIN_HZ;
            float x = (2.0f * amp / dcRef) / svmThreshold(f);
            if (x > 0.0f)
                s += powf(x, 3.7f);
            k += 3;                              // 跳过本主瓣
        }
        else
        {
            k++;
        }
    }
    return (s > 0.0f) ? powf(s, 1.0f / 3.7f) : 0.0f;
}

// ---------------- 综合判定 (Pst^LM + SVM, v0.9) ----------------
// 与教室测量同一口径: Pst^LM ≤ 1 (IEC TR 61547-1) + SVM ≤ 0.4 (IEC TR 63158 / EU 2024);
// 波动深度与主频仅展示不判定; 判定入参为平滑后显示值 (教室侧为全窗均值)
uint8_t flickerVerdict(float pst, float svm)
{
    bool pstOK = (pst <= 1.0f);
    bool svmOK = (svm <= 0.4f);

    if (pstOK && svmOK)  return FV_PASS;
    if (!pstOK && svmOK) return FV_FAIL_PST;
    if (pstOK && !svmOK) return FV_FAIL_SVM;
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

    // 频谱归一化为单块平均: specAccum 是窗口内 winBlocks 块 FFT 幅度累加,
    // 而 Pst 的 DC 基准 |X_DC| = 窗口均值×N/2 是单块尺度, 两者必须同尺度,
    // 否则 m_k 虚大 winBlocks(=5) 倍 (50Hz 100% 方波 Pst 实测 ~150 的根因)
    if (winBlocks > 1)
    {
        float inv = 1.0f / (float)winBlocks;
        for (uint32_t k = 0; k <= FFT_SIZE / 2; k++)
            specAccum[k] *= inv;
    }

    // Pst^LM (IEC TR 61547-1 谱估计): Pst = sqrt(Σ (m_i·H(f_i))²)
    // m_i = 2·|X_i|/|X_DC| 为相对平均光水平的调制分量幅度, H 为人眼加权曲线
    // DC 基准取窗口实测均值 (块内已 dcRemoval, FFT bin0 只剩泄漏不能作基准);
    // Hann 窗对直流的等效幅度 |X_DC| = DC·Σw ≈ DC·N/2
    // raw  : 本窗频谱直接计算 → 存入 10min 能量累积 (保持逐窗口径)
    // disp : 跨窗 EMA 平滑频谱计算 → 显示/推送 (抗噪抖, 周期分量无损)
    // SVM  : 同一 dcRef/同一平滑频谱, 频段与权重不同, 同样 raw(备用)/disp(显示)
    float pstRaw = 0.0f;
    float pstDisp = 0.0f;
    float svmRaw = 0.0f;
    float svmDisp = 0.0f;
    if (winCount > 0)
    {
        float dcRef = ((float)winSum / (float)winCount) *
                      ((float)FFT_SIZE * 0.5f);
        if (dcRef > 0.0f)
        {
            pstRaw = pstFromSpectrum(specAccum, dcRef);
            svmRaw = svmFromSpectrum(specAccum, dcRef);

            if (!specSmoothInit)
            {
                for (uint32_t k = 1; k <= SVM_BIN_MAX; k++)
                    specSmooth[k] = specAccum[k];
                specSmoothInit = true;
            }
            else
            {
                for (uint32_t k = 1; k <= SVM_BIN_MAX; k++)
                    specSmooth[k] += (specAccum[k] - specSmooth[k]) *
                                     (1.0f / (float)(1 << PST_SMOOTH_SHIFT));
            }
            pstDisp = pstFromSpectrum(specSmooth, dcRef);
            svmDisp = svmFromSpectrum(specSmooth, dcRef);
            (void)svmRaw;                // SVM 无 10min 累积口径, raw 仅保留调试语义
        }
    }

    // 累积 10min Pst^LM: 本窗原始能量存入环形缓冲, 全体均值开方
    pstRing[pst10Idx] = pstRaw * pstRaw;
    if (++pst10Idx >= PST10_WINDOWS)
        pst10Idx = 0;
    if (pst10Count < PST10_WINDOWS)
        pst10Count++;
    float pst10 = 0.0f;
    if (pst10Count > 0)
    {
        float e10 = 0.0f;
        for (uint32_t i = 0; i < pst10Count; i++)
            e10 += pstRing[i];
        pst10 = sqrtf(e10 / (float)pst10Count);
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
    Serial.println(pstDisp, 3);

    Serial.print("Pst 10min: ");
    Serial.print(pst10, 3);
    Serial.print("  (");
    Serial.print(pst10Count);
    Serial.print("/");
    Serial.print(PST10_WINDOWS);
    Serial.println(" win)");

    Serial.print("SVM      : ");
    Serial.println(svmDisp, 3);

    Serial.print("Freq     : ");
    Serial.print(freq, 2);
    Serial.println(" Hz");

    // 综合判定: Pst^LM ≤ 1 + SVM ≤ 0.4 (v0.9, 与教室测量口径一致)
    snap.zone = flickerVerdict(pstDisp, svmDisp);
    static const char *const zoneStr[] = {"PASS", "FAIL-M", "FAIL-PST", "FAIL-BOTH", "EXEMPT", "FAIL-SVM"};
    Serial.print("Zone     : Pst+SVM ");
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
    snap.pstLM = pstDisp;
    snap.pst10LM = pst10;
    snap.pst10Wins = pst10Count;
    snap.svm = svmDisp;
    snap.freqHz = freq;
    for (uint32_t g = 0; g < WEB_WAVE_POINTS; g++)
    {
        snap.wmin[g] = lastWmin[g];
        snap.wmax[g] = lastWmax[g];
    }
    // 频谱: 单块平均后按对数频带 max-pool 降采样 (跳过 DC bin 0, 显示上限 5 kHz)
    // 正向映射 bin k -> 组 g = N*ln(k)/ln(KMAX): 组号线性 = 对数线性 (与前端刻度一致);
    // 低频端多组共享同一 bin, 空组延续前一组的值 (阶梯平台), 高频端每组约 27 bin 取 max
    const uint32_t kLogMax = (uint32_t)(5000.0f / FFT_BIN_HZ);
    const float N_OVER_LOGR = (float)WEB_SPEC_POINTS / logf((float)kLogMax);
    int32_t gPrev = -1;
    for (uint32_t k = 1; k <= kLogMax; k++)
    {
        int32_t g = (int32_t)(logf((float)k) * N_OVER_LOGR);
        if (g > WEB_SPEC_POINTS - 1)
            g = WEB_SPEC_POINTS - 1;
        float a = specAccum[k];
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
