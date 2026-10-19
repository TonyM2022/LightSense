#pragma once
// ---------------- OneNET 云端层 ----------------
// WiFi AP+STA 联网 + PubSubClient MQTT, 与 OneNET Studio 对接:
//   上行: 周期属性快照 (30s 节流) + 教室记录逐条补传 (断网缓存于 NVS, 联网后同步)
//   下行: 属性设置 {"params":{"led":N}} 远程切换 LED 模式
// 全部操作非阻塞, 不触碰 204.8ms 的 ADC 块节拍 (MQTT connect 限退避 ≥10s)
#include "flicker.h"
#include "classroom.h"

// 云端连接状态 (网页快照 cloud 字段 / 串口日志用)
enum CloudState {
    CL_OFF = 0,        // 未配置 (未填 WiFi/Token)
    CL_WAIT_WIFI = 1,  // 已配置, 等待 STA 连上路由器
    CL_CONNECTING = 2, // WiFi 已连, MQTT 握手中
    CL_ONLINE = 3,     // 已订阅下行 + 周期上报中
};

// 初始化: 读 NVS 配置; 已配置则切 AP+STA 并发起 WiFi 连接
// 必须在 webdashBegin() 之前调用 (webdash 检测模式避免覆盖)
void cloudBegin(void);

// 主循环每圈调用: 状态机推进 + 节流上报 + 记录补传 + 下行泵, 全非阻塞
void cloudTick(void);

// 每报告窗口调用一次, 缓存最新快照供周期上报
void cloudOnSnapshot(const FlickerSnapshot *s);

// 教室记录入库后调用, 进入待同步队列 (联网后补传, 成功置 NVS 同步位图)
void cloudPushRecord(const ClassroomRecord *r, uint16_t idx);

// 教室记录清空后调用: 同步位图归零 (记录索引重新从 0 对应)
void cloudResetSync(void);

CloudState cloudGetState(void);
const char *cloudStateName(void);

// 读取当前配置 (网页 /api/cloud 回显用), 返回是否已配置
bool cloudCfgGet(char *ssid, size_t ss, char *pass, size_t ps,
                 char *pid, size_t pp, char *dev, size_t ds, char *tok, size_t ts);

// 保存配置到 NVS (调用方应提示重启后生效)
bool cloudCfgSave(const String &ssid, const String &pass, const String &pid,
                  const String &dev, const String &tok);
