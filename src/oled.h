#pragma once
// ---------------- OLED 显示层 (v1.2) ----------------
// 1.54" 128x64 OLED, SSD1309 驱动, IIC 接口 (板载 SDA=GPIO4 / SCL=GPIO5, 见 config.h)
// 主页: 综合判定 + 实时频闪指标 (每报告窗口刷新, ≈1.024s)
// 教室页: 测量进行中自动切换 (标签 + 进度条 + 剩余时间), 完成后自动回主页
// 纯只读显示: 消费 FlickerSnapshot 与 classroom 查询接口, 不影响测量/判定/网页
#include <stdint.h>
#include "flicker.h"

// 初始化: I2C 地址探测 (0x3C/0x3D) + U8g2 初始化; 未接模块时返回 false 并禁用显示
bool oledBegin(void);

// 每报告窗口调用一次: 绘制当前页并整帧刷新 (未初始化成功时直接返回)
void oledShow(const FlickerSnapshot *snap);
