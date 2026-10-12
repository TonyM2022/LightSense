// ---------------- OLED 显示层实现 (v1.2) ----------------
// U8g2 全缓冲 (F) 模式: 1KB RAM, 整帧 sendBuffer 无闪烁
// 刷新: 每报告窗口一次 (≈1.024s), 400kHz I2C 全帧传输 ≈25ms,
//       远小于单块周期 204.8ms, ADC DMA 硬件节拍不受影响
// 字库: u8g2_font_wqy12/wqy16_t_gb2312 (文泉驿全量 GB2312, 中文界面与网页一致)
// 综合判定短文案为判定码映射第五处 (新增判定码需同步: flicker zoneStr /
//       buildJson zoneNames / 前端 VNAME+VCLS / 本表 ZONE_OLED)
#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include "config.h"
#include "flicker.h"
#include "classroom.h"
#include "oled.h"

// SSD1309 专用构造器, HW I2C (clock=GPIO5 / data=GPIO4), I2C 模块无复位引脚
static U8G2_SSD1309_128X64_NONAME2_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);
static bool s_enabled = false;

// 综合判定中文短文案 (FV_* 码 0~7, 与前端 VNAME 同源; 屏窄用短词, 1/4 为旧记录码兜底)
static const char *const ZONE_OLED[] = {
    "合规 PASS", "超标", "Pst超标", "双超标", "超标", "SVM超标", "过暗", "过亮"
};

bool oledBegin(void)
{
    Wire.begin(OLED_SDA, OLED_SCL);       // 总线初始化 (U8g2 内部复用同一 Wire)

    // I2C 地址探测: 0x3C 常规 / 0x3D 备用; 未找到模块则禁用, 不影响主功能
    bool found = false;
    const uint8_t addrs[2] = {OLED_ADDR, 0x3D};
    for (uint8_t i = 0; i < 2 && !found; i++)
    {
        Wire.beginTransmission(addrs[i]);
        found = (Wire.endTransmission() == 0);
    }
    if (!found)
    {
        Serial.println("OLED   : not found on I2C bus, display disabled.");
        return false;
    }

    u8g2.begin();
    u8g2.setBusClock(400000);             // 400kHz: 全帧传输 ≈25ms
    s_enabled = true;
    Serial.println("OLED   : SSD1309 128x64 ready.");
    return true;
}

// ---------------- 主页: 综合判定 + 实时频闪指标 ----------------
static void drawMainPage(const FlickerSnapshot *snap)
{
    char buf[32];

    // 行1 综合判定 (wqy16 居中; 超标时双线框强调)
    u8g2.setFont(u8g2_font_wqy16_t_gb2312);
    const char *zone = (snap->zone < 8) ? ZONE_OLED[snap->zone] : "?";
    uint16_t w = u8g2.getUTF8Width(zone);
    u8g2.drawUTF8((128 - w) / 2, 15, zone);
    u8g2.drawFrame(0, 0, 128, 18);
    if (snap->zone != FV_PASS)
        u8g2.drawFrame(1, 1, 126, 16);

    u8g2.setFont(u8g2_font_wqy12_t_gb2312);

    // 行2 波动深度 / 主频 (仅展示)
    snprintf(buf, sizeof(buf), "m %.1f%%", snap->flickerPercent);
    u8g2.drawUTF8(4, 33, buf);
    snprintf(buf, sizeof(buf), "Fq %.1fHz", snap->freqHz);
    u8g2.drawUTF8(66, 33, buf);

    // 行3 Pst^LM / SVM
    snprintf(buf, sizeof(buf), "Pst %.1f", snap->pstLM);
    u8g2.drawUTF8(4, 47, buf);
    snprintf(buf, sizeof(buf), "SVM %.2f", snap->svm);
    u8g2.drawUTF8(66, 47, buf);

    // 行4 光照均值 / Flicker Index
    snprintf(buf, sizeof(buf), "ADC %.0f", snap->vavg);
    u8g2.drawUTF8(4, 61, buf);
    snprintf(buf, sizeof(buf), "FI %.2f", snap->flickerIndex);
    u8g2.drawUTF8(66, 61, buf);
}

// ---------------- 教室页: 测量进行中 ----------------
static void drawClassroomPage(void)
{
    char buf[48];
    uint32_t done = classroomWindowsDone();
    uint16_t target = classroomWindowsTarget();
    if (target == 0) target = 1;

    // 行1 标题
    u8g2.setFont(u8g2_font_wqy16_t_gb2312);
    u8g2.drawUTF8(4, 16, "教室测量中");

    // 行2 灯具标签 (网页 maxlength=20 字节, wqy12 最宽 ~120px 不溢出)
    u8g2.setFont(u8g2_font_wqy12_t_gb2312);
    u8g2.drawUTF8(4, 34, classroomLabel());

    // 行3 进度条
    u8g2.drawFrame(4, 38, 120, 10);
    uint32_t fill = done * 120UL / target;
    if (fill > 120) fill = 120;
    if (fill > 0) u8g2.drawBox(4, 38, (uint16_t)fill, 10);

    // 行4 窗口计数 + 剩余时间 (每窗 1.024s)
    uint32_t remS = (done < target) ? ((target - done) * 1024UL / 1000UL) : 0;
    char rem[12];
    if (remS >= 60) snprintf(rem, sizeof(rem), "%lu:%02lu", (unsigned long)(remS / 60), (unsigned long)(remS % 60));
    else            snprintf(rem, sizeof(rem), "%lus", (unsigned long)remS);
    snprintf(buf, sizeof(buf), "%lu/%u  剩余 %s", (unsigned long)done, target, rem);
    u8g2.drawUTF8(4, 62, buf);
}

void oledShow(const FlickerSnapshot *snap)
{
    if (!s_enabled) return;
    u8g2.clearBuffer();
    if (classroomIsRunning())
        drawClassroomPage();
    else
        drawMainPage(snap);
    u8g2.sendBuffer();
}
