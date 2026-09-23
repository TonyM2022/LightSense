#pragma once
// ---------------- 教室灯光测量层 ----------------
// 多灯具依次测量: 每次固定 29 个报告窗口 (≈29.7s), 逐窗口聚合指标,
// 按 GB 40070-2021 判定 (波动深度均值 vs 表4 限值 + Pst^LM 均值 ≤ 1),
// 记录 NVS 持久化, 断电不丢
#include <stdint.h>
#include "flicker.h"

#define CLS_MAX_RECORDS     50   // NVS 最大记录数 (满后需导出 CSV 并清空)
#define CLS_WINDOWS         29   // 单次测量窗口数 (29 × 1.024s ≈ 29.7s)

// 判定码
#define CLS_PASS            0    // 合规 (含波动深度 <1% 的噪声)
#define CLS_FAIL_M          1    // 超标: 波动深度
#define CLS_FAIL_PST        2    // 超标: Pst^LM > 1
#define CLS_FAIL_BOTH       3    // 超标: 波动深度 + Pst^LM
#define CLS_EXEMPT          4    // 高频豁免 (>3125 Hz)

typedef struct {
    uint32_t ts;                 // 测量时刻 (开机秒数)
    char     label[32];          // 灯具标签 (UTF-8)
    float    mMean;              // 波动深度均值 % (判定用)
    float    mMax;               // 波动深度峰值 % (展示用)
    float    fi;                 // Flicker Index 均值
    float    pstMean;            // Pst^LM 均值 (判定用)
    float    pstMax;             // Pst^LM 峰值 (展示用)
    float    freq;               // 主频 Hz (取频谱能量最大窗口)
    uint8_t  verdict;            // CLS_*
} ClassroomRecord;

// 初始化: 从 NVS 加载历史记录
void classroomBegin(void);

// 开始一次测量 (运行中或记录已满返回 false)
// label 为 NULL/空串时自动命名 "灯具 N"
bool classroomStart(const char *label);

// 是否正在测量
bool classroomIsRunning(void);

// 本次测量已完成的窗口数 (0~CLS_WINDOWS)
uint32_t classroomWindowsDone(void);

// 每个报告窗口调用一次; 完成最后一次测量时返回 true (结果已入库)
bool classroomOnWindow(const FlickerSnapshot *s);

// 取走 "最近一次测量完成" 标志 (供网页推送, 仅返回一次)
bool classroomPopResult(ClassroomRecord *out);

// 记录列表 (内存缓存, 与 NVS 同步)
const ClassroomRecord *classroomRecords(uint16_t *n);

// 清空全部记录 (NVS + 内存)
void classroomClear(void);
