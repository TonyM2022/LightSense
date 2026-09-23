#pragma once
// ---------------- LED PWM 驱动层 ----------------
// LEDC 硬件 PWM 输出 (D8 = GPIO21): 生成精确的 LED 闪烁频率,
// 时序由 LEDC 外设硬件保证, 不受主循环 / WiFi / ADC 干扰
#include <stdint.h>

#define LED_MODE_COUNT       6

// 初始化 LEDC 并默认输出常亮
bool ledPwmBegin(void);

// 切换输出模式: 0=常亮(100%) 1=50Hz 2=100Hz 3=500Hz 4=1kHz 5=5kHz (其余 50% 占空比)
void ledPwmSetMode(uint8_t idx);

// 当前模式索引
uint8_t ledPwmGetMode(void);

// 模式显示名 ("常亮" / "100 Hz" / ...)
const char *ledPwmModeName(uint8_t idx);

// 模式 URL 参数 ("const" / "100hz" / ...), idx 越界返回 nullptr
const char *ledPwmModeParam(uint8_t idx);
