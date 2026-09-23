// ---------------- LED PWM 驱动层实现 ----------------
// arduino-esp32 2.0.x LEDC API: ledcSetup / ledcAttachPin / ledcWrite
#include <Arduino.h>
#include "config.h"
#include "led_pwm.h"

// 模式表 (URL 参数与 LightSource 项目保持一致, "off" 为本项目扩展)
// freq = 0 表示恒定电平: 脱离 LEDC 直接驱动 GPIO (const=高电平常亮, off=低电平常灭)
static const struct
{
    const char *name;    // 显示名
    const char *param;   // URL 参数
    uint32_t freq;       // 输出频率 (Hz)
} MODES[LED_MODE_COUNT] = {
    {"常灭",   "off",   0},
    {"常亮",   "const", 0},
    {"50 Hz",  "50hz",  50},
    {"100 Hz", "100hz", 100},
    {"500 Hz", "500hz", 500},
    {"1 kHz",  "1khz",  1000},
    {"5 kHz",  "5khz",  5000},
};

static uint8_t curMode = 0;

bool ledPwmBegin(void)
{
    ledcSetup(LED_PWM_CHANNEL, 5000, LED_PWM_RESOLUTION);
    ledcAttachPin(LED_PIN, LED_PWM_CHANNEL);
    ledPwmSetMode(0);                    // 默认常灭 (1=常亮)
    return true;
}

void ledPwmSetMode(uint8_t idx)
{
    if (idx >= LED_MODE_COUNT)
        return;
    curMode = idx;

    if (MODES[idx].freq == 0)
    {
        // 恒定电平: 脱离 LEDC, const 输出高 (常亮), off 输出低 (常灭)
        ledcDetachPin(LED_PIN);
        digitalWrite(LED_PIN, (strcmp(MODES[idx].param, "off") == 0) ? LOW : HIGH);
    }
    else
    {
        ledcAttachPin(LED_PIN, LED_PWM_CHANNEL);
        ledcSetup(LED_PWM_CHANNEL, (double)MODES[idx].freq, LED_PWM_RESOLUTION);
        ledcWrite(LED_PWM_CHANNEL, (1 << LED_PWM_RESOLUTION) / 2);   // 50%
    }

    Serial.print("LED mode: ");
    Serial.print(MODES[idx].name);
    Serial.print(" (");
    Serial.print(MODES[idx].param);
    Serial.println(")");
}

uint8_t ledPwmGetMode(void)
{
    return curMode;
}

const char *ledPwmModeName(uint8_t idx)
{
    return (idx < LED_MODE_COUNT) ? MODES[idx].name : "";
}

const char *ledPwmModeParam(uint8_t idx)
{
    return (idx < LED_MODE_COUNT) ? MODES[idx].param : nullptr;
}
