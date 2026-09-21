#pragma once
// ---------------- WiFi 网页仪表层 ----------------
// S3 AP 热点 + ESPAsyncWebServer + AsyncWebSocket, 推送 JSON 快照到浏览器
#include "flicker.h"

// 启动 AP 热点 + Web 服务器 (页面 / + WebSocket /ws), 串口打印访问地址
bool webdashBegin(void);

// 将最新快照广播给所有已连接的 WebSocket 客户端 (每报告窗口调用一次)
void webdashBroadcast(const FlickerSnapshot *snap);
