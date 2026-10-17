// ---------------- 薄膜按键层实现 (v1.3) ----------------
// 轮询任务: 10ms 采样, 连续 2 拍一致才确认状态翻转 (20ms 消抖, 覆盖薄膜开关抖动),
// 按下沿产生事件入队 (深度 4); 释放不产生事件
// 任务仅负责采键与发事件, 不触碰 OLED/classroom —— 所有交互响应在主循环上下文
// (oledHandleKey) 执行, 保证 u8g2 与教室状态单任务访问, 无竞态
#include <Arduino.h>
#include "config.h"
#include "keypad.h"

static const uint8_t KEY_PINS[5] = {KEY_PIN_UP, KEY_PIN_DOWN, KEY_PIN_OK,
                                    KEY_PIN_LEFT, KEY_PIN_RIGHT};
static QueueHandle_t s_keyQueue = nullptr;

static void keypadTask(void *)
{
    bool stable[5]  = {false, false, false, false, false};   // 稳定态 (true=按下)
    bool pending[5] = {false, false, false, false, false};   // 消抖中的候选态

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
        for (int i = 0; i < 5; i++)
        {
            bool pressed = (digitalRead(KEY_PINS[i]) == LOW);
            if (pressed == stable[i])
            {
                pending[i] = pressed;                  // 与稳定态一致, 复位消抖窗
            }
            else if (pressed == pending[i])
            {
                stable[i] = pending[i] = pressed;      // 连续 2 拍一致 → 翻转确认
                if (pressed)
                {
                    uint8_t ev = (uint8_t)(KEY_UP + i);
                    xQueueSend(s_keyQueue, &ev, 0);
                }
            }
            else
            {
                pending[i] = pressed;                  // 首拍变化, 下一拍再确认
            }
        }
    }
}

void keypadBegin(void)
{
    pinMode(KEY_GND_PIN, OUTPUT);
    digitalWrite(KEY_GND_PIN, LOW);         // 虚拟 GND: 按键模块公共端 (GPIO21 常置低)
    for (int i = 0; i < 5; i++)
        pinMode(KEY_PINS[i], INPUT_PULLUP);
    s_keyQueue = xQueueCreate(4, sizeof(uint8_t));
    xTaskCreate(keypadTask, "keypad", 2048, nullptr, 1, nullptr);
    Serial.println("Keypad  : 5 keys ready (UP/DOWN/OK/LEFT/RIGHT).");
}

uint8_t keypadGetEvent(void)
{
    uint8_t ev = KEY_NONE;
    if (s_keyQueue) xQueueReceive(s_keyQueue, &ev, 0);
    return ev;
}
