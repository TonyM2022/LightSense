// ---------------- WiFi 网页仪表层实现 ----------------
// AP 热点 (config.h 配置 SSID/密码) + 异步 Web 服务器:
//   GET /     → 内嵌网页 (PROGMEM, 三页面: 频闪仪仪表 / LED 控制 / 教室测量)
//   WS  /ws   → 每报告窗口广播一次 JSON 快照 (指标 + 波形包络 + 频谱 + 教室进度)
//   GET /led  → 查询/设置 LED 输出模式 (?mode=const|50hz|100hz|500hz|1khz|5khz)
//   GET  /api/classroom        → 教室测量状态 + 记录列表
//   POST /api/classroom/start  → 开始一次 30s 测量 (?label=灯具标签)
//   POST /api/classroom/clear  → 清空记录
#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include "config.h"
#include "led_pwm.h"
#include "flicker.h"
#include "classroom.h"
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
.nav{display:flex;gap:8px;margin-bottom:12px}
.nav button{flex:1;background:var(--card);border:1px solid var(--edge);color:var(--dim);font-size:14px;padding:9px 0;border-radius:8px;cursor:pointer;font-weight:600}
.nav button.act{border-color:#1f6feb;color:#58a6ff;background:rgba(31,111,235,.12)}
.btns{display:grid;grid-template-columns:repeat(auto-fit,minmax(96px,1fr));gap:8px}
.btns button{background:var(--bg);border:1px solid var(--edge);color:var(--txt);font-size:15px;padding:12px 0;border-radius:8px;cursor:pointer}
.btns button.act{border-color:var(--grn);color:var(--grn);box-shadow:0 0 8px rgba(63,185,80,.25)}
.btns button:active{transform:scale(.97)}
.cls-row{display:flex;gap:8px;margin-bottom:10px}
.cls-row input{flex:1;background:var(--bg);border:1px solid var(--edge);color:var(--txt);border-radius:8px;padding:9px 10px;font-size:14px;min-width:0}
.cls-row button{background:#238636;border:1px solid #2ea043;color:#fff;font-size:14px;font-weight:600;border-radius:8px;padding:9px 20px;cursor:pointer;white-space:nowrap}
.cls-row button:disabled{opacity:.45;cursor:not-allowed}
.pwrap{height:8px;background:#21262d;border-radius:4px;overflow:hidden;margin-bottom:8px}
.pbar{height:100%;width:0;background:#1f6feb;transition:width .3s}
.tbl{width:100%;border-collapse:collapse;font-size:13px;font-variant-numeric:tabular-nums}
.tbl th,.tbl td{border-bottom:1px solid var(--edge);padding:6px 4px;text-align:left}
.tbl th{color:var(--dim);font-weight:500;font-size:12px}
.vchip{font-weight:600}
.btn2{display:flex;gap:8px;margin-top:10px}
.btn2 button{background:var(--bg);border:1px solid var(--edge);color:var(--txt);font-size:13px;padding:7px 14px;border-radius:8px;cursor:pointer}
</style>
</head>
<body>
<h1>LightSense 频闪仪<span id="conn" class="dot"></span></h1>
<nav class="nav">
  <button id="tab-meter" class="act" onclick="showPage('meter')">频闪仪</button>
  <button id="tab-led" onclick="showPage('led')">LED 控制</button>
  <button id="tab-cls" onclick="showPage('cls')">教室测量</button>
</nav>
<section id="pg-meter">
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
</section>
<section id="pg-led" style="display:none">
  <div class="card wide">
    <div class="label">LED 输出模式 (D8 = GPIO21, 闪烁模式占空比 50%, 常亮 100%)</div>
    <div class="btns" id="ledbtns"></div>
    <div class="meta" id="ledmeta" style="margin-top:8px">--</div>
  </div>
  <div class="card wide">
    <div class="label">自测方法</div>
    <div class="meta">把光传感器 (GPIO1) 对准外接 LED, 切换模式后回到「频闪仪」页: 主频应跟随输出频率 (50/100/500/1000/5000 Hz), 暗背景下波动深度接近 100% (方波 50% 占空比)。1 kHz 以上肉眼看近似常亮, 可用手机相机观察滚动条纹。</div>
  </div>
</section>
<section id="pg-cls" style="display:none">
  <div class="card wide">
    <div class="label">灯具标签 (留空自动编号)</div>
    <div class="cls-row">
      <input id="clslbl" maxlength="20" placeholder="例如: 第3排靠窗 / 讲台正中">
      <button id="clsbtn" onclick="clsStart()">开始测量</button>
    </div>
    <div class="pwrap"><div class="pbar" id="clspbar"></div></div>
    <div class="meta" id="clsstat">待机 · 每次测量约 30 s (29 个窗口)</div>
  </div>
  <div class="card wide">
    <div class="label">最近结果 (GB 40070-2021 + Pst^LM ≤ 1)</div>
    <div class="value" id="clsv">--</div>
    <div class="cards" style="margin:10px 0 0">
      <div><div class="label">波动深度 均值</div><div class="value" style="font-size:18px"><span id="clsm">--</span><small>%</small></div></div>
      <div><div class="label">波动深度 峰值</div><div class="value" style="font-size:18px"><span id="clsmx">--</span><small>%</small></div></div>
      <div><div class="label">主频</div><div class="value" style="font-size:18px"><span id="clsf">--</span><small>Hz</small></div></div>
      <div><div class="label">Flicker Index</div><div class="value" style="font-size:18px" id="clsfi">--</div></div>
      <div><div class="label">Pst^LM 均值</div><div class="value" style="font-size:18px" id="clsp">--</div></div>
      <div><div class="label">Pst^LM 峰值</div><div class="value" style="font-size:18px" id="clspx">--</div></div>
    </div>
    <div class="meta" id="clslbl2" style="margin-top:8px">尚未测量</div>
  </div>
  <div class="card wide">
    <div class="label">测量记录 (<span id="clscnt">0</span> 条, 断电保存)</div>
    <table class="tbl" id="clstbl"></table>
    <div class="btn2">
      <button onclick="clsCsv()">导出 CSV</button>
      <button onclick="clsClear()">清空记录</button>
    </div>
  </div>
  <div class="card wide">
    <div class="label">说明与局限</div>
    <div class="meta">判定口径: 波动深度取 30s 均值 vs 国标表4 限值 (按主频), Pst^LM 取 30s 均值 ≤ 1。局限: ① 单测点只反映测点位置光照, 教室验收应多点测量 (中心+四角) 并用标签区分; ② Pst^LM 为 30s 估计, 标准要求 10min; ③ 4.88Hz 频率分辨率, &lt;15Hz 主频精度有限; ④ 波动深度 &lt;1% 视为噪声直接判合规。</div>
  </div>
</section>
<script>
const $=id=>document.getElementById(id);
let ws=null,tmr=null,lastFreq=0;

// ---------------- 页面切换 ----------------
function showPage(p){
  for(const id of ['meter','led','cls']){
    $('pg-'+id).style.display=(id===p)?'':'none';
    $('tab-'+id).classList.toggle('act',id===p);
  }
}

// ---------------- LED 模式控制 ----------------
const MODES=[{p:'const',n:'常亮',f:0},{p:'50hz',n:'50 Hz',f:50},{p:'100hz',n:'100 Hz',f:100},{p:'500hz',n:'500 Hz',f:500},{p:'1khz',n:'1 kHz',f:1000},{p:'5khz',n:'5 kHz',f:5000}];
function buildLed(){
  const box=$('ledbtns');box.innerHTML='';
  for(const m of MODES){
    const b=document.createElement('button');
    b.textContent=m.n;b.dataset.p=m.p;
    b.onclick=()=>applyMode(m.p);
    box.appendChild(b);
  }
}
function paintLed(param){
  for(const b of $('ledbtns').children)b.classList.toggle('act',b.dataset.p===param);
  const m=MODES.find(x=>x.p===param);
  $('ledmeta').textContent=m?('当前模式: '+m.n+(m.f?(' @ '+m.f+' Hz, 占空比 50%'):' (GPIO 恒亮 100%)')):'--';
}
async function applyMode(p){
  $('ledmeta').textContent='设置中...';
  try{const r=await fetch('/led?mode='+p);if(!r.ok)throw new Error(r.status);paintLed(p);}
  catch(e){$('ledmeta').textContent='设置失败: '+e;refreshMode();}
}
async function refreshMode(){
  try{
    const r=await fetch('/led');
    const j=await r.json();
    paintLed(j.param);
  }catch(e){$('ledmeta').textContent='获取当前模式失败 (设备未连接?)';}
}
buildLed();refreshMode();

// ---------------- 教室测量 ----------------
const VNAME={0:'合规 PASS',1:'超标 · 波动深度',2:'超标 · Pst^LM',3:'超标 · 双项',4:'高频豁免'};
const VCLS={0:'z-pass',1:'z-fail',2:'z-fail',3:'z-fail',4:'z-exempt'};
let clsList=[];
function esc(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));}
function paintClsRes(r){
  const v=$('clsv');
  v.textContent=VNAME[r.v]||('--');
  v.className='value '+(VCLS[r.v]||'');
  $('clsm').textContent=r.m.toFixed(2);
  $('clsmx').textContent=r.mmax.toFixed(2);
  $('clsf').textContent=r.freq.toFixed(1);
  $('clsfi').textContent=r.fi.toFixed(3);
  $('clsp').textContent=r.pst.toFixed(3);
  $('clspx').textContent=r.pstmax.toFixed(3);
  $('clslbl2').textContent='标签: '+r.lb+' · 测量于开机 T+'+r.ts+' s · 主频取频谱能量最大窗口';
}
function paintClsTable(){
  let h='<tr><th>#</th><th>标签</th><th>主频</th><th>m 均值</th><th>m 峰值</th><th>Pst 均值</th><th>判定</th></tr>';
  clsList.forEach((r,i)=>{
    h+='<tr><td>'+(i+1)+'</td><td>'+esc(r.lb)+'</td><td>'+r.f.toFixed(1)+' Hz</td><td>'+r.m.toFixed(2)+'%</td><td>'+r.mx.toFixed(2)+'%</td><td>'+r.p.toFixed(3)+'</td><td class="vchip '+(VCLS[r.v]||'')+'">'+(VNAME[r.v]||'--')+'</td></tr>';
  });
  $('clstbl').innerHTML=h;
  $('clscnt').textContent=clsList.length;
}
async function clsRefresh(){
  try{
    const r=await fetch('/api/classroom');
    const j=await r.json();
    clsList=j.records||[];
    paintClsTable();
    if(j.latest)paintClsRes(j.latest);
    $('clsbtn').disabled=!!j.run;
    if(!j.run)$('clspbar').style.width=(j.nwin>=29?'100%':'0%');
  }catch(e){}
}
async function clsStart(){
  const lbl=$('clslbl').value.trim();
  try{
    const r=await fetch('/api/classroom/start?label='+encodeURIComponent(lbl),{method:'POST'});
    if(!r.ok){const j=await r.json().catch(()=>({}));throw new Error(j.err||r.status);}
    $('clsbtn').disabled=true;
    $('clsstat').textContent='测量中 0 / 29 窗口 (~30 s)...';
  }catch(e){alert('开始失败: '+e);}
}
async function clsClear(){
  if(!confirm('确定清空全部测量记录?'))return;
  try{await fetch('/api/classroom/clear',{method:'POST'});await clsRefresh();
      $('clsv').textContent='--';$('clsv').className='value';
      $('clslbl2').textContent='尚未测量';}catch(e){alert('清空失败: '+e);}
}
function clsCsv(){
  const rows=[['#','标签','T+秒','主频Hz','m均值%','m峰值%','FlickerIndex','Pst均值','Pst峰值','判定']];
  clsList.forEach((r,i)=>rows.push([i+1,r.lb,r.ts,r.f.toFixed(2),r.m.toFixed(2),r.mx.toFixed(2),r.fi.toFixed(3),r.p.toFixed(3),r.px.toFixed(3),VNAME[r.v]||'']));
  const csv='\ufeff'+rows.map(r=>r.map(c=>'"'+String(c).replace(/"/g,'""')+'"').join(',')).join('\r\n');
  const a=document.createElement('a');
  a.href=URL.createObjectURL(new Blob([csv],{type:'text/csv;charset=utf-8'}));
  a.download='classroom.csv';
  a.click();
  URL.revokeObjectURL(a.href);
}
function clsUpdate(c){
  const pct=c.res?100:Math.round(100*c.win/c.tot);
  $('clspbar').style.width=pct+'%';
  if(c.run){
    $('clsbtn').disabled=true;
    $('clsstat').textContent='测量中 '+c.win+' / '+c.tot+' 窗口 (~30 s) · 实时 m '+c.m.toFixed(2)+'% · '+c.freq.toFixed(1)+' Hz';
  }else{
    $('clsbtn').disabled=false;
    if(c.res){
      paintClsRes(c.res);
      clsRefresh();
      $('clsstat').textContent='测量完成: '+(VNAME[c.res.v]||'');
    }else if($('clsstat').textContent.startsWith('测量中')){
      $('clsstat').textContent='待机 · 每次测量约 30 s (29 个窗口)';
    }
  }
}
clsRefresh();

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
  if(d.cls)clsUpdate(d.cls);
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
// JSON 字符串转义 (用户标签可能含 " 或 \)
static void jsonEscape(char *dst, size_t cap, const char *src)
{
    size_t j = 0;
    for (size_t i = 0; src[i] != 0 && j + 6 < cap; i++)
    {
        char c = src[i];
        if (c == '"' || c == '\\')
            dst[j++] = '\\';
        dst[j++] = c;
    }
    dst[j] = 0;
}

// UTF-8 安全截断 (最长 maxBytes 字节, 不切断多字节字符)
static void utf8Trunc(String &s, size_t maxBytes)
{
    if (s.length() <= maxBytes)
        return;
    size_t cut = maxBytes;
    while (cut > 0 && ((uint8_t)s[cut] & 0xC0) == 0x80)
        cut--;
    s = s.substring(0, cut);
}

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

    // LED 模式查询/设置: GET /led        → 当前模式 JSON
    //                    GET /led?mode=X → 切换模式 (const|50hz|100hz|500hz|1khz|5khz)
    server.on("/led", HTTP_GET, [](AsyncWebServerRequest *req)
    {
        if (req->hasParam("mode"))
        {
            String v = req->getParam("mode")->value();
            bool ok = false;
            for (uint8_t i = 0; i < LED_MODE_COUNT; i++)
            {
                const char *p = ledPwmModeParam(i);
                if (p && v.equalsIgnoreCase(p))
                {
                    ledPwmSetMode(i);
                    ok = true;
                    break;
                }
            }
            if (!ok)
            {
                req->send(400, "application/json", "{\"err\":\"bad mode\"}");
                return;
            }
        }
        uint8_t m = ledPwmGetMode();
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"mode\":%u,\"name\":\"%s\",\"param\":\"%s\"}",
                 m, ledPwmModeName(m), ledPwmModeParam(m));
        req->send(200, "application/json", buf);
    });

    // 教室测量: 状态 + 记录列表
    server.on("/api/classroom", HTTP_GET, [](AsyncWebServerRequest *req)
    {
        static char cb[9 * 1024];       // 仅 async_tcp 任务调用, 单线程安全
        uint16_t nR = 0;
        const ClassroomRecord *rs = classroomRecords(&nR);
        int n = snprintf(cb, sizeof(cb),
                         "{\"run\":%u,\"nwin\":%lu,\"tot\":%u,\"n\":%u,\"records\":[",
                         (unsigned)classroomIsRunning(),
                         (unsigned long)classroomWindowsDone(),
                         (unsigned)CLS_WINDOWS, (unsigned)nR);
        for (uint16_t i = 0; i < nR && n < (int)sizeof(cb) - 192; i++)
        {
            char lb[80];
            jsonEscape(lb, sizeof(lb), rs[i].label);
            n += snprintf(cb + n, sizeof(cb) - n,
                          "%s{\"i\":%u,\"lb\":\"%s\",\"ts\":%lu,\"m\":%.2f,\"mx\":%.2f,"
                          "\"fi\":%.3f,\"p\":%.3f,\"px\":%.3f,\"f\":%.2f,\"v\":%u}",
                          (i ? "," : ""), (unsigned)i, lb,
                          (unsigned long)rs[i].ts, rs[i].mMean, rs[i].mMax,
                          rs[i].fi, rs[i].pstMean, rs[i].pstMax, rs[i].freq,
                          (unsigned)rs[i].verdict);
        }
        n += snprintf(cb + n, sizeof(cb) - n, "],\"latest\":");
        if (nR > 0)
        {
            const ClassroomRecord *L = &rs[nR - 1];
            char lb[80];
            jsonEscape(lb, sizeof(lb), L->label);
            n += snprintf(cb + n, sizeof(cb) - n,
                          "{\"lb\":\"%s\",\"m\":%.2f,\"mmax\":%.2f,\"fi\":%.3f,"
                          "\"pst\":%.3f,\"pstmax\":%.3f,\"freq\":%.2f,\"v\":%u,\"ts\":%lu}",
                          lb, L->mMean, L->mMax, L->fi, L->pstMean, L->pstMax,
                          L->freq, (unsigned)L->verdict, (unsigned long)L->ts);
        }
        else
        {
            n += snprintf(cb + n, sizeof(cb) - n, "null");
        }
        snprintf(cb + n, sizeof(cb) - n, "}");
        req->send(200, "application/json", cb);
    });

    // 教室测量: 开始一次 30s 测量 (?label=灯具标签, 可省略)
    server.on("/api/classroom/start", HTTP_POST, [](AsyncWebServerRequest *req)
    {
        String label = "";
        if (req->hasParam("label"))
            label = req->getParam("label")->value();
        utf8Trunc(label, 31);
        if (!classroomStart(label.c_str()))
        {
            req->send(409, "application/json", "{\"err\":\"busy or records full\"}");
            return;
        }
        req->send(200, "application/json", "{\"ok\":1}");
    });

    // 教室测量: 清空全部记录
    server.on("/api/classroom/clear", HTTP_POST, [](AsyncWebServerRequest *req)
    {
        classroomClear();
        req->send(200, "application/json", "{\"ok\":1}");
    });

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
//        "cls":{"run","win","tot","m","freq"[,"res"]},"wmin":[...],"wmax":[...],"spec":[...]}
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

    // 教室测量状态 (+ 刚完成的测量结果)
    n += snprintf(jsonBuf + n, sizeof(jsonBuf) - n,
                  "],\"cls\":{\"run\":%u,\"win\":%lu,\"tot\":%u,\"m\":%.2f,\"freq\":%.2f",
                  (unsigned)classroomIsRunning(),
                  (unsigned long)classroomWindowsDone(),
                  (unsigned)CLS_WINDOWS, s->flickerPercent, s->freqHz);
    ClassroomRecord cr;
    if (classroomPopResult(&cr))
    {
        char lb[80];
        jsonEscape(lb, sizeof(lb), cr.label);
        n += snprintf(jsonBuf + n, sizeof(jsonBuf) - n,
                      ",\"res\":{\"lb\":\"%s\",\"m\":%.2f,\"mx\":%.2f,\"fi\":%.3f,"
                      "\"p\":%.3f,\"px\":%.3f,\"f\":%.2f,\"v\":%u,\"ts\":%lu}",
                      lb, cr.mMean, cr.mMax, cr.fi, cr.pstMean, cr.pstMax,
                      cr.freq, (unsigned)cr.verdict, (unsigned long)cr.ts);
    }

    n += snprintf(jsonBuf + n, sizeof(jsonBuf) - n, "}}");

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
