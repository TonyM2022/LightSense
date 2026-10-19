# LightSense — ESP32-S3 频闪仪 (Flicker Meter)

## 项目简介

**面向中小学教室照明频闪健康风险的低成本快速检测系统**

随着我国持续推进儿童青少年近视防控和教室照明改造，越来越多学校使用 LED 照明设备。
然而，亮度达标并不代表视觉健康达标，部分灯具仍可能存在不易察觉的频闪问题，长期暴露
可能影响视觉舒适度和学习体验。现有专业频闪检测设备价格昂贵、操作复杂，难以在学校
普及应用。

本项目开发了一套基于 ESP32-S3 的低成本便携式教室照明频闪健康风险检测系统。系统通过
高速采集光信号波形，结合 FFT 频谱分析技术，实时计算波动深度（Percent Flicker）、
Flicker Index、Pst^LM 和 SVM 等国际通用频闪评价指标，并依据相关标准自动完成健康风险
判定。检测结果可通过 OLED 屏和手机浏览器实时显示，同时支持教室多点测量、数据存储和
CSV 导出。

**项目创新点：**

1. **低成本专业级检测** —— 采用低成本硬件实现接近专业频闪仪的核心检测能力，大幅降低校园检测门槛；
2. **指标直观化** —— 将复杂的国际标准指标转换为直观的健康风险等级，实现非专业人员快速使用；
3. **标准闪烁源自校验** —— 集成标准 LED 频闪源用于自校验，提高测量可靠性；
4. **现场巡检与数据留存** —— 支持教室现场快速巡检和数据留存，为学校照明改造和健康用光管理提供数据依据。

本项目旨在构建一套"看得见、测得到、用得起"的校园照明健康检测工具，助力创建更加
健康、安全的学习环境。

基于 ESP32-S3 的光源频闪（Temporal Light Artifact, TLA）测量与合规判定仪表。
通过 ADC 高速采样光传感器的光输出波形，实时计算 **波动深度 (Percent Flicker)**、
**Flicker Index**、**Pst^LM**（IEC TR 61547-1）、**SVM**（IEC TR 63158）等指标，
按 **光照量程检查 + Pst^LM ≤ 1 + SVM ≤ 0.4** 口径自动判定，并提供 WiFi 网页仪表、
LED 标准闪烁源与教室灯光测量记录功能。

## 主要功能

| 功能 | 说明 |
|---|---|
| 高速光波形采样 | ESP32-S3 SAR ADC + DMA 连续采样，20 ksps / 12 bit，硬件时钟定节拍 |
| FFT 频谱分析 | 4096 点 Hann 窗 FFT，频率分辨率 4.88 Hz，频谱显示 4.9 Hz ~ 5 kHz（对数轴） |
| 波动深度 (m) | IEEE 1789 Percent Flicker，`m = 100%×(A−B)/(A+B)`，与国标"波动深度"公式相同 |
| Flicker Index | IEEE 1789 时域指标（均值线以上面积比），仅展示参考 |
| Pst^LM | IEC TR 61547-1 短时闪烁严重度谱估计，加权 4.9~80 Hz，`Pst = √(Σ(m_k·H(f_k))²)` |
| SVM | IEC TR 63158 频闪可见度，80~2000 Hz，`SVM = (Σ(C_m/T(f_m))^3.7)^(1/3.7)` |
| 综合判定 | 光照量程前置否决 + Pst^LM ≤ 1 + SVM ≤ 0.4（见下文判定标准） |
| WiFi 网页仪表 | 设备开 AP 热点，浏览器实时查看指标/波形/频谱，WebSocket 每窗口推送 |
| LED 标准闪烁源 | 板载外接 LED 输出 常亮/50/100/500/1000/5000 Hz 方波（占空比 50%），闭环自测 |
| 教室灯光测量 | 30 s / 1 min / 10 min 三档时长，多灯具打标签依次测量，NVS 断电保存 50 条，CSV 导出 |
| OLED 本地显示 + 五键交互 | 1.54" SSD1309 OLED（软件模拟 I2C）+ 5 键薄膜按键，三页面与网页页签一致，离线完成 LED 切换与教室测量 |
| OneNET 云端接入 | MQTT 接入 OneNET Studio 物模型平台（OneJSON），30 s 周期属性上云、教室记录事件补传（断点续传）、LED 远程控制；网页 `/setup` 配网，AP+STA 共存本地功能不受影响 |

## 硬件结构

### 器件与连接

```
                    ┌──────────────────────┐
  光传感器 (光电二极管/  │                      │
  光敏电阻+分压等) ────┤ GPIO1  (ADC1_CH0)    │
                    │                      │      D7       220~330Ω
                    │   ESP32-S3           ├──── GPIO17 ───[电阻]───►|── GND
                    │   (Wifiduino32S3,    │                        (外接 LED,
                    │    8MB Flash)        │ WiFi AP: LightSense     自测光源)
                    └──────────────────────┘ http://192.168.4.1
```

| 引脚/资源 | 用途 | 说明 |
|---|---|---|
| GPIO1 (ADC1_CH0) | 光信号输入 | 12 bit（0~4095），12 dB 衰减（满量程约 3.1 V）；仅 ADC1 支持 DMA 连续模式 |
| GPIO17 (D7) | LED 闪烁源输出 | LEDC 硬件 PWM，10 bit 分辨率；经 220~330 Ω 限流电阻接 LED |
| GPIO21 (D8) | 按键模块公共端 | 固件常置低电平（虚拟 GND），薄膜按键公共端接此脚 |
| GPIO4 / GPIO5 | OLED I2C SDA / SCL | 板载丝印 SDA/SCL；接 1.54" OLED（SSD1309，3.3 V 供电），软件模拟 I2C 驱动 |
| GPIO38~GPIO42 | 五键薄膜按键 | 左/右/上/下/OK，内部上拉，按键公共端接 GPIO21（虚拟 GND） |
| WiFi AP | 网页仪表 | SSID `LightSense`，密码 `lightsense`（见 `src/config.h` 可改） |
| USB 串口 | 调试输出 | 115200 bps，每报告窗口（≈1.024 s）输出一次统计 |

### 前端信号建议

- 传感器输出直接进 GPIO1，确保光照适中（ADC 码均值 400~3000 之间，见判定标准）；
- 可在靠近引脚处加 RC 低通（如 1 kΩ + 10 nF，截止约 16 kHz）：既满足 S/H 回充
  （20 ksps 采样间隔 50 µs ≈ 5τ），又对 5 kHz 以内的显示频带几乎无衰减（<0.4 dB）；
- 高阻传感器（如光敏电阻分压）建议加缓冲（运放跟随）后再接入。

## 软件架构

三层架构 + 网页层，主循环轮询 DMA 缓存 → 度量层处理 → 报告窗口填充快照 → 网页层广播：

```
main.cpp (编排层)
  │  初始化各层; 主循环: 按键事件 → 取 DMA 块 → flickerProcessBlock()
  │  → 报告窗口到 → flickerReport() → classroomOnWindow() → oledShow() → webdashBroadcast()
  ▼
┌──────────────────────────────────────────────────────────────┐
│ 驱动层    adc_dma.*   ADC DMA 连续采样 (20 ksps, 双缓冲块)      │
│           led_pwm.*   LEDC PWM 标准闪烁源 (7 种模式)            │
│           keypad.*    5 键薄膜按键 (轮询消抖 + 事件队列)         │
│           oled.*      OLED 显示 (三页交互, 与网页页签一致)       │
├──────────────────────────────────────────────────────────────┤
│ 度量层    flicker.*   窗口统计 + FFT + 指标计算 + 综合判定       │
│                       (flickerReport 每 5 块 ≈ 1.024 s 输出一次,│
│                        填充 FlickerSnapshot 网页快照)           │
├──────────────────────────────────────────────────────────────┤
│ 应用层    classroom.* 教室测量: 逐窗聚合 + 三档时长 + NVS 存储    │
│           cloud.*     OneNET 云端: 属性上报/记录补传/远程控制     │
├──────────────────────────────────────────────────────────────┤
│ 网页层    webdash.*   AP 热点 + AsyncWebServer + WebSocket 广播 │
│                       (内嵌单页面前端, 三页面切换)               │
└──────────────────────────────────────────────────────────────┘
```

### 信号处理链路（度量层）

1. **时域统计**：窗口内峰谷值 → 波动深度 m；均值线以上面积比 → Flicker Index；
2. **频谱**：每块 4096 点去直流 + Hann 窗 FFT，5 块幅度累加后归一化为单块平均；
3. **主瓣簇提取**：对局部极大值取 3 bin 幅度和÷2 恢复真实幅度（Hann 窗泄漏校正），
   抛物线插值确定真实频率后单次加权，避免非 bin 中心频率被相邻 bin 重复计入；
4. **Pst^LM**：`m_k = 2|X_k|/|X_DC|`（DC 基准 = 窗口实测均值 × N/2，Hann 等效直流幅度），
   人眼加权 H = 1/可见阈值（8.8 Hz 处 0.25% 正弦单独贡献 Pst = 1 为标定点），
   >35 Hz 阈值按闪烁仪二阶低通滚降外推（thr ∝ f²）至 80 Hz；
5. **SVM**：同一频谱取 80~2000 Hz 频段，可见阈 T(f) = 2.865×10⁻⁵·f^1.543 + 0.225，
   按 3.7 次方 Minkowski 范数求和。

## 判定标准（v1.1）

综合判定按以下顺序执行（频闪仪页与教室测量共用同一口径）：

```
① 光照量程前置检查 (窗口均值 ADC 码, 阈值 config.h 可调)
   vavg < 400   → 光线过暗 (过暗时 DC 基准过小, Pst/SVM 噪声底虚高)
   vavg > 3000  → 光线太亮 (接近 12 bit 满量程, 可能削顶引入假谐波)
   → 光照异常时一票否决, 不再进入 ② (双指标数据无效)

② 双指标判定 (光照正常时)
   Pst^LM ≤ 1   (IEC TR 61547-1, 50% 人群察觉阈)
   SVM    ≤ 0.4 (IEC TR 63158 / EU (EU) 2019/2020 生态设计限值)
```

### 判定码

| 码 | 名称 | 网页显示 | 含义 |
|---|---|---|---|
| 0 | PASS | 合规 PASS | 光照正常且双指标合格 |
| 1 | FAIL-M | 超标 · 波动深度 | 仅旧记录（v0.8 及以前口径），新逻辑不产出 |
| 2 | FAIL-PST | 超标 · Pst^LM | 仅 Pst^LM > 1 |
| 3 | FAIL-BOTH | 超标 · 双项 | Pst^LM + SVM 双超 |
| 4 | EXEMPT | 高频豁免 | 仅旧记录，新逻辑不产出 |
| 5 | FAIL-SVM | 超标 · SVM | 仅 SVM > 0.4 |
| 6 | DARK | 光线过暗 | ADC 均值 < 400 |
| 7 | BRIGHT | 光线太亮 | ADC 均值 > 3000 |

> 波动深度 m 与主频 freq 不参与判定，仅作展示（频谱 + 峰谷/主频卡片）。
> 频闪仪页判定用当前 1 s 窗值（逐窗刷新），教室测量用全窗均值（更稳定）。

### 判定依据背景

- **GB 40070-2021**《儿童青少年学习用品近视防控卫生要求》曾以波动深度表 4 限值为主
  （≤10 Hz → 0.1%；10~90 Hz → 0.01×f；90~3125 Hz → 0.032×f；>3125 Hz 豁免），
  与 IEEE Std 1789-2015 的 NOEL 线同源；v0.9 起本仪表判定切换为 Pst^LM + SVM 双指标，
  与教育部教室照明改造技术指引中"Pst^LM ≤ 1"口径及 EU 生态设计法规接轨。
- **Pst^LM**：IEC TR 61547-1 短时闪烁严重度（照明设备闪烁计权），标准统计窗口 10 分钟；
- **SVM**：IEC TR 63158 频闪可见度，SVM = 1 为 50% 人群可察觉频闪效应的阈值；
- **IEEE 1789-2015**：波动深度/Flicker Index 定义出处，提供 NOEL / Low Risk 推荐线。

## 项目文件结构

```
LightSense/
├── platformio.ini        # PlatformIO 配置 (固定 espressif32@6.10.0 / arduino 2.0.17)
├── src/
│   ├── main.cpp          # 编排层: 初始化 + 主循环 (DMA→FFT→报告→网页广播)
│   ├── config.h          # 全局配置: 引脚/采样率/FFT/判定阈值/WiFi/网页参数
│   ├── adc_dma.h/.cpp    # 驱动层: ADC1 DMA 连续采样 (IDF adc_digi 驱动)
│   ├── led_pwm.h/.cpp    # 驱动层: LEDC PWM 标准闪烁源 (模式表 + 硬件控制)
│   ├── flicker.h/.cpp    # 度量层: 窗口统计 + FFT 频谱 + Pst/SVM 计算 + 综合判定
│   ├── classroom.h/.cpp  # 应用层: 教室测量聚合 + 三档时长 + NVS 记录存储
│   ├── webdash.h/.cpp    # 网页层: AP 热点 + 异步服务器 + WebSocket + 内嵌前端
│   ├── keypad.h/.cpp     # 按键层: 5 键轮询消抖 + 事件队列
│   ├── oled.h/.cpp       # 显示层: U8g2 SSD1309 OLED (三页交互, 软件模拟 I2C)
│   ├── cloud.h/.cpp      # 云端层: OneNET MQTT 状态机 + 属性/事件上报 + LED 远程控制
│   ├── cloud_cfg.h.example # 云端 Token 生成方法示例 (凭据经 /setup 页存 NVS, 不入库)
├── readme.md             # 本文件
└── .gitignore            # 忽略 .pio/ 等构建产物
```

### 关键配置（src/config.h）

| 宏 | 默认值 | 说明 |
|---|---|---|
| `ADC_PIN` / `ADC_CHANNEL` | GPIO1 / ADC1_CH0 | 光信号输入 |
| `SAMPLE_RATE_HZ` | 20000 | ADC 采样率 |
| `FFT_SIZE` | 4096 | FFT 点数（分辨率 4.88 Hz） |
| `BLOCKS_PER_REPORT` | 5 | 报告窗口 ≈ 1.024 s |
| `ADC_DARK_LIMIT` | 400 | 判定光照下限（窗口均值 ADC 码） |
| `ADC_BRIGHT_LIMIT` | 3000 | 判定光照上限 |
| `WIFI_AP_SSID` / `WIFI_AP_PASS` | LightSense / lightsense | AP 热点 |
| `LED_PIN` | GPIO17 | LED 闪烁源输出 |
| `KEY_GND_PIN` | GPIO21 | 按键模块公共端（固件常置低电平，虚拟 GND） |
| `OLED_SDA` / `OLED_SCL` | 4 / 5 | OLED I2C 引脚（软件模拟，板载丝印 SDA/SCL） |
| `KEY_PIN_UP/DOWN/OK/LEFT/RIGHT` | 40/41/42/38/39 | 五键薄膜按键引脚（内部上拉，公共端接 `KEY_GND_PIN`） |
| `CLOUD_MQTT_HOST` / `CLOUD_MQTT_PORT` | mqtts.heclouds.com / 1883 | OneNET Studio MQTT 接入点 |
| `CLOUD_SNAPSHOT_SEC` / `CLOUD_RETRY_SEC` | 30 / 10 | 周期属性上报间隔 / 重连退避 |
| `CLOUD_PUB_ENABLE` / `CLOUD_REC_ENABLE` / `CLOUD_PROBE` | 1 / 1 / 0 | 云端开关（属性上报 / 记录补传 / 诊断探测，平时 0） |

## 使用说明

### 构建与烧录

1. 安装 [PlatformIO](https://platformio.org/)（VSCode 插件或 CLI）；
2. 构建：`python -m platformio run -e wifiduino32s3`（或 IDE → Upload）；
3. 烧录后打开串口监视器（115200 bps），设备将输出启动信息与逐窗口统计；
4. WiFi 连接热点 `LightSense`（密码 `lightsense`），浏览器访问 `http://192.168.4.1`。

> `platformio.ini` 已将构建目录重定向到 `~/.platformio` 下，避免 OneDrive 同步
> 目录的文件锁随机损坏 `.pio` 构建产物。

### 网页仪表（三个页面）

- **频闪仪**：实时卡片（波动深度 / Flicker Index / Pst^LM / SVM / 主频 / 综合判定 +
  ADC 码统计）+ 时域波形包络 + 对数轴频谱，WebSocket 每秒自动刷新；
- **LED 控制**：切换外接 LED 输出模式（常灭 / 常亮 / 50 / 100 / 500 / 1000 / 5000 Hz），
  用于闭环自测——把光传感器对准 LED，主频应跟随输出频率；
- **教室测量**：选择时长（30 s / 1 min / 10 min）与灯具标签（留空自动编号）开始测量，
  完成后显示结果与判定，记录断电保存（最多 50 条），可导出 CSV（带 BOM，Excel 直接打开）
  或清空。教室验收建议多点测量（中心 + 四角）并用标签区分。

### OLED 显示与按键交互（v1.3）

1.54" 128×64 SSD1309 OLED 接板载 I2C（VCC→3.3 V、SDA→GPIO4、SCL→GPIO5、共地），
驱动采用 U8g2 **软件模拟 I2C**（绕开 arduino-esp32 2.0.x 硬件 I2C 驱动在总线异常时
阻塞初始化的问题）。每报告窗口（≈1 s）例行刷新，按键操作立即重绘（响应约 0.2 s）。

五键薄膜按键（上/下/OK/左/右 = GPIO40/41/42/38/39，内部上拉，公共端接 GND）：

- **左/右键**：三个页面循环切换（频闪仪 → LED 控制 → 教室测量），与网页端页签一致；
- **频闪仪页**：综合判定（中文大字加框）+ 波动深度 / 主频 / Pst^LM / SVM / 光照均值 /
  Flicker Index 摘要（只读）；
- **LED 控制页**：上下键在 7 种模式间循环预选（左列 常灭→常亮→50→100 Hz，
  右列 500 Hz→1 kHz→5 kHz），OK 键应用，当前输出模式反白显示；
- **教室测量页**：上下键选时长（30 s / 1 min / 10 min），OK 键开始测量（灯具标签自动
  编号）；测量进行中显示标签 + 进度条 + 剩余时间，OK 键忽略（防误触），左右键仍可
  切页查看实时值；
- 未接 OLED 时屏幕黑屏，测量与按键功能不受影响。

### OneNET 云端接入（v1.4）

设备可接入移动 OneNET Studio 物模型平台（MQTT，`mqtts.heclouds.com:1883`），
实现数据上云与远程控制，全部通信遵循官方 OneJSON 协议：

1. **配网**：连接设备热点后访问 `http://192.168.4.1/setup`，依次填入 WiFi
   账号/密码（支持下拉选择在线扫描到的热点）、OneNET 产品 ID、设备名称、
   设备 Token（生成方法见 `src/cloud_cfg.h.example`），保存后立即生效；
   凭据存 NVS 断电不丢。AP+STA 共存，配置云端不影响本地仪表使用；
2. **属性上报**：联网后每 30 s 上报一次 `lux / pst / svm / freq / ripple /
   verdict / led` 七个属性（topic `$sys/{pid}/{设备名}/thing/property/post`，
   平台经 `.../post/reply` 应答校验结果）；
3. **教室记录补传**：教室测量记录经物模型**事件**通道上云（topic
   `$sys/{pid}/{设备名}/thing/event/post`，事件标识符 `record`，输出参数
   `data` 为记录 JSON 字符串，保留 3 位小数精度）；断网期间记录存本地，
   重连后按 NVS 位图逐条补传（断点续传）；
4. **LED 远程控制**：平台下发属性设置 `led`（0~6，对应 7 种模式）即可远程
   切换 LED 输出，设备执行后自动应答 `set_reply`；
5. **物模型配置要求**（平台侧需定义并发布）：

   | 标识符 | 类型 | 规格 | 说明 |
   |---|---|---|---|
   | lux | int32 | step 1 | 窗口光照均值（ADC 码） |
   | pst | float | step 0.01 | Pst^LM（步长务必 0.01，0.1 会被平台拒收） |
   | svm | float | step 0.01 | SVM |
   | freq | float | step 0.1, max 10000 | 主频 Hz |
   | ripple | float | step 0.01 | 波动深度 % |
   | verdict | enum | 0/2/3/5/6/7 | 综合判定码 |
   | led | int32 | rw, 0~100 | LED 远程控制 |
   | 事件 record | — | 输出参数 data: string 512 | 教室记录补传 |

   > float 属性的步长（step）定义了允许的小数位数，上报值超出步长精度会被
   > 平台以 `code 2254` 拒收；int 属性上报浮点同样被拒（`code 2211`）。

### 串口输出示例

```
------------------
Samples  : 20480
Max      : 3892
Min      : 154
Average  : 2023.4
Flicker  : 92.35 %
FlickerIx: 0.471
Pst (LM) : 24.1
SVM      : 1.512
Freq     : 50.02 Hz
Zone     : Light+Pst+SVM FAIL-BOTH
------------------
```

### HTTP API

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/` | 内嵌网页仪表 |
| WS | `/ws` | WebSocket，每报告窗口推送 JSON 快照（指标/波形/频谱/教室进度） |
| GET | `/led` | 查询当前 LED 模式；`?mode=off\|const\|50hz\|100hz\|500hz\|1khz\|5khz` 切换 |
| GET | `/api/classroom` | 教室测量状态 + 全部记录（JSON） |
| POST | `/api/classroom/start?label=xxx&dur=30\|60\|600` | 开始一次测量（dur 秒，缺省 30） |
| POST | `/api/classroom/clear` | 清空全部记录 |
| GET | `/setup` | 云端配网页面（WiFi 账号/密码、产品 ID、设备名、Token，存 NVS） |

### 自测参考值（LED 自测档，外接 LED 方波 50% 占空比）

| LED 模式 | 主频 | 波动深度 | Pst^LM | SVM | 预期判定 |
|---|---|---|---|---|---|
| 50 Hz | ≈50 Hz | ≈100% | ≈24 | ≈1.5 | 超标 · 双项 |
| 100 Hz | ≈100 Hz | ≈100% | ≈0 | ≈4.9 | 超标 · SVM |
| 500 Hz | ≈500 Hz | ≈100% | 0 | ≈2.0 | 超标 · SVM |
| 1 kHz | ≈1 kHz | ≈100% | 0 | ≈0.88 | 超标 · SVM |
| 5 kHz | ≈5 kHz | ≈100% | 0 | ≈0 | 合规 PASS |
| 常亮 | — | <1% | 0.02~0.1 | ≈0 | 合规 PASS |

> 1 kHz 以上肉眼看近似常亮，可用手机相机观察滚动条纹确认在闪烁。
> 传感器遮光时判定显示「光线过暗」，强光直射近满量程时显示「光线太亮」。

## 已知局限

1. **Pst^LM 为 1 s 窗值**：频闪仪页实时值为单窗口径（无平滑，读数有正常窗间抖动）；
   标准统计口径为 10 分钟，教室测量 10 min 档（586 窗均值）最接近标准口径；
2. **频率分辨率**：4.88 Hz bin 宽度使 <15 Hz 频段的主频提取精度有限；
3. **主频简化**：仅取频谱能量最大分量作代表频率，多频率成分叠加波形未逐分量判定
   （Pst/SVM 本身已对全频段分量求和，不受此限）；
4. **单测点**：只反映测点位置光照，教室验收应多点测量并用标签区分；
5. **SVM/Pst 未认证**：实现遵循标准算法但未经计量认证，适用于工程评估与横向对比，
   不替代实验室级频闪仪。

## 版本历史

| 版本 | 内容 |
|---|---|
| v0.6~v0.7 | DMA+FFT 基础测量、IEEE 指标、GB 40070 判定、分层重构、WiFi 网页仪表、LED 控制、教室测量 |
| v0.7.1 | FFT NaN 修复、对数轴频谱、统一判定 |
| v0.8 | SVM 测量、Pst^LM 加权/尺度/泄漏五处修正、教室测量三档时长 |
| v0.9 | 判定切换为 Pst^LM ≤ 1 + SVM ≤ 0.4（m/freq 仅展示） |
| v1.0 | 移除 EMA 频谱平滑与 10min 累积 Pst 显示 |
| v1.1 | 综合判定增加光照量程前置检查（过暗/过亮否决） |
| v1.2 | 1.54" OLED 本地显示（判定 + 指标摘要 / 教室进度页）；切换 8MB 分区表 |
| v1.3 | 五键薄膜按键 + OLED 三页交互（页面/光标/时长选择，离线完成 LED 切换与教室测量）；OLED 改软件模拟 I2C，修复 2.0.17 硬件 I2C 驱动总线卡死；ADC 溢出自愈；判定框自适应宽度 |
| v1.4 | OneNET 云端层：MQTT 属性上报（30 s）/ 教室记录事件补传（NVS 位图断点续传）/ LED 远程控制；网页 /setup 配网（WiFi 扫描下拉 + 255 字符 Token）；按 OneJSON 官方协议对齐（property/post + event/post + set），修复 lux int32 类型、pst 步长校验、记录位图 NVS 持久化等问题 |

## 参考来源

- IEC TR 61547-1（Pst^LM 短时闪烁严重度，照明设备）
- IEC TR 63158（SVM 频闪可见度计权）
- GB 40070-2021《儿童青少年学习用品近视防控卫生要求》（波动深度限值背景）
  [标准解读 — 国家卫健委疾控局](https://www.nhc.gov.cn/jkj/c100062/202103/8d4befaf9721404da536521c8a9b73cf.shtml)
- IEEE Std 1789-2015（Percent Flicker / Flicker Index 定义，NOEL / Low Risk 线）
- EU (EU) 2019/2020 生态设计法规附例（SVM ≤ 0.4 / Pst^LM ≤ 1 限值）
- [Flicker Measurement and Standards — U.S. DOE](https://www.energy.gov/eere/ssl/flicker-measurement-and-standards)
