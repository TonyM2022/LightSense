// ---------------- WiFi 网页仪表层实现 ----------------
// AP 热点 (config.h 配置 SSID/密码) + 异步 Web 服务器:
//   GET /    → 内嵌网页 (PROGMEM, 手写 canvas 渲染, 无外部依赖, AP 下可离线打开)
//   WS  /ws  → 每报告窗口广播一次 JSON 快照 (指标 + 波形包络 + 频谱)
#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include "config.h"
#include "webdash.h"

static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");

// ---------------- 内嵌网页 ----------------
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>LightSense 频闪仪</title>
<style>
:root{--bg:#0d1117;--card:#161b22;--edge:#30363d;--txt:#e6edf3;--dim:#8b949e;--grn:#3fb950;--red:#f85149}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--txt);font-family:system-ui,-apple-system,"Segoe UI",Roboto,"PingFang SC","Microsoft YaHei",sans-serif;padding:14px;max-width:960px;margin:0 auto}
h1{font-size:19px;display:flex;align-items:center;gap:8px;margin-bottom:12px;font-weight:600}
.dot{width:10px;height:10px;border-radius:50%;background:#6e7681}
.dot.on{background:var(--grn);box-shadow:0 0 8px var(--grn)}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:8px;margin-bottom:8px}
.card{background:var(--card);border:1px solid var(--edge);border-radius:8px;padding:10px 12px}
.label{font-size:12px;color:var(--dim);margin-bottom:4px}
.value{font-size:22px;font-weight:600;font-variant-numeric:tabular-nums}
.value small{font-size:12px;color:var(--dim);font-weight:400;margin-left:2px}
#zone{font-size:19px}
.z-pass{color:var(--grn)} .z-fail{color:var(--red)} .z-exempt{color:var(--dim)}
.wide{margin-bottom:8px}
canvas{width:100%;height:180px;display:block}
.meta{font-size:12px;color:var(--dim);font-variant-numeric:tabular-nums}
</style>
</head>
<body>
<h1>LightSense 频闪仪<span id="conn" class="dot"></span></h1>
<div class="cards">
  <div class="card"><div class="label">波动深度 (Percent Flicker)</div><div class="value"><span id="flk">--</span><small>%</small></div></div>
  <div class="card"><div class="label">Flicker Index</div><div class="value"><span id="fi">--</span></div></div>
  <div class="card"><div class="label">Pst<sup>LM</sup></div><div class="value"><span id="pst">--</span></div></div>
  <div class="card"><div class="label">主频</div><div class="value"><span id="freq">--</span><small>Hz</small></div></div>
  <div class="card"><div class="label">GB 40070-2021 判定</div><div class="value" id="zone">--</div></div>
</div>
<div class="card wide"><div class="label">时域波形 (末块包络, 204.8 ms, ADC 码)</div><canvas id="wave"></canvas></div>
<div class="card wide"><div class="label">频谱 (窗口平均, 0~10 kHz)</div><canvas id="spec"></canvas></div>
<div class="card wide">
  <div class="label">ADC 码值 (窗口统计)</div>
  <div class="cards" style="margin:0">
    <div><div class="label">Max</div><div class="value" style="font-size:18px" id="vmax">--</div></div>
    <div><div class="label">Min</div><div class="value" style="font-size:18px" id="vmin">--</div></div>
    <div><div class="label">Average</div><div class="value" style="font-size:18px" id="vavg">--</div></div>
  </div>
</div>
<div class="meta" id="meta">未连接, 等待 WebSocket...</div>
<script>
const $=id=>document.getElementById(id);
let ws=null,tmr=null,lastFreq=0;

function retry(){clearTimeout(tmr);tmr=setTimeout(connect,2000);}
function connect(){
  clearTimeout(tmr);
  try{ws=new WebSocket('ws://'+location.host+'/ws');}catch(e){retry();return;}
  ws.onopen=()=>{$('conn').classList.add('on');$('meta').textContent='已连接, 等待数据...';};
  ws.onclose=()=>{$('conn').classList.remove('on');retry();};
  ws.onerror=()=>{try{ws.close();}catch(e){}};
  ws.onmessage=ev=>{
    try{render(JSON.parse(ev.data));}
    catch(e){console.error('bad json:',String(ev.data).slice(0,120),e);}
  };
}
connect();

function render(d){
  $('flk').textContent=d.flk.toFixed(2);
  $('fi').textContent=d.fi.toFixed(3);
  $('pst').textContent=d.pst.toFixed(3);
  $('freq').textContent=d.freq.toFixed(2);
  $('vmax').textContent=d.vmax;
  $('vmin').textContent=d.vmin;
  $('vavg').textContent=d.vavg.toFixed(1);
  lastFreq=d.freq;
  const z=$('zone');
  z.textContent=d.zone;
  z.className='value '+(d.zcode===1?'z-fail':d.zcode===2?'z-exempt':'z-pass');
  drawWave(d.wmin,d.wmax);
  drawSpec(d.spec);
  $('meta').textContent='窗口 #'+d.seq+' · '+d.samples+' 样本 · DMA 溢出 '+d.ovf+
    (d.ovf>0?' (数据丢失!)':'')+' · 更新于 '+new Date().toLocaleTimeString();
}

function setup(cv){
  const dpr=window.devicePixelRatio||1;
  const w=cv.clientWidth,h=cv.clientHeight;
  if(cv.width!==Math.round(w*dpr)||cv.height!==Math.round(h*dpr)){
    cv.width=Math.round(w*dpr);cv.height=Math.round(h*dpr);
  }
  const ctx=cv.getContext('2d');
  ctx.setTransform(dpr,0,0,dpr,0,0);
  ctx.clearRect(0,0,w,h);
  return {ctx,w,h};
}

function drawWave(wmn,wmx){
  const c=setup($('wave')),ctx=c.ctx,w=c.w,h=c.h;
  const L=46,R=10,T=10,B=20,gw=w-L-R,gh=h-T-B;
  const y=v=>T+gh*(1-v/4095);
  ctx.font='10px monospace';ctx.lineWidth=1;
  for(let g=0;g<=4;g++){
    const v=(g===4)?4095:g*1024,yy=Math.round(y(v))+.5;
    ctx.strokeStyle='#21262d';
    ctx.beginPath();ctx.moveTo(L,yy);ctx.lineTo(w-R,yy);ctx.stroke();
    ctx.fillStyle='#8b949e';ctx.fillText(v,4,yy+3);
  }
  if(!wmn||!wmn.length)return;
  const n=wmn.length,px=i=>L+gw*i/(n-1);
  ctx.beginPath();
  for(let i=0;i<n;i++)i?ctx.lineTo(px(i),y(wmx[i])):ctx.moveTo(px(i),y(wmx[i]));
  for(let i=n-1;i>=0;i--)ctx.lineTo(px(i),y(wmn[i]));
  ctx.closePath();
  ctx.fillStyle='rgba(88,166,255,0.20)';ctx.fill();
  ctx.strokeStyle='#58a6ff';ctx.lineWidth=1.5;
  for(const arr of [wmx,wmn]){
    ctx.beginPath();
    for(let i=0;i<n;i++)i?ctx.lineTo(px(i),y(arr[i])):ctx.moveTo(px(i),y(arr[i]));
    ctx.stroke();
  }
  ctx.fillStyle='#8b949e';
  ctx.fillText('0 ms',L,h-6);
  ctx.fillText('204.8 ms',w-R-52,h-6);
}

function drawSpec(sp){
  const c=setup($('spec')),ctx=c.ctx,w=c.w,h=c.h;
  const L=46,R=10,T=10,B=20,gw=w-L-R,gh=h-T-B;
  ctx.font='10px monospace';ctx.lineWidth=1;
  for(let k=0;k<=10;k++){
    const x=Math.round(L+gw*k/10)+.5;
    ctx.strokeStyle='#21262d';
    ctx.beginPath();ctx.moveTo(x,T);ctx.lineTo(x,T+gh);ctx.stroke();
    if(k%2===0){ctx.fillStyle='#8b949e';ctx.fillText(k+'k',x-8,h-6);}
  }
  if(!sp||!sp.length)return;
  let mx=0;for(const v of sp)if(v>mx)mx=v;
  if(mx<=0)mx=1;
  const n=sp.length;
  ctx.beginPath();ctx.moveTo(L,T+gh);
  for(let i=0;i<n;i++)ctx.lineTo(L+gw*i/(n-1),T+gh*(1-sp[i]/mx));
  ctx.lineTo(L+gw,T+gh);ctx.closePath();
  ctx.fillStyle='rgba(63,185,80,0.15)';ctx.fill();
  ctx.strokeStyle='#3fb950';ctx.lineWidth=1.2;ctx.stroke();
  if(lastFreq>0&&lastFreq<10000){
    const x=Math.round(L+gw*lastFreq/10000)+.5;
    ctx.strokeStyle='#f0883e';ctx.setLineDash([4,3]);
    ctx.beginPath();ctx.moveTo(x,T);ctx.lineTo(x,T+gh);ctx.stroke();
    ctx.setLineDash([]);
  }
}
</script>
</body>
</html>
)rawliteral";

// ---------------- WebSocket 事件 ----------------
static void onWsEvent(AsyncWebSocket *srv, AsyncWebSocketClient *client,
                      AwsEventType type, void *arg, uint8_t *data, size_t len)
{
    (void)arg; (void)data; (void)len;
    if (type == WS_EVT_CONNECT)
    {
        Serial.printf("WS client #%u connected (total %u)\n",
                      client->id(), ws.count());
    }
    else if (type == WS_EVT_DISCONNECT)
    {
        Serial.printf("WS client #%u disconnected (total %u)\n",
                      client->id(), ws.count());
    }
}

// ---------------- 启动 AP + 服务器 ----------------
bool webdashBegin(void)
{
    WiFi.mode(WIFI_AP);
    if (!WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS))
    {
        Serial.println("WiFi softAP failed.");
        return false;
    }

    ws.onEvent(onWsEvent);
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *req)
              { req->send_P(200, "text/html", INDEX_HTML); });
    server.addHandler(&ws);
    server.begin();

    Serial.print("WiFi AP : SSID ");
    Serial.print(WIFI_AP_SSID);
    Serial.print("  IP ");
    Serial.println(WiFi.softAPIP());
    Serial.println("Web dash: open http://192.168.4.1");
    return true;
}

// ---------------- JSON 快照构建 ----------------
// 格式: {"seq","samples","ovf","vmax","vmin","vavg","flk","fi","pst","freq","zcode","zone",
//        "wmin":[...],"wmax":[...],"spec":[...]}
static char jsonBuf[12 * 1024];

static size_t buildJson(const FlickerSnapshot *s)
{
    static const char *zoneNames[] = {"PASS", "FAIL", "EXEMPT"};
    const char *zone = s->noisePass ? "PASS (<1%)" : zoneNames[s->zone];

    int n = snprintf(jsonBuf, sizeof(jsonBuf),
                     "{\"seq\":%lu,\"samples\":%lu,\"ovf\":%lu,"
                     "\"vmax\":%u,\"vmin\":%u,\"vavg\":%.1f,"
                     "\"flk\":%.2f,\"fi\":%.3f,\"pst\":%.3f,\"freq\":%.2f,"
                     "\"zcode\":%u,\"zone\":\"%s\","
                     "\"wmin\":[",
                     (unsigned long)s->seq, (unsigned long)s->samples,
                     (unsigned long)s->overflow,
                     (unsigned)s->vmax, (unsigned)s->vmin, s->vavg,
                     s->flickerPercent, s->flickerIndex, s->pstLM, s->freqHz,
                     (unsigned)s->zone, zone);

    for (int i = 0; i < WEB_WAVE_POINTS && n < (int)sizeof(jsonBuf) - 16; i++)
        n += snprintf(jsonBuf + n, sizeof(jsonBuf) - n, "%.0f,", s->wmin[i]);
    if (n > 0 && jsonBuf[n - 1] == ',') n--;

    n += snprintf(jsonBuf + n, sizeof(jsonBuf) - n, "],\"wmax\":[");
    for (int i = 0; i < WEB_WAVE_POINTS && n < (int)sizeof(jsonBuf) - 16; i++)
        n += snprintf(jsonBuf + n, sizeof(jsonBuf) - n, "%.0f,", s->wmax[i]);
    if (n > 0 && jsonBuf[n - 1] == ',') n--;

    n += snprintf(jsonBuf + n, sizeof(jsonBuf) - n, "],\"spec\":[");
    for (int i = 0; i < WEB_SPEC_POINTS && n < (int)sizeof(jsonBuf) - 16; i++)
        n += snprintf(jsonBuf + n, sizeof(jsonBuf) - n, "%.0f,", s->spec[i]);
    if (n > 0 && jsonBuf[n - 1] == ',') n--;

    n += snprintf(jsonBuf + n, sizeof(jsonBuf) - n, "]}");

    return (n > 0 && n < (int)sizeof(jsonBuf)) ? (size_t)n : 0;
}

// ---------------- 广播 ----------------
void webdashBroadcast(const FlickerSnapshot *snap)
{
    ws.cleanupClients();
    if (ws.count() == 0)
        return;                       // 无客户端时不构建 JSON, 节省 CPU

    size_t len = buildJson(snap);
    if (len == 0)
        return;
    ws.textAll(jsonBuf, len);
}
