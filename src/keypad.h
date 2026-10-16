#pragma once
// ---------------- 薄膜按键层 (v1.3) ----------------
// 5 键独立 GPIO (上/下/确认/左/右, 引脚见 config.h), 内部上拉, 公共端接 GND, 按下为低
// 后台任务 10ms 轮询消抖 (20ms), 按下沿产生事件; 主循环非阻塞取事件
// 事件仅在按下瞬间产生一次 (不支持长按连发, 交互需求无此项)
#include <stdint.h>

enum {
    KEY_NONE = 0,
    KEY_UP,          // 上移
    KEY_DOWN,        // 下移
    KEY_OK,          // 确认
    KEY_LEFT,        // 左移 (全局切页: 上一页)
    KEY_RIGHT        // 右移 (全局切页: 下一页)
};

// 初始化 5 个引脚 (INPUT_PULLUP) 并启动轮询消抖任务 (2KB 栈)
void keypadBegin(void);

// 非阻塞取一个按键事件 (主循环每圈调用); 无事件返回 KEY_NONE
uint8_t keypadGetEvent(void);
