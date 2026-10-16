#pragma once
// ---------------- OLED 显示层 (v1.2 显示 / v1.3 按键交互) ----------------
// 1.54" 128x64 OLED, SSD1309 驱动, IIC 接口 (板载 SDA=GPIO4 / SCL=GPIO5, 见 config.h)
// 三页面 = 网页端三页签 (频闪仪 / LED 控制 / 教室测量), 顶部 tab 栏, 左右键循环切换
// 按键 (v1.3): 左右全局切页; 上下页内光标 (LED 模式预选 / 教室时长选择); 确认执行
//   页1 频闪仪: 仅展示, 上下/确认无动作;  页3 运行中: 进度显示, 确认忽略防误触
// 绘制全部在主循环上下文 (oledShow 例行刷新 + oledHandleKey 事件立即重绘),
// u8g2 单任务访问; 消费 FlickerSnapshot 与 classroom/led_pwm 查询接口
#include <stdint.h>
#include "flicker.h"

// 初始化: I2C 地址探测 (0x3C/0x3D) + U8g2 初始化; 未接模块时返回 false 并禁用显示
bool oledBegin(void);

// 每报告窗口调用一次: 绘制当前页并整帧刷新 (未初始化成功时直接返回)
void oledShow(const FlickerSnapshot *snap);

// 按键事件处理 (v1.3, 主循环上下文调用): 更新页面/光标 → 执行动作 → 立即重绘
// ev 为 keypad.h 事件码 (KEY_UP/DOWN/OK/LEFT/RIGHT); 未接模块时直接返回
void oledHandleKey(uint8_t ev);
