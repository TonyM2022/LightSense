// ---------------- 教室灯光测量层实现 ----------------
// 聚合口径 (v1.1 判定):
//   光照    : 窗口均值 ADC 码取均值, 判定前置量程检查 (过暗/过亮直接判异常)
//   Pst^LM  : 全部窗口均值判定 ≤ 1 (峰值仅展示)
//   SVM     : 全部窗口均值判定 ≤ 0.4 (峰值仅展示)
//   波动深度: 全部窗口均值, 仅展示不判定
//   主频    : 取频谱能量最大窗口的主频, 仅展示
// 时长档位: 30s (29 窗) / 1min (58 窗) / 10min (586 窗), 网页选择
// 存储: NVS namespace "cls", 键 cnt + r0..rN-1. 记录 blob 六代:
//   v0.7.1 64B (无 windows/svm/vavg/epoch) / 中间版 64B (仅 windows) /
//   v0.8~v0.9 72B (+svm) / v1.1 76B (+vavg) / v1.5 80B (+epoch) /
//   v1.6 84B (+taskId/pointCode 巡检归属), 按 blob 长度识别;
//   追加尾字段后恒有 "旧 blob 长度 == offsetof(新字段)", 故逐级用 offsetof 判定
//   旧记录 windows==0 (v0.7.1 填充字节恒 0) 补 30s 档, SVM/vavg 补 0 (网页显示 --),
//   epoch 补 0 表示测量日期未知 (无法参与趋势分析), taskId/pointCode 补 0 (非巡检测量)
// 巡检任务: NVS namespace "insp", 键 cnt/seq/cur + t0..tN-1 (InspectionTask blob)
#include <Arduino.h>
#include <Preferences.h>
#include <stddef.h>
#include "config.h"
#include "flicker.h"
#include "classroom.h"
#include "cloud.h"

static Preferences prefs;
static Preferences prefsInsp;               // 巡检任务 NVS (namespace "insp")
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
static uint32_t runEpoch = 0;           // 本次测量的真实时刻 (浏览器传入; 0=未知, 完成时回落 NTP)
static uint16_t runTaskId = 0;          // 本次测量归属的巡检任务号 (0=普通测量)
static uint8_t  runPointCode = CLS_PT_NONE;   // 本次测量的巡检点位码

// 巡检任务 (内存缓存, 与 NVS 同步)
static InspectionTask tasks[INSP_MAX_TASKS];
static uint16_t taskCount = 0;          // 已存任务数
static uint16_t taskSeq = 0;            // 任务号自增计数
static int16_t inspCur = -1;            // 进行中任务在 tasks[] 的下标 (-1=无)

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

// 巡检任务: 单个任务 blob + 元信息 (数量/自增号/进行中任务号)
static void nvsSaveTask(uint16_t idx)
{
    char key[8];
    snprintf(key, sizeof(key), "t%u", idx);
    prefsInsp.putBytes(key, &tasks[idx], sizeof(InspectionTask));
}

static void nvsSaveInspMeta(void)
{
    prefsInsp.putUShort("cnt", taskCount);
    prefsInsp.putUShort("seq", taskSeq);
    prefsInsp.putUShort("cur", (inspCur >= 0) ? tasks[inspCur].id : 0);
}

static void inspLoad(void)
{
    taskCount = prefsInsp.getUShort("cnt", 0);
    taskSeq = prefsInsp.getUShort("seq", 0);
    if (taskCount > INSP_MAX_TASKS)
        taskCount = INSP_MAX_TASKS;
    for (uint16_t i = 0; i < taskCount; i++)
    {
        char key[8];
        snprintf(key, sizeof(key), "t%u", i);
        size_t got = prefsInsp.getBytes(key, &tasks[i], sizeof(InspectionTask));
        if (got != sizeof(InspectionTask))
        {
            taskCount = i;               // 任务损坏, 截断
            break;
        }
        if (tasks[i].id > taskSeq)
            taskSeq = tasks[i].id;
    }
    uint16_t cur = prefsInsp.getUShort("cur", 0);   // 断电前进行中的任务: 恢复继续测
    inspCur = -1;
    if (cur)
    {
        for (uint16_t i = 0; i < taskCount; i++)
            if (tasks[i].id == cur && tasks[i].status == INSP_ST_RUN)
            {
                inspCur = (int16_t)i;
                break;
            }
    }
    Serial.print("Inspect : ");
    Serial.print(taskCount);
    Serial.print(" task(s) loaded, current=");
    Serial.println(inspCur >= 0 ? tasks[inspCur].id : 0);
}

void classroomBegin(void)
{
    prefs.begin("cls", false);
    prefsInsp.begin("insp", false);
    inspLoad();
    recCount = prefs.getUShort("cnt", 0);
    if (recCount > CLS_MAX_RECORDS)
        recCount = CLS_MAX_RECORDS;
    for (uint16_t i = 0; i < recCount; i++)
    {
        char key[8];
        snprintf(key, sizeof(key), "r%u", i);
        size_t got = prefs.getBytes(key, &recs[i], sizeof(ClassroomRecord));
        if (got == offsetof(ClassroomRecord, taskId))
        {
            // 80B blob: v1.5 (有 epoch, 无巡检归属) → 非巡检测量
            recs[i].taskId = 0;
            recs[i].pointCode = CLS_PT_NONE;
        }
        else if (got == offsetof(ClassroomRecord, epoch))
        {
            // 76B blob: v1.1 (有 vavg, 无 epoch) → 测量日期未知
            recs[i].epoch = 0;
            recs[i].taskId = 0;
            recs[i].pointCode = CLS_PT_NONE;
        }
        else if (got == offsetof(ClassroomRecord, vavgMean))
        {
            // 72B blob: v0.8~v0.9 (有 windows/svm, 无 vavg)
            recs[i].vavgMean = 0;
            recs[i].epoch = 0;
            recs[i].taskId = 0;
            recs[i].pointCode = CLS_PT_NONE;
        }
        else if (got == offsetof(ClassroomRecord, svmMean))
        {
            // 64B 旧 blob: v0.7.1 (无 windows/svm/vavg) 或中间版 (仅 windows)
            if (recs[i].windows == 0)
                recs[i].windows = CLS_WINDOWS;   // v0.7.1: windows 落在填充字节, 恒 0
            recs[i].svmMean = recs[i].svmMax = 0.0f;
            recs[i].vavgMean = 0;
            recs[i].epoch = 0;
            recs[i].taskId = 0;
            recs[i].pointCode = CLS_PT_NONE;
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
// 统一启动入口: 普通测量与巡检测点共用 (taskId/pointCode 为巡检归属, 普通测量传 0/NONE)
static bool startRun(const char *label, uint16_t windows, uint32_t epoch,
                     uint16_t taskId, uint8_t pointCode)
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
    runEpoch = epoch;
    runTaskId = taskId;
    runPointCode = pointCode;
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

bool classroomStart(const char *label, uint16_t windows, uint32_t epoch)
{
    return startRun(label, windows, epoch, 0, CLS_PT_NONE);
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

const char *classroomLabel(void)
{
    return runLabel;
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
    r.epoch   = runEpoch ? runEpoch : cloudEpoch();   // 浏览器时间优先; 否则用 NTP 校时 (无网则 0=日期未知)
    r.taskId  = runTaskId;                            // 巡检测量归属任务 (0 = 普通测量)
    r.pointCode = runPointCode;
    r.verdict = flickerVerdict(r.pstMean, r.svmMean, (float)r.vavgMean);

    recs[recCount++] = r;
    nvsSaveRecord(recCount - 1);
    cloudPushRecord(&recs[recCount - 1], recCount - 1);   // 云端: 入待同步队列 (未配置时忽略)

    // 巡检: 记录该点位已完成 (掩码置位 + 计数刷新 + 落盘), 供 OLED/网页推进到下一点位
    if (runTaskId && runPointCode >= CLS_PT_C && runPointCode <= CLS_PT_RB)
    {
        for (uint16_t i = 0; i < taskCount; i++)
        {
            if (tasks[i].id != runTaskId)
                continue;
            tasks[i].mask |= (uint8_t)(1u << (runPointCode - CLS_PT_C));
            uint8_t c = 0;
            for (uint8_t b = 0; b < CLS_PT_COUNT; b++)
                if (tasks[i].mask & (uint8_t)(1u << b)) c++;
            tasks[i].pointsCount = c;
            ClassroomSummary sum;
            classroomSummarize(tasks[i].id, &sum);   // 每点完成即刷新教室级结论 (M2)
            tasks[i].verdict = sum.verdict;
            nvsSaveTask(i);
            Serial.print("Inspect : task ");
            Serial.print(runTaskId);
            Serial.print(" point ");
            Serial.print(classroomPointCode(runPointCode));
            Serial.print(" done, ");
            Serial.print(c);
            Serial.println("/5");
            break;
        }
    }
    runTaskId = 0;
    runPointCode = CLS_PT_NONE;

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

    // 记录清空后巡检任务的点位归属已不存在, 任务一并清空
    taskCount = 0;
    taskSeq = 0;
    inspCur = -1;
    prefsInsp.clear();
    Serial.println("Classroom: all records + inspection tasks cleared");
}

// ---------------- 巡检点位名 ----------------
static const char *const PT_CODES[CLS_PT_COUNT + 1] = {"--", "C", "LF", "RF", "LB", "RB"};
static const char *const PT_NAMES[CLS_PT_COUNT + 1] = {"--", "中心", "左前", "右前", "左后", "右后"};

const char *classroomPointCode(uint8_t code)
{
    return (code <= CLS_PT_COUNT) ? PT_CODES[code] : "--";
}

const char *classroomPointName(uint8_t code)
{
    return (code <= CLS_PT_COUNT) ? PT_NAMES[code] : "--";
}

// ---------------- 巡检任务层 ----------------
bool classroomInspectCreate(const char *room, const char *scene, const char *inspector, uint32_t epoch)
{
    if (running || inspCur >= 0 || taskCount >= INSP_MAX_TASKS)
        return false;
    if (!room || !room[0])
        return false;

    InspectionTask &t = tasks[taskCount];
    memset(&t, 0, sizeof(t));
    t.id = ++taskSeq;
    strncpy(t.room, room, sizeof(t.room) - 1);
    if (scene) strncpy(t.scene, scene, sizeof(t.scene) - 1);
    if (inspector) strncpy(t.inspector, inspector, sizeof(t.inspector) - 1);
    t.epoch = epoch;
    t.status = INSP_ST_RUN;
    t.mask = 0;
    t.pointsCount = 0;
    t.verdict = 0xFF;                      // 保留: 设备端不汇总教室级结论 (前端按 5 点结果计算)
    inspCur = (int16_t)taskCount;
    taskCount++;
    nvsSaveTask((uint16_t)inspCur);
    nvsSaveInspMeta();

    Serial.print("Inspect : task ");
    Serial.print(t.id);
    Serial.print(" created, room=\"");
    Serial.print(t.room);
    Serial.print("\", scene=\"");
    Serial.print(t.scene);
    Serial.println("\"");
    return true;
}

bool classroomInspectAbort(void)
{
    if (inspCur < 0)
        return false;
    if (running)                           // 测量进行中不允许中止 (等本次点位测完)
        return false;
    tasks[inspCur].status = INSP_ST_STOP;
    nvsSaveTask((uint16_t)inspCur);
    Serial.print("Inspect : task ");
    Serial.print(tasks[inspCur].id);
    Serial.println(" aborted");
    inspCur = -1;
    nvsSaveInspMeta();
    return true;
}

bool classroomInspectFinish(void)
{
    if (inspCur < 0)
        return false;
    if (running)
        return false;
    tasks[inspCur].status = INSP_ST_DONE;
    ClassroomSummary sum;
    classroomSummarize(tasks[inspCur].id, &sum);
    tasks[inspCur].verdict = sum.verdict;
    nvsSaveTask((uint16_t)inspCur);
    Serial.print("Inspect : task ");
    Serial.print(tasks[inspCur].id);
    Serial.print(" finished, ");
    Serial.print(tasks[inspCur].pointsCount);
    Serial.println("/5 points");
    inspCur = -1;
    nvsSaveInspMeta();
    return true;
}

bool classroomInspectActive(void)
{
    return inspCur >= 0;
}

uint16_t classroomInspectTaskId(void)
{
    return (inspCur >= 0) ? tasks[inspCur].id : 0;
}

const InspectionTask *classroomInspectTask(void)
{
    return (inspCur >= 0) ? &tasks[inspCur] : NULL;
}

uint8_t classroomInspectNextPoint(void)
{
    if (inspCur < 0)
        return CLS_PT_NONE;
    for (uint8_t i = 0; i < CLS_PT_COUNT; i++)
        if (!(tasks[inspCur].mask & (uint8_t)(1u << i)))
            return (uint8_t)(CLS_PT_C + i);
    return CLS_PT_NONE;
}

bool classroomInspectStartPoint(uint8_t pt, uint16_t windows, uint32_t epoch)
{
    if (inspCur < 0 || pt < CLS_PT_C || pt > CLS_PT_RB)
        return false;
    const InspectionTask &t = tasks[inspCur];
    if (t.status != INSP_ST_RUN)
        return false;

    char lbl[32];
    snprintf(lbl, sizeof(lbl), "%s-%s", t.room, classroomPointCode(pt));
    return startRun(lbl, windows, epoch, t.id, pt);
}

const InspectionTask *classroomTasks(uint16_t *n)
{
    *n = taskCount;
    return tasks;
}

// ---------------- 教室级汇总层 (M2) ----------------
// 同一任务同一点位多次复测: recs[] 按入库顺序追加, 取最后一条
static const ClassroomRecord *recOfPoint(uint16_t taskId, uint8_t pt)
{
    const ClassroomRecord *out = NULL;
    for (uint16_t i = 0; i < recCount; i++)
        if (recs[i].taskId == taskId && recs[i].pointCode == pt)
            out = &recs[i];
    return out;
}

// 单点严重度: 双项超标 > 单项超标 > 光照异常 > 合规 (与网页 sevOf 一致)
static uint8_t verdictSeverity(uint8_t v)
{
    switch (v)
    {
    case FV_PASS:
    case FV_EXEMPT:    return 0;
    case FV_FAIL_BOTH: return 3;
    case FV_DARK:
    case FV_BRIGHT:    return 1;
    default:           return 2;
    }
}

uint8_t classroomSummarize(uint16_t taskId, ClassroomSummary *out)
{
    if (!out)
        return CLS_SUM_NONE;
    memset(out, 0, sizeof(*out));
    out->taskId = taskId;
    if (!taskId)
        return out->verdict = CLS_SUM_NONE;

    uint8_t worstSev = 0;
    bool mHigh = false;          // 波动深度大但 Pst/SVM 合规
    bool centerFail = false;     // 中心点异常 → 整体风险 (设计文档 9.2 C)

    for (uint8_t pt = CLS_PT_C; pt <= CLS_PT_RB; pt++)
    {
        const ClassroomRecord *r = recOfPoint(taskId, pt);
        if (!r)
            continue;
        out->points++;
        if (r->pstMean > 1.0f) out->pstFailCnt++;
        if (r->svmMean > 0.4f) out->svmFailCnt++;
        if (r->mMean >= 30.0f && r->pstMean <= 1.0f && r->svmMean <= 0.4f)
            mHigh = true;
        if (r->verdict == FV_PASS)
        {
            out->passCnt++;
            continue;
        }
        out->failCnt++;
        if (r->verdict == FV_DARK)
            out->darkCnt++;
        else if (r->verdict == FV_BRIGHT)
            out->brightCnt++;
        if (pt == CLS_PT_C)
            centerFail = true;
        uint8_t sev = verdictSeverity(r->verdict);
        if (sev > worstSev)      // 同严重度保留先出现的点位 (中心优先)
        {
            worstSev = sev;
            out->worstCode = pt;
        }
    }

    if (out->points < CLS_PT_COUNT)
        out->verdict = CLS_SUM_NONE;              // 未测全: 不下"合规"结论
    else if (!out->failCnt)
        out->verdict = CLS_SUM_PASS;
    else if (out->failCnt <= 2 && !centerFail)
        out->verdict = CLS_SUM_LOCAL;
    else
        out->verdict = CLS_SUM_ALL;

    uint16_t adv = 0;
    if (out->verdict == CLS_SUM_PASS) adv |= CLS_ADV_PASS;
    if (out->pstFailCnt)              adv |= CLS_ADV_PST;
    if (out->svmFailCnt)              adv |= CLS_ADV_SVM;
    if (out->darkCnt)                 adv |= CLS_ADV_DARK;
    if (out->brightCnt)               adv |= CLS_ADV_BRIGHT;
    if (mHigh)                        adv |= CLS_ADV_MFREQ;
    if (out->failCnt >= 3)            adv |= CLS_ADV_ALL;
    if (centerFail && out->failCnt <= 2) adv |= CLS_ADV_CENTER;
    if (out->failCnt && out->failCnt <= 2 && !centerFail) adv |= CLS_ADV_LOCAL;
    if (out->points < CLS_PT_COUNT)   adv |= CLS_ADV_INCOMPLETE;
    out->advice = adv;
    return out->verdict;
}

bool classroomSummaryActive(ClassroomSummary *out)
{
    if (inspCur < 0)
        return false;
    classroomSummarize(tasks[inspCur].id, out);
    return out->points > 0;
}

const char *classroomSummaryText(uint8_t verdict)
{
    switch (verdict)
    {
    case CLS_SUM_PASS:  return "A 教室合规";
    case CLS_SUM_LOCAL: return "B 教室局部风险";
    case CLS_SUM_ALL:   return "C 教室整体风险";
    default:            return "--";
    }
}
