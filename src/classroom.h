#pragma once
// ---------------- 教室灯光测量层 ----------------
// 多灯具依次测量: 时长可选 30s / 1min / 10min 档, 逐窗口聚合指标,
// 判定 (v1.1): 光照量程前置否决 + Pst^LM 均值 ≤ 1 + SVM 均值 ≤ 0.4
// (波动深度/主频/光照仅展示或检查不参与双指标); 记录 NVS 持久化, 断电不丢
// v1.6: 教室巡检 (5 点标准测点) —— 记录追加 taskId/pointCode 归属巡检任务,
//       新增 InspectionTask 任务层 (教室号/场景/巡检人/已完成点位), NVS namespace "insp"
#include <stdint.h>
#include "flicker.h"

#define CLS_MAX_RECORDS     80   // NVS 最大记录数 (满后需导出 CSV 并清空; 80 条 ≈ 16 次 5 点巡检)
#define CLS_WINDOWS         29   // 30s 档窗口数 (29 × 1.024s ≈ 29.7s)
#define CLS_WINDOWS_1MIN    58   // 1min 档窗口数 (58 × 1.024s ≈ 59.4s)
#define CLS_WINDOWS_10MIN   586  // 10min 档窗口数 (586 × 1.024s ≈ 600.1s)

// 巡检点位 (标准 5 点模板: 中心 + 四角, 编码与设计文档一致)
#define CLS_PT_NONE  0   // 非巡检 / 无待测点位
#define CLS_PT_C     1   // 中心
#define CLS_PT_LF    2   // 左前
#define CLS_PT_RF    3   // 右前
#define CLS_PT_LB    4   // 左后
#define CLS_PT_RB    5   // 右后
#define CLS_PT_COUNT 5

// 巡检任务状态
#define INSP_ST_RUN  0   // 进行中
#define INSP_ST_DONE 1   // 已完成
#define INSP_ST_STOP 2   // 已中止

#define INSP_MAX_TASKS 20   // NVS 任务上限 (记录上限 80 条 ≈ 16 次巡检, 任务不会先满)

// 教室级汇总判定 (M2: 设备端统一判定, 供网页/OLED/报告复用)
#define CLS_SUM_NONE  0   // 点位未测全, 暂无教室级结论
#define CLS_SUM_PASS  1   // A 教室合规 (5 点全部合规)
#define CLS_SUM_LOCAL 2   // B 教室局部风险 (1~2 点异常)
#define CLS_SUM_ALL   3   // C 教室整体风险 (≥3 点异常)

// 整改建议位掩码 (位序即网页展示顺序; 文案由网页/报告按位渲染)
#define CLS_ADV_PASS       0x0001  // 5 点全部合规 → 保持现状 + 定期复测
#define CLS_ADV_PST        0x0002  // 有点位 Pst^LM > 1
#define CLS_ADV_SVM        0x0004  // 有点位 SVM > 0.4
#define CLS_ADV_DARK       0x0008  // 有点位光线过暗
#define CLS_ADV_BRIGHT     0x0010  // 有点位光线过亮
#define CLS_ADV_MFREQ      0x0020  // 波动深度大但 Pst/SVM 合规 (高频调制)
#define CLS_ADV_ALL        0x0040  // ≥3 点异常 → 排查共性供电/驱动
#define CLS_ADV_CENTER     0x0080  // 中心异常而四角正常
#define CLS_ADV_LOCAL      0x0100  // 1~2 点局部异常
#define CLS_ADV_INCOMPLETE 0x0200  // 点位未测全, 结论不完整

// 教室级汇总 (由记录表按 taskId 归组实时计算, 不落 NVS)
typedef struct {
    uint16_t taskId;       // 所属巡检任务
    uint8_t  points;       // 已测点数 0..5
    uint8_t  passCnt;      // 合规点数
    uint8_t  failCnt;      // 异常点数
    uint8_t  darkCnt;      // 光线过暗点数
    uint8_t  brightCnt;    // 光线过亮点数
    uint8_t  pstFailCnt;   // Pst^LM 超标点数
    uint8_t  svmFailCnt;   // SVM 超标点数
    uint8_t  worstCode;    // 最差点位 CLS_PT_* (0 = 无异常)
    uint8_t  verdict;      // CLS_SUM_*
    uint16_t advice;       // CLS_ADV_* 位掩码
} ClassroomSummary;

typedef struct {
    uint32_t ts;                 // 测量时刻 (开机秒数)
    char     label[32];          // 灯具标签 (UTF-8)
    float    mMean;              // 波动深度均值 % (判定用)
    float    mMax;               // 波动深度峰值 % (展示用)
    float    fi;                 // Flicker Index 均值
    float    pstMean;            // Pst^LM 均值 (判定用)
    float    pstMax;             // Pst^LM 峰值 (展示用)
    float    freq;               // 主频 Hz (取频谱能量最大窗口)
    uint8_t  verdict;            // FV_* 综合判定码 (v1.1: 光照+Pst^LM+SVM 口径; 旧记录为旧口径)
    uint16_t windows;            // 测量窗口数 (v0.7.1 旧记录无此字段, 读到的填充字节恒 0, 载入时补 30s 档)
    float    svmMean;            // SVM 均值 (判定用 ≤0.4; 旧记录为 0 → 网页显示 --)
    float    svmMax;             // SVM 峰值 (仅展示)
    uint16_t vavgMean;           // 光照均值 ADC 码 (v1.1 判定前置量程检查用; 旧记录为 0 → 网页显示 --)
    uint32_t epoch;              // 真实测量时刻 Unix 秒 (浏览器传入或 NTP 校时; 0 = 未知, 旧记录无此字段)
    uint16_t taskId;             // 所属巡检任务号 (v1.6; 0 = 非巡检的普通测量)
    uint8_t  pointCode;          // 巡检点位码 CLS_PT_* (v1.6; 0 = 无点位)
} ClassroomRecord;

// 巡检任务 (一个教室一次巡检; NVS 持久化, 断电接着测)
typedef struct {
    uint16_t id;              // 任务号 (自增, 1..)
    uint8_t  status;          // INSP_ST_*
    uint8_t  mask;            // 已完成点位掩码 (bit0..bit4 = C/LF/RF/LB/RB)
    uint8_t  verdict;         // 教室级判定 CLS_SUM_* (0xFF = 尚未汇总; 每点完成/结束时刷新)
    uint8_t  pointsCount;     // 已完成点位数 (0..5, 由 mask 派生)
    uint32_t epoch;           // 创建时刻 Unix 秒 (0 = 未知)
    char     room[24];        // 教室号 (UTF-8)
    char     scene[16];       // 场景类型 (UTF-8)
    char     inspector[16];   // 巡检人 (UTF-8, 可空)
} InspectionTask;

// 初始化: 从 NVS 加载历史记录 (按 blob 长度兼容 v0.7.1 64B / v0.8~v0.9 72B / v1.1 76B /
// v1.5 80B / 当前 84B) + 巡检任务 (namespace "insp")
void classroomBegin(void);

// 开始一次测量 (运行中或记录已满返回 false)
// label 为 NULL/空串时自动命名 "灯具 N"; windows 取 CLS_WINDOWS / _1MIN / _10MIN 之一
// epoch: 本次测量的真实 Unix 秒 (网页传浏览器时间); 传 0 表示未知, 完成时回落到 NTP 校时值
bool classroomStart(const char *label, uint16_t windows, uint32_t epoch = 0);

// 是否正在测量
bool classroomIsRunning(void);

// 本次测量已完成的窗口数 (0~目标窗口数)
uint32_t classroomWindowsDone(void);

// 本次测量的目标窗口数
uint16_t classroomWindowsTarget(void);

// 当前测量标签 (仅测量运行中调用有意义, OLED 教室页显示用)
const char *classroomLabel(void);

// 每个报告窗口调用一次; 完成最后一次测量时返回 true (结果已入库)
bool classroomOnWindow(const FlickerSnapshot *s);

// 取走 "最近一次测量完成" 标志 (供网页推送, 仅返回一次)
bool classroomPopResult(ClassroomRecord *out);

// 记录列表 (内存缓存, 与 NVS 同步)
const ClassroomRecord *classroomRecords(uint16_t *n);

// 清空全部记录 (NVS + 内存) 与全部巡检任务 (记录清空后任务归属无意义)
void classroomClear(void);

// ---------------- 巡检点位名 ----------------
const char *classroomPointCode(uint8_t code);   // CLS_PT_* → "C"/"LF"/"RF"/"LB"/"RB" ("--" for NONE)
const char *classroomPointName(uint8_t code);   // CLS_PT_* → "中心"/"左前"/"右前"/"左后"/"右后"

// ---------------- 巡检任务层 (v1.6) ----------------
// 新建巡检任务并置为当前 (已有进行中任务 / 任务已满 / 正在测量 → false)
bool classroomInspectCreate(const char *room, const char *scene, const char *inspector, uint32_t epoch);
// 自动编号新建巡检: 教室号自 "Z001" 起递增 (编号存 NVS 断电续号, 清空记录时重置), 场景记 "OLED"
// 守卫: 正在测量 / 已有进行中任务 / 任务已满 / 记录已满 → false
bool classroomInspectCreateAuto(uint32_t epoch);
// 下一个自动教室编号数值 (供 OLED 预览 "Z%03u")
uint16_t classroomInspectNextAutoNo(void);
// 中止当前任务 (status → INSP_ST_STOP)
bool classroomInspectAbort(void);

// 结束当前任务 (status → INSP_ST_DONE, 不要求 5 点齐全)
bool classroomInspectFinish(void);

// 是否有进行中的巡检任务
bool classroomInspectActive(void);

// 当前任务号 (无任务返回 0)
uint16_t classroomInspectTaskId(void);

// 当前任务 (无任务返回 NULL)
const InspectionTask *classroomInspectTask(void);

// 下一个待测点位码 (按 C→LF→RF→LB→RB 顺序; 5 点齐全或无任务返回 CLS_PT_NONE)
uint8_t classroomInspectNextPoint(void);

// 开始测量指定点位: 标签自动取 "<教室号>-<点位码>", 完成时记录归属当前任务
bool classroomInspectStartPoint(uint8_t pt, uint16_t windows, uint32_t epoch);

// 任务列表 (内存缓存, 与 NVS 同步)
const InspectionTask *classroomTasks(uint16_t *n);

// ---------------- 教室级汇总层 (M2) ----------------
// 按任务号汇总 5 点结果 (记录表按 taskId 归组, 同点位多次复测取最后一条)
// 判定沿用设计文档 9.2: 5 点齐全且全部合规 → A; 1~2 点异常且中心正常 → B;
// ≥3 点异常或中心异常 → C; 点位未测全 → CLS_SUM_NONE (不下"合规"结论)
uint8_t classroomSummarize(uint16_t taskId, ClassroomSummary *out);

// 汇总当前进行中的任务返回 true 并填充 out (无进行中任务 / 无待测点返回 false)
bool classroomSummaryActive(ClassroomSummary *out);

// 教室级判定 → 中文短结论 ("A 教室合规" / "B 教室局部风险" / "C 教室整体风险" / "--")
const char *classroomSummaryText(uint8_t verdict);
