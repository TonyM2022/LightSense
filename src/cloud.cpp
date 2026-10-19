// ---------------- OneNET 云端层实现 ----------------
// 状态机: CL_OFF → CL_WAIT_WIFI → CL_CONNECTING → CL_ONLINE (掉线回退)
// 防阻塞纪律:
//   - WiFi.begin/mqtt.connect 每次只在状态切换点执行一次, 失败退避 CLOUD_RETRY_SEC
//   - mqtt.connect 前必须确认 WiFi 已连上 (PubSubClient 掉线时 connect 会阻塞 socket 超时)
//   - mqtt.setSocketTimeout(4) 收紧 connect 阻塞上限 (默认 15s)
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include "config.h"
#include "flicker.h"
#include "classroom.h"
#include "led_pwm.h"
#include "cloud.h"

static Preferences prefs;
static WiFiClient netClient;
static PubSubClient mqtt(netClient);

// 连接参数 (cloudBegin 从 NVS 读入)
static char cfgSsid[33], cfgPass[65], cfgPid[25], cfgDev[25], cfgTok[257];
static bool cfgOk = false;

static CloudState st = CL_OFF;
static uint32_t lastMqttTry = 0, lastPub = 0, lastRecPub = 0;
static const FlickerSnapshot *lastSnap = NULL;

// MQTT topic (OneNET Studio OneJSON 物模型, 官方文档 detail/902)
static char upTopic[96];      // $sys/{pid}/{dev}/thing/property/post       属性上报 (OneJSON)
static char downTopic[96];    // $sys/{pid}/{dev}/thing/property/set        属性设置下行 (LED 远程控制)
static char recTopic[96];     // $sys/{pid}/{dev}/thing/event/post          教室记录补传 (物模型事件 record, 输出参数 data:string)
static char repTopic[112];    // $sys/{pid}/{dev}/thing/property/post/reply 属性上报应答 (平台校验结果)
static char evRepTopic[112];  // $sys/{pid}/{dev}/thing/event/post/reply    事件上报应答 (平台校验结果)
static char setRepTopic[112]; // $sys/{pid}/{dev}/thing/property/set_reply  属性设置应答
static char dpTopic[96];      // $sys/{pid}/{dev}/dp/post/json              老版数据点 (仅 CLOUD_PROBE=2 探测用)

// 教室记录待同步: 队列 (新记录优先) + NVS 位图 (断网期间历史记录补传)
#define CLOUD_PEND_CAP 8
static ClassroomRecord pendRec[CLOUD_PEND_CAP];
static uint16_t pendIdx[CLOUD_PEND_CAP];
static uint8_t pendHead = 0, pendTail = 0, pendCnt = 0;
static uint64_t syncMap = 0;
static bool backfillDone = false;

// ---------------- 小工具 ----------------
static size_t escJson(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    for (; *src && n + 7 < cap; src++)
    {
        if (*src == '"' || *src == '\\')
            dst[n++] = '\\';
        dst[n++] = *src;
    }
    dst[n] = 0;
    return n;
}

static void pushPend(const ClassroomRecord *r, uint16_t idx)
{
    if (pendCnt >= CLOUD_PEND_CAP)          // 队列满: 丢最旧, 保最新
    {
        pendHead = (pendHead + 1) % CLOUD_PEND_CAP;
        pendCnt--;
    }
    pendRec[pendTail] = *r;
    pendIdx[pendTail] = idx;
    pendTail = (pendTail + 1) % CLOUD_PEND_CAP;
    pendCnt++;
}

static void markSynced(uint16_t idx)
{
    syncMap |= (1ULL << idx);
    // cloudBegin 读完配置已 prefs.end(), 须重新打开 NVS 才能写入 (否则位图不持久化, 记录每轮重复补传)
    prefs.begin("cloud", false);
    prefs.putULong64("syncmap", syncMap);
    prefs.end();
}

// ---------------- MQTT 回调: 下行属性设置 ----------------
// OneNET 属性设置: {"id":"x","version":"1.0","params":{"led":N}} → 切 LED 并应答
static void onMqttMsg(char *topic, byte *payload, unsigned int len)
{
    Serial.print("Cloud  : down [");
    Serial.print(topic);
    Serial.print("] len=");
    Serial.println(len);                 // 调试: 观察平台是否在踢连接前下发消息
    char buf[256];
    unsigned c = (len < sizeof(buf) - 1) ? len : (unsigned)(sizeof(buf) - 1);
    memcpy(buf, payload, c);
    buf[c] = 0;
    Serial.print("Cloud  : payload ");
    Serial.println(buf);                 // 调试: 打印平台下行/应答内容 (校验错误码等)

    const char *p = strstr(buf, "\"led\"");
    if (!p)
        return;
    p = strchr(p + 5, ':');
    if (!p)
        return;
    long m = strtol(p + 1, NULL, 10);
    if (m < 0 || m >= (long)LED_MODE_COUNT || m == (long)ledPwmGetMode())
        return;

    ledPwmSetMode((uint8_t)m);
    // OneJSON set_reply: 回显请求 id + code 200 (官方文档 detail/902)
    char rid[16] = "set";
    const char *q = strstr(buf, "\"id\":\"");
    if (q)
    {
        q += 6;
        size_t ri = 0;
        while (q[ri] && q[ri] != '"' && ri < sizeof(rid) - 1)
        {
            rid[ri] = q[ri];
            ri++;
        }
        rid[ri] = 0;
    }
    char rep[96];
    snprintf(rep, sizeof(rep), "{\"id\":\"%s\",\"code\":200,\"msg\":\"success\"}", rid);
    mqtt.publish(setRepTopic, rep);
    Serial.print("Cloud  : LED mode <- ");
    Serial.print(m);
    Serial.println(" (remote)");
}

// ---------------- 上行 ----------------
#if CLOUD_PROBE
static const char *const PROBE_IDS[] = {"lux", "pst", "svm", "freq", "ripple", "verdict", "led"};
static uint8_t probeIdx = 0;
#endif

static bool publishProperty(void)
{
    if (!lastSnap)
        return false;
    char buf[320];
#if CLOUD_PROBE == 2
    // 老版多协议接入 dp 数据点探测: 若此 publish 不被踢, 产品即老版 MQTT 产品
    snprintf(buf, sizeof(buf), "{\"id\":1,\"dp\":{\"lux\":[{\"v\":%.1f}]}}", lastSnap->vavg);
    bool ok = mqtt.publish(dpTopic, buf);
    Serial.print("Cloud  : dp publish(lux) ");
    Serial.println(ok ? "ok" : "FAILED");
    return ok;
#elif CLOUD_PROBE == 1
    // 逐属性诊断: 每次只发一个属性, 若平台在某个属性后断开即为元凶
    const char *id = PROBE_IDS[probeIdx];
    char val[32];
    switch (probeIdx)
    {
    case 0: snprintf(val, sizeof(val), "%.1f", lastSnap->vavg); break;
    case 1: snprintf(val, sizeof(val), "%.3f", lastSnap->pstLM); break;
    case 2: snprintf(val, sizeof(val), "%.3f", lastSnap->svm); break;
    case 3: snprintf(val, sizeof(val), "%.2f", lastSnap->freqHz); break;
    case 4: snprintf(val, sizeof(val), "%.2f", lastSnap->flickerPercent); break;
    case 5: snprintf(val, sizeof(val), "%u", (unsigned)lastSnap->zone); break;
    default: snprintf(val, sizeof(val), "%u", (unsigned)ledPwmGetMode()); break;
    }
    snprintf(buf, sizeof(buf),
             "{\"id\":\"%lu\",\"version\":\"1.0\",\"params\":{\"%s\":%s}}",
             (unsigned long)lastSnap->seq, id, val);
    Serial.print("Cloud  : probe publish ");
    Serial.print(id);
    Serial.print(" = ");
    Serial.println(val);
    probeIdx = (probeIdx + 1) % 7;
    bool ok = mqtt.publish(upTopic, buf);
    Serial.print("Cloud  : probe publish ");
    Serial.println(ok ? "ok" : "FAILED");
    return ok;
#else
    // OneJSON 属性上报: 属性值须包在 {"value":..} 对象中 (官方文档 detail/902)
    // lux 物模型为 int32 上报取整; pst/svm 步长 0.01 上报 2 位小数 (code 2254 not conform step)
    snprintf(buf, sizeof(buf),
             "{\"id\":\"%lu\",\"version\":\"1.0\",\"params\":{"
             "\"lux\":{\"value\":%d},\"pst\":{\"value\":%.2f},\"svm\":{\"value\":%.2f},"
             "\"freq\":{\"value\":%.1f},\"ripple\":{\"value\":%.2f},"
             "\"verdict\":{\"value\":%u},\"led\":{\"value\":%u}}}",
             (unsigned long)lastSnap->seq,
             (int)lastSnap->vavg, lastSnap->pstLM, lastSnap->svm, lastSnap->freqHz,
             lastSnap->flickerPercent, (unsigned)lastSnap->zone,
             (unsigned)ledPwmGetMode());
    bool ok = mqtt.publish(upTopic, buf);
    Serial.print("Cloud  : up ");
    Serial.println(buf);                 // 调试: 观察实际上报数值与平台校验结果的关系
    Serial.print("Cloud  : property publish ");
    Serial.println(ok ? "ok" : "FAILED");
    return ok;
#endif
}

// 发送待同步队列头一条; 成功置同步位
// 走 OneJSON 事件上报 (官方文档 detail/902): 事件标识符 record, 输出参数 data:string
// 记录 JSON 整体转义后嵌入 data — string 类型无 step 校验, 保留 3 位精度
static bool publishPendOne(void)
{
    ClassroomRecord *r = &pendRec[pendHead];
    uint16_t idx = pendIdx[pendHead];
    char lb[80], rec[400], esc[560], buf[720];
    escJson(lb, sizeof(lb), r->label);
    snprintf(rec, sizeof(rec),
             "{\"i\":%u,\"ts\":%lu,\"lb\":\"%s\",\"w\":%u,"
             "\"m\":%.2f,\"mx\":%.2f,\"fi\":%.3f,\"pst\":%.3f,\"px\":%.3f,"
             "\"svm\":%.3f,\"svmx\":%.3f,\"vavg\":%u,\"f\":%.2f,\"v\":%u}",
             (unsigned)idx, (unsigned long)r->ts, lb, (unsigned)r->windows,
             r->mMean, r->mMax, r->fi, r->pstMean, r->pstMax,
             r->svmMean, r->svmMax, (unsigned)r->vavgMean, r->freq,
             (unsigned)r->verdict);
    escJson(esc, sizeof(esc), rec);
    snprintf(buf, sizeof(buf),
             "{\"id\":\"%lu\",\"version\":\"1.0\",\"params\":{"
             "\"record\":{\"value\":{\"data\":\"%s\"}}}}",
             (unsigned long)r->ts, esc);
    if (!mqtt.publish(recTopic, buf))
    {
        if (mqtt.connected())
            Serial.println("Cloud  : record publish FAILED");
        return false;
    }
    pendHead = (pendHead + 1) % CLOUD_PEND_CAP;
    pendCnt--;
    markSynced(idx);
    Serial.print("Cloud  : record #");
    Serial.print(idx);
    Serial.println(" synced (event)");
    return true;
}

// 从历史记录中找出下一条未同步的入队 (逐条补, 队列空隙间调用)
static bool enqueueNextUnsynced(void)
{
    uint16_t nR = 0;
    const ClassroomRecord *rs = classroomRecords(&nR);
    for (uint16_t i = 0; i < nR; i++)
    {
        if (!(syncMap >> i & 1ULL))
        {
            pushPend(&rs[i], i);
            return true;
        }
    }
    return false;
}

// ---------------- 接口实现 ----------------
void cloudBegin(void)
{
    prefs.begin("cloud", true);
    prefs.getString("ssid", cfgSsid, sizeof(cfgSsid));
    prefs.getString("pass", cfgPass, sizeof(cfgPass));
    prefs.getString("pid", cfgPid, sizeof(cfgPid));
    prefs.getString("dev", cfgDev, sizeof(cfgDev));
    prefs.getString("tok", cfgTok, sizeof(cfgTok));
    syncMap = prefs.getULong64("syncmap", 0ULL);
    prefs.end();

    mqtt.setServer(CLOUD_MQTT_HOST, CLOUD_MQTT_PORT);
    mqtt.setSocketTimeout(4);
    mqtt.setKeepAlive(120);              // OneNET 要求 keepalive 30~1200s (PubSubClient 默认 15s 超下限)
    mqtt.setBufferSize(1024);            // 防平台下行大包 (期望值同步) 超缓冲导致自断
    mqtt.setCallback(onMqttMsg);

    snprintf(upTopic, sizeof(upTopic), "$sys/%s/%s/thing/property/post", cfgPid, cfgDev);
    snprintf(downTopic, sizeof(downTopic), "$sys/%s/%s/thing/property/set", cfgPid, cfgDev);
    snprintf(recTopic, sizeof(recTopic), "$sys/%s/%s/thing/event/post", cfgPid, cfgDev);
    snprintf(repTopic, sizeof(repTopic), "$sys/%s/%s/thing/property/post/reply", cfgPid, cfgDev);
    snprintf(evRepTopic, sizeof(evRepTopic), "$sys/%s/%s/thing/event/post/reply", cfgPid, cfgDev);
    snprintf(setRepTopic, sizeof(setRepTopic), "$sys/%s/%s/thing/property/set_reply", cfgPid, cfgDev);
    snprintf(dpTopic, sizeof(dpTopic), "$sys/%s/%s/dp/post/json", cfgPid, cfgDev);

    Serial.print("Cloud  : ");
    if (cfgSsid[0] == 0 || cfgPid[0] == 0 || cfgDev[0] == 0 || cfgTok[0] == 0)
    {
        Serial.println("not configured (open http://192.168.4.1/setup)");
        st = CL_OFF;
        return;                          // 未配置: 维持纯 AP (webdashBegin 置 WIFI_AP)
    }
    cfgOk = true;
    Serial.print("OneNET dev=");
    Serial.print(cfgDev);
    Serial.print(", WiFi \"");
    Serial.print(cfgSsid);
    Serial.println("\"");
    WiFi.mode(WIFI_AP_STA);              // AP+STA 共存: 本地仪表照常可用
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
    WiFi.begin(cfgSsid, cfgPass);
    st = CL_WAIT_WIFI;
}

void cloudTick(void)
{
    if (!cfgOk)
        return;
    uint32_t now = millis();

    if (st == CL_WAIT_WIFI)
    {
        if (WiFi.status() == WL_CONNECTED)
        {
            Serial.print("Cloud  : WiFi connected, IP ");
            Serial.println(WiFi.localIP());
            st = CL_CONNECTING;
            lastMqttTry = 0;             // 立即尝试 MQTT
        }
        return;
    }

    if (WiFi.status() != WL_CONNECTED)   // 已连接状态下掉线 → 回退重等
    {
        if (mqtt.connected())
            mqtt.disconnect();
        Serial.println("Cloud  : WiFi lost");
        st = CL_WAIT_WIFI;
        backfillDone = false;
        return;
    }

    if (!mqtt.connected())
    {
        if (st == CL_ONLINE)             // 会话曾被踢: 打一条定位平台断连
        {
            Serial.print("Cloud  : MQTT lost (server closed TCP), PubSubClient state=");
            Serial.println(mqtt.state());
        }
        st = CL_CONNECTING;
        if (now - lastMqttTry >= (uint32_t)CLOUD_RETRY_SEC * 1000)
        {
            lastMqttTry = now;
            bool ok = mqtt.connect(cfgDev, cfgPid, cfgTok);
            Serial.print("Cloud  : MQTT ");
            Serial.println(ok ? "connected" : "connect failed, retry...");
        }
        return;
    }

    if (st != CL_ONLINE)                 // 首次上线: 订阅下行 + 立即无条件上报一帧
    {
        st = CL_ONLINE;
        Serial.print("Cloud  : subscribe down=");
        Serial.print(mqtt.subscribe(downTopic) ? "ok" : "FAILED");
        Serial.print(" reply=");
        Serial.print(mqtt.subscribe(repTopic) ? "ok" : "FAILED");
        Serial.print(" evreply=");
        Serial.println(mqtt.subscribe(evRepTopic) ? "ok" : "FAILED");
        backfillDone = false;
        if (CLOUD_PUB_ENABLE)
            publishProperty();               // 观察平台是否拒收/踢连接 (无论节流)
        lastPub = now;
        Serial.println("Cloud  : ONLINE");
    }

    mqtt.loop();                         // 非阻塞泵: 收下行/保活

    if (CLOUD_PUB_ENABLE && lastSnap && now - lastPub >= (uint32_t)CLOUD_SNAPSHOT_SEC * 1000)
    {
        if (publishProperty())
            lastPub = now;
    }

    if (CLOUD_REC_ENABLE && now - lastRecPub >= 1000)    // 记录补传: 每秒最多一条 (A/B 排查开关)
    {
        if (pendCnt > 0)
        {
            if (publishPendOne())
                lastRecPub = now;
        }
        else if (!backfillDone)
        {
            if (enqueueNextUnsynced())
                lastRecPub = now;
            else
                backfillDone = true;     // 全部同步过
        }
    }
}

void cloudOnSnapshot(const FlickerSnapshot *s)
{
    lastSnap = s;
}

void cloudPushRecord(const ClassroomRecord *r, uint16_t idx)
{
    if (!cfgOk)
        return;
    pushPend(r, idx);
    backfillDone = false;
}

void cloudResetSync(void)
{
    syncMap = 0;
    pendHead = pendTail = pendCnt = 0;
    backfillDone = true;
    prefs.begin("cloud", false);
    prefs.putULong64("syncmap", 0ULL);
    prefs.end();
}

CloudState cloudGetState(void)
{
    return cfgOk ? st : CL_OFF;
}

const char *cloudStateName(void)
{
    switch (cloudGetState())
    {
    case CL_WAIT_WIFI:  return "WAIT-WIFI";
    case CL_CONNECTING: return "MQTT";
    case CL_ONLINE:     return "ONLINE";
    default:            return "OFF";
    }
}

bool cloudCfgGet(char *ssid, size_t ss, char *pass, size_t ps,
                 char *pid, size_t pp, char *dev, size_t ds, char *tok, size_t ts)
{
    snprintf(ssid, ss, "%s", cfgSsid);
    snprintf(pass, ps, "%s", cfgPass);
    snprintf(pid, pp, "%s", cfgPid);
    snprintf(dev, ds, "%s", cfgDev);
    snprintf(tok, ts, "%s", cfgTok);
    return cfgOk;
}

bool cloudCfgSave(const String &ssid, const String &pass, const String &pid,
                  const String &dev, const String &tok)
{
    prefs.begin("cloud", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.putString("pid", pid);
    prefs.putString("dev", dev);
    prefs.putString("tok", tok);
    prefs.end();
    return true;                         // 重启后生效
}
