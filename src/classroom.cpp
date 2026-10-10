// ---------------- 教室灯光测量层实现 ----------------
// 聚合口径 (v1.1 判定):
//   光照    : 窗口均值 ADC 码取均值, 判定前置量程检查 (过暗/过亮直接判异常)
//   Pst^LM  : 全部窗口均值判定 ≤ 1 (峰值仅展示)
//   SVM     : 全部窗口均值判定 ≤ 0.4 (峰值仅展示)
//   波动深度: 全部窗口均值, 仅展示不判定
//   主频    : 取频谱能量最大窗口的主频, 仅展示
// 时长档位: 30s (29 窗) / 1min (58 窗) / 10min (586 窗), 网页选择
// 存储: NVS namespace "cls", 键 cnt + r0..rN-1. 记录 blob 四代:
//   v0.7.1 64B (无 windows/svm/vavg) / 中间版 64B (仅 windows) /
//   v0.8~v0.9 72B (+svm) / 当前版 74B (+vavg), 按 blob 长度识别;
//   64B 旧记录 windows==0 (v0.7.1 填充字节恒 0) 补 30s 档, SVM/vavg 补 0 (网页显示 --)
#include <Arduino.h>
#include <Preferences.h>
#include <stddef.h>
#include "config.h"
#include "flicker.h"
#include "classroom.h"

static Preferences prefs;
static ClassroomRecord recs[CLS_MAX_RECORDS];
static uint16_t recCount = 0;
static uint16_t autoIndex = 0;          // 自动命名计数
static bool running = false;
static uint16_t runTarget = CLS_WINDOWS; // 本次测量目标窗口数

// 测量累加器
static uint32_t runWindows = 0;
static float accM = 0.0f;               // 波动深度累加
static float accMMax = 0.0f;
static float accFI = 0.0f;
static float accPst = 0.0f;
static float accPstMax = 0.0f;
static float accSvm = 0.0f;
static float accSvmMax = 0.0f;
static float accV = 0.0f;               // 光照 (窗口均值 ADC 码) 累加
static float bestEnergy = -1.0f;        // 频谱能量最大窗口
static float bestFreq = 0.0f;
static char runLabel[32];

// 最近一次完成的结果 (供网页层取走)
static bool resultPending = false;
static ClassroomRecord lastResult;

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
        if (got == offsetof(ClassroomRecord, vavgMean))
        {
            // 72B blob: v0.8~v0.9 (有 windows/svm, 无 vavg)
            recs[i].vavgMean = 0;
        }
        else if (got == offsetof(ClassroomRecord, svmMean))
        {
            // 64B 旧 blob: v0.7.1 (无 windows/svm/vavg) 或中间版 (仅 windows)
            if (recs[i].windows == 0)
                recs[i].windows = CLS_WINDOWS;   // v0.7.1: windows 落在填充字节, 恒 0
            recs[i].svmMean = recs[i].svmMax = 0.0f;
            recs[i].vavgMean = 0;
        }
        else if (got != sizeof(ClassroomRecord))
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
bool classroomStart(const char *label, uint16_t windows)
{
    if (running || recCount >= CLS_MAX_RECORDS)
        return false;

    // 时长档位限定 (非法值回落 30s 档)
    if (windows != CLS_WINDOWS &&
        windows != CLS_WINDOWS_1MIN &&
        windows != CLS_WINDOWS_10MIN)
        windows = CLS_WINDOWS;

    running = true;
    runWindows = 0;
    runTarget = windows;
    accM = accMMax = accFI = accPst = accPstMax = accSvm = accSvmMax = accV = 0.0f;
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
    Serial.print("\", ");
    Serial.print(runTarget);
    Serial.print(" windows (");
    Serial.print((float)runTarget * 1.024f, 1);
    Serial.println(" s)");
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

uint16_t classroomWindowsTarget(void)
{
    return runTarget;
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
    accSvm += s->svm;
    if (s->svm > accSvmMax) accSvmMax = s->svm;
    accV += s->vavg;

    float e = 0.0f;
    for (int i = 0; i < WEB_SPEC_POINTS; i++)
        e += s->spec[i];
    if (e > bestEnergy)
    {
        bestEnergy = e;
        bestFreq = s->freqHz;
    }

    if (++runWindows < runTarget)
        return false;

    // 测量完成: 汇总并入库
    ClassroomRecord r = {};
    r.ts = millis() / 1000;
    strncpy(r.label, runLabel, sizeof(r.label) - 1);
    r.windows = runTarget;
    r.mMean   = accM / (float)runTarget;
    r.mMax    = accMMax;
    r.fi      = accFI / (float)runTarget;
    r.pstMean = accPst / (float)runTarget;
    r.pstMax  = accPstMax;
    r.svmMean = accSvm / (float)runTarget;
    r.svmMax  = accSvmMax;
    r.vavgMean = (uint16_t)(accV / (float)runTarget + 0.5f);
    r.freq    = bestFreq;
    r.verdict = flickerVerdict(r.pstMean, r.svmMean, (float)r.vavgMean);

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
    Serial.print(" svm=");
    Serial.print(r.svmMean, 3);
    Serial.print(" vavg=");
    Serial.print(r.vavgMean);
    Serial.print(" f=");
    Serial.print(r.freq, 2);
    Serial.print("Hz wins=");
    Serial.print(r.windows);
    Serial.print(" verdict=");
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
