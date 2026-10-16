// ---------------- OLED 显示层实现 (v1.2 显示 / v1.3 三页按键交互) ----------------
// U8g2 全缓冲 (F) 模式: 1KB RAM, 整帧 sendBuffer 无闪烁
// 刷新: ① 每报告窗口 oledShow() 例行刷新 (≈1.024s)
//       ② 按键事件 oledHandleKey() 立即重绘 (响应 ≈0.2s, 不等报告窗口)
//       全部在主循环上下文执行, u8g2 单任务访问; SW I2C 全帧 ≈40~80ms,
//       远小于单块周期 204.8ms, ADC DMA 硬件节拍不受影响
// 字库: u8g2_font_wqy12/wqy16_t_gb2312 (文泉驿全量 GB2312, 与网页界面同语言)
// 综合判定短文案为判定码映射第五处 (新增判定码需同步: flicker zoneStr /
//       buildJson zoneNames / 前端 VNAME+VCLS / 本表 ZONE_OLED)
// 页面 = 网页端三页签; 按键: 左右全局切页 / 上下页内光标 / 确认执行 (v1.3)
#include <Arduino.h>
#include <U8g2lib.h>
#include "config.h"
#include "flicker.h"
#include "classroom.h"
#include "led_pwm.h"
#include "keypad.h"
#include "oled.h"

// SSD1309 专用构造器, SW I2C 软件模拟 (clock=GPIO5 / data=GPIO4), I2C 模块无复位引脚
// 弃用 HW I2C 的原因: arduino-esp32 2.0.17 (IDF 4.4 旧 I2C 驱动) 在总线被钳住
// (模块上电瞬态/上拉不足/接线瞬态) 时 i2c_master_cmd_begin 超时中断不触发,
// setup() 永久阻塞在 OLED 初始化; 软件位拍纯 GPIO 无驱动/无 ISR/无锁, 机制上不可能卡死
static U8G2_SSD1309_128X64_NONAME2_F_SW_I2C u8g2(U8G2_R0, OLED_SCL, OLED_SDA, U8X8_PIN_NONE);
static bool s_enabled = false;

// ---------------- 交互状态 (仅主循环上下文访问, 无需锁) ----------------
static uint8_t s_page   = 0;     // 当前页 0=频闪仪 1=LED 控制 2=教室测量
static uint8_t s_ledSel = 0;     // LED 页模式预选光标 (0..6, 确认前不改变输出)
static uint8_t s_durSel = 0;     // 教室页时长光标 (0=30s 1=1min 2=10min)

// 教室时长档 (窗口数/名称, 与网页端三档一致)
static const uint16_t DUR_WINDOWS[3] = {CLS_WINDOWS, CLS_WINDOWS_1MIN, CLS_WINDOWS_10MIN};
static const char *const DUR_NAMES[3] = {"30s", "1min", "10min"};

// 综合判定中文短文案 (FV_* 码 0~7, 与前端 VNAME 同源; 屏窄用短词, 1/4 为旧记录码兜底)
static const char *const ZONE_OLED[] = {
    "合规 PASS", "超标", "Pst超标", "双超标", "超标", "SVM超标", "过暗", "过亮"
};

bool oledBegin(void)
{
    // 软件模拟 I2C 无 ACK 反馈, 不做地址探测: 模块未接时位拍空总线无副作用,
    // 屏幕黑屏、主功能不受影响 (u8g2.begin 内部完成引脚方向配置与显示初始化)
    u8g2.begin();
    s_enabled = true;
    Serial.println("OLED   : SSD1309 128x64 ready (SW I2C, 3 pages).");
    return true;
}

// ---------------- 顶部 tab 栏 (三页签, 当前页反白, 对应网页端导航) ----------------
static void drawTabBar(void)
{
    static const char *const tabs[3] = {"频闪仪", "LED", "教室"};
    u8g2.setFont(u8g2_font_wqy12_t_gb2312);
    int x = 3;
    for (int i = 0; i < 3; i++)
    {
        int w = u8g2.getUTF8Width(tabs[i]);
        if (i == (int)s_page)
        {
            u8g2.drawBox(x - 3, 0, w + 6, 12);
            u8g2.setDrawColor(0);
            u8g2.drawUTF8(x, 10, tabs[i]);
            u8g2.setDrawColor(1);
        }
        else
        {
            u8g2.drawUTF8(x, 10, tabs[i]);
        }
        x += w + 14;
    }
    u8g2.drawHLine(0, 12, 128);
}

// ---------------- 页 1: 频闪仪 (判定 + 指标摘要, 上下/确认无动作) ----------------
static void drawMeterPage(const FlickerSnapshot *snap)
{
    char buf[32];

    // 综合判定 (wqy16 居中, 框随文本宽度自适应)
    u8g2.setFont(u8g2_font_wqy16_t_gb2312);
    const char *zone = (snap->zone < 8) ? ZONE_OLED[snap->zone] : "?";
    uint16_t tw = u8g2.getUTF8Width(zone);
    int bw = tw + 16;                     // 框 = 文本宽 + 左右 8px 内边距
    if (bw > 124) bw = 124;               // 不触碰屏幕最左/最右列: 1.54" 模块面板列对齐
                                          // 有偏差, 画满 0~127 列时右边缘会包绕到左侧
    int bx = (128 - bw) / 2;              // bx ≥ 2, 框右缘 ≤ 125
    u8g2.drawUTF8((128 - tw) / 2, 28, zone);
    u8g2.drawFrame(bx, 14, bw, 16);

    u8g2.setFont(u8g2_font_wqy12_t_gb2312);
    snprintf(buf, sizeof(buf), "m %.1f%%", snap->flickerPercent);
    u8g2.drawUTF8(4, 41, buf);
    if (snap->freqHz >= 1000.0f)          // ≥1kHz 换 kHz 单位, 防止 11 字符 "Fq 9999.9Hz" 压出屏边
        snprintf(buf, sizeof(buf), "Fq %.1fkHz", snap->freqHz / 1000.0f);
    else
        snprintf(buf, sizeof(buf), "Fq %.1fHz", snap->freqHz);
    u8g2.drawUTF8(66, 41, buf);
    snprintf(buf, sizeof(buf), "Pst %.1f", snap->pstLM);
    u8g2.drawUTF8(4, 52, buf);
    snprintf(buf, sizeof(buf), "SVM %.2f", snap->svm);
    u8g2.drawUTF8(66, 52, buf);
    snprintf(buf, sizeof(buf), "ADC %.0f", snap->vavg);
    u8g2.drawUTF8(4, 63, buf);
    snprintf(buf, sizeof(buf), "FI %.2f", snap->flickerIndex);
    u8g2.drawUTF8(66, 63, buf);
}

// ---------------- 页 2: LED 控制 (7 模式 2 列网格, 光标预选 + 确认应用) ----------------
// ▸ = 预选光标 (上下键移动); 反白 = 当前实际输出模式 (确认后与光标重合)
static void drawLedPage(void)
{
    u8g2.setFont(u8g2_font_wqy12_t_gb2312);
    uint8_t cur = ledPwmGetMode();

    for (uint8_t i = 0; i < LED_MODE_COUNT; i++)
    {
        int col = i / 4, row = i % 4;          // 网格列优先: 左列 0~3 / 右列 4~6 (光标遍历顺序同网格)
        int x = 12 + col * 62;
        int y = 25 + row * 12;
        const char *name = ledPwmModeName(i);
        if (i == cur)                          // 当前输出模式: 反白
        {
            int w = u8g2.getUTF8Width(name);
            u8g2.drawBox(x - 2, y - 11, w + 4, 12);
            u8g2.setDrawColor(0);
            u8g2.drawUTF8(x, y, name);
            u8g2.setDrawColor(1);
        }
        else
        {
            u8g2.drawUTF8(x, y, name);
        }
        if (i == s_ledSel)
            u8g2.drawStr(x - 10, y, ">");      // 预选光标
    }
}

// ---------------- 页 3: 教室测量 (空闲: 时长选择+开始 / 运行: 进度) ----------------
static void drawClassroomPage(void)
{
    char buf[48];
    u8g2.setFont(u8g2_font_wqy12_t_gb2312);

    if (classroomIsRunning())
    {
        // 运行中: 标签 + 进度条 + 计数/剩余 (确认键忽略, 左右仍可切页看实时值)
        u8g2.drawUTF8(4, 26, "教室测量中");
        u8g2.drawUTF8(4, 42, classroomLabel());

        uint32_t done = classroomWindowsDone();
        uint16_t target = classroomWindowsTarget();
        if (target == 0) target = 1;
        u8g2.drawFrame(4, 46, 120, 8);
        uint32_t fill = done * 120UL / target;
        if (fill > 120) fill = 120;
        if (fill > 0) u8g2.drawBox(4, 46, (uint16_t)fill, 8);

        uint32_t remS = (done < target) ? ((target - done) * 1024UL / 1000UL) : 0;
        char rem[12];
        if (remS >= 60) snprintf(rem, sizeof(rem), "%lu:%02lu", (unsigned long)(remS / 60), (unsigned long)(remS % 60));
        else            snprintf(rem, sizeof(rem), "%lus", (unsigned long)remS);
        snprintf(buf, sizeof(buf), "%lu/%u  剩余 %s", (unsigned long)done, target, rem);
        u8g2.drawUTF8(4, 62, buf);
        return;
    }

    // 空闲: 三档时长选择 (上下键循环, 反白光标) + 开始提示
    u8g2.drawUTF8(4, 26, "测量时长:");
    const int xs[3] = {4, 46, 94};
    for (int i = 0; i < 3; i++)
    {
        if (i == (int)s_durSel)
        {
            int w = u8g2.getUTF8Width(DUR_NAMES[i]);
            u8g2.drawBox(xs[i] - 2, 33, w + 4, 12);
            u8g2.setDrawColor(0);
            u8g2.drawUTF8(xs[i], 44, DUR_NAMES[i]);
            u8g2.setDrawColor(1);
        }
        else
        {
            u8g2.drawUTF8(xs[i], 44, DUR_NAMES[i]);
        }
    }
    u8g2.drawUTF8(4, 60, "OK 键: 开始测量");
}

// ---------------- 统一绘制入口 ----------------
static void drawAll(const FlickerSnapshot *snap)
{
    u8g2.clearBuffer();
    drawTabBar();
    switch (s_page)
    {
    case 0:  drawMeterPage(snap);  break;
    case 1:  drawLedPage();        break;
    default: drawClassroomPage();  break;
    }
    u8g2.sendBuffer();
}

// 每报告窗口例行刷新 (与网页广播同节拍)
void oledShow(const FlickerSnapshot *snap)
{
    if (!s_enabled) return;
    drawAll(snap);
}

// ---------------- 按键事件处理 (主循环上下文) ----------------
// 左右: 全局循环切页; 上下: 页内光标 (LED 同列行移动 / 教室时长循环); OK: 执行
void oledHandleKey(uint8_t ev)
{
    if (!s_enabled || ev == KEY_NONE) return;

    switch (ev)
    {
    case KEY_LEFT:
        s_page = (s_page + 2) % 3;
        break;
    case KEY_RIGHT:
        s_page = (s_page + 1) % 3;
        break;
    case KEY_UP:
    case KEY_DOWN:
        if (s_page == 1)                    // LED 网格: 上下键在 7 个模式间循环
        {                                   // (列优先: 先左列 0常灭/1常亮/2 50Hz/3 100Hz, 再右列 4 500Hz/5 1kHz/6 5kHz)
            s_ledSel = (uint8_t)((s_ledSel + ((ev == KEY_DOWN) ? 1 : LED_MODE_COUNT - 1)) % LED_MODE_COUNT);
        }
        else if (s_page == 2 && !classroomIsRunning())
        {
            s_durSel = (uint8_t)((s_durSel + ((ev == KEY_DOWN) ? 1 : 2)) % 3);
        }
        break;                              // 页 1 (频闪仪) 上下键无动作 (按需求)
    case KEY_OK:
        if (s_page == 1)
        {
            ledPwmSetMode(s_ledSel);        // 应用预选模式 (与网页 /led 同一接口)
        }
        else if (s_page == 2 && !classroomIsRunning())
        {
            classroomStart(NULL, DUR_WINDOWS[s_durSel]);   // NULL → 自动命名灯具 N
        }
        break;                              // 页 3 运行中确认忽略 (防误触重启)
    default:
        return;
    }

    drawAll(flickerSnapshot());             // 立即重绘 (不等报告窗口)
}
