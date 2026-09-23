// ---------------- 教室灯光测量层实现 ----------------
// 聚合口径 (用户确认):
//   波动深度: 29 窗口均值判定 (峰值仅展示), <1% 视为噪声 PASS
//   Pst^LM  : 29 窗口均值判定 ≤ 1 (峰值仅展示)
//   主频    : 取频谱能量最大窗口的主频
// 存储: NVS namespace "cls", 键 cnt + r0..rN-1 (每记录 65 字节 blob)
#include <Arduino.h>
#include <Preferences.h>
#include "config.h"
#include "flicker.h"
#include "classroom.h"

static Preferences prefs;
static ClassroomRecord recs[CLS_MAX_RECORDS];
static uint16_t recCount = 0;
static uint16_t autoIndex = 0;          // 自动命名计数
static bool running = false;

// 测量累加器
static uint32_t runWindows = 0;
static float accM = 0.0f;               // 波动深度累加
static float accMMax = 0.0f;
static float accFI = 0.0f;
static float accPst = 0.0f;
static float accPstMax = 0.0f;
static float bestEnergy = -1.0f;        // 频谱能量最大窗口
static float bestFreq = 0.0f;
static char runLabel[32];

// 最近一次完成的结果 (供网页层取走)
static bool resultPending = false;
static ClassroomRecord lastResult;

// ---------------- GB 40070-2021 判定 ----------------
// 限值 (表4): 0.1% (f<=10) | 0.01f (10~90) | 0.032f (90~3125) | >3125 免除
// 波动深度均值 + Pst^LM 均值 (≤1) 双指标
static uint8_t judge(float freq, float mMean, float pstMean)
{
    if (mMean < 1.0f)
        return CLS_PASS;                // 噪声门限

    bool pstOK = (pstMean <= 1.0f);
    if (freq > 3125.0f)
        return pstOK ? CLS_EXEMPT : CLS_FAIL_PST;

    float limit = (freq <= 10.0f)  ? 0.1f
                : (freq <= 90.0f)  ? 0.01f  * freq
                :                    0.032f * freq;
    bool mOK = (mMean <= limit);

    if (mOK && pstOK)   return CLS_PASS;
    if (!mOK && pstOK)  return CLS_FAIL_M;
    if (mOK && !pstOK)  return CLS_FAIL_PST;
    return CLS_FAIL_BOTH;
}

// ---------------- NVS ----------------
static void nvsSaveRecord(uint16_t idx)
{
    char key[8];
    snprintf(key, sizeof(key), "r%u", idx);
    prefs.putBytes(key, &recs[idx], sizeof(ClassroomRecord));
    prefs.putUShort("cnt", recCount);
}

void classroomBegin(void)
{
    prefs.begin("cls", false);
    recCount = prefs.getUShort("cnt", 0);
    if (recCount > CLS_MAX_RECORDS)
        recCount = CLS_MAX_RECORDS;
    for (uint16_t i = 0; i < recCount; i++)
    {
        char key[8];
        snprintf(key, sizeof(key), "r%u", i);
        size_t got = prefs.getBytes(key, &recs[i], sizeof(ClassroomRecord));
        if (got != sizeof(ClassroomRecord))
        {
            recCount = i;               // 记录损坏, 截断
            break;
        }
    }
    autoIndex = recCount;

    Serial.print("Classroom: ");
    Serial.print(recCount);
    Serial.println(" record(s) loaded from NVS");
}

// ---------------- 控制 ----------------
bool classroomStart(const char *label)
{
    if (running || recCount >= CLS_MAX_RECORDS)
        return false;

    running = true;
    runWindows = 0;
    accM = accMMax = accFI = accPst = accPstMax = 0.0f;
    bestEnergy = -1.0f;
    bestFreq = 0.0f;

    if (label && label[0])
    {
        strncpy(runLabel, label, sizeof(runLabel) - 1);
        runLabel[sizeof(runLabel) - 1] = 0;
    }
    else
    {
        snprintf(runLabel, sizeof(runLabel), "灯具 %u", ++autoIndex);
    }

    Serial.print("Classroom: start \"");
    Serial.print(runLabel);
    Serial.println("\"");
    return true;
}

bool classroomIsRunning(void)
{
    return running;
}

uint32_t classroomWindowsDone(void)
{
    return runWindows;
}

// ---------------- 逐窗口聚合 ----------------
bool classroomOnWindow(const FlickerSnapshot *s)
{
    if (!running)
        return false;

    accM += s->flickerPercent;
    if (s->flickerPercent > accMMax) accMMax = s->flickerPercent;
    accFI += s->flickerIndex;
    accPst += s->pstLM;
    if (s->pstLM > accPstMax) accPstMax = s->pstLM;

    float e = 0.0f;
    for (int i = 0; i < WEB_SPEC_POINTS; i++)
        e += s->spec[i];
    if (e > bestEnergy)
    {
        bestEnergy = e;
        bestFreq = s->freqHz;
    }

    if (++runWindows < CLS_WINDOWS)
        return false;

    // 测量完成: 汇总并入库
    ClassroomRecord r = {};
    r.ts = millis() / 1000;
    strncpy(r.label, runLabel, sizeof(r.label) - 1);
    r.mMean   = accM / (float)CLS_WINDOWS;
    r.mMax    = accMMax;
    r.fi      = accFI / (float)CLS_WINDOWS;
    r.pstMean = accPst / (float)CLS_WINDOWS;
    r.pstMax  = accPstMax;
    r.freq    = bestFreq;
    r.verdict = judge(r.freq, r.mMean, r.pstMean);

    recs[recCount++] = r;
    nvsSaveRecord(recCount - 1);

    lastResult = r;
    resultPending = true;
    running = false;
    runWindows = 0;

    Serial.print("Classroom: done \"");
    Serial.print(r.label);
    Serial.print("\" m=");
    Serial.print(r.mMean, 2);
    Serial.print("% pst=");
    Serial.print(r.pstMean, 3);
    Serial.print(" f=");
    Serial.print(r.freq, 2);
    Serial.print("Hz verdict=");
    Serial.println(r.verdict);
    return true;
}

bool classroomPopResult(ClassroomRecord *out)
{
    if (!resultPending)
        return false;
    resultPending = false;
    *out = lastResult;
    return true;
}

const ClassroomRecord *classroomRecords(uint16_t *n)
{
    *n = recCount;
    return recs;
}

void classroomClear(void)
{
    recCount = 0;
    autoIndex = 0;
    prefs.clear();
    prefs.putUShort("cnt", 0);
    Serial.println("Classroom: all records cleared");
}
