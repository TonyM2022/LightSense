#pragma once
// ---------------- 教室灯光测量层 ----------------
// 多灯具依次测量: 时长可选 30s / 1min / 10min 档, 逐窗口聚合指标,
// 判定 (v1.1): 光照量程前置否决 + Pst^LM 均值 ≤ 1 + SVM 均值 ≤ 0.4
// (波动深度/主频/光照仅展示或检查不参与双指标); 记录 NVS 持久化, 断电不丢
#include <stdint.h>
#include "flicker.h"

#define CLS_MAX_RECORDS     50   // NVS 最大记录数 (满后需导出 CSV 并清空)
#define CLS_WINDOWS         29   // 30s 档窗口数 (29 × 1.024s ≈ 29.7s)
#define CLS_WINDOWS_1MIN    58   // 1min 档窗口数 (58 × 1.024s ≈ 59.4s)
#define CLS_WINDOWS_10MIN   586  // 10min 档窗口数 (586 × 1.024s ≈ 600.1s)

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
} ClassroomRecord;

// 初始化: 从 NVS 加载历史记录 (按 blob 长度兼容 v0.7.1 64B / v0.8~v0.9 72B / 当前 74B)
void classroomBegin(void);

// 开始一次测量 (运行中或记录已满返回 false)
// label 为 NULL/空串时自动命名 "灯具 N"; windows 取 CLS_WINDOWS / _1MIN / _10MIN 之一
bool classroomStart(const char *label, uint16_t windows);

// 是否正在测量
bool classroomIsRunning(void);

// 本次测量已完成的窗口数 (0~目标窗口数)
uint32_t classroomWindowsDone(void);

// 本次测量的目标窗口数
uint16_t classroomWindowsTarget(void);

// 每个报告窗口调用一次; 完成最后一次测量时返回 true (结果已入库)
bool classroomOnWindow(const FlickerSnapshot *s);

// 取走 "最近一次测量完成" 标志 (供网页推送, 仅返回一次)
bool classroomPopResult(ClassroomRecord *out);

// 记录列表 (内存缓存, 与 NVS 同步)
const ClassroomRecord *classroomRecords(uint16_t *n);

// 清空全部记录 (NVS + 内存)
void classroomClear(void);
