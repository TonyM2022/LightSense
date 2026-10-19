// ---------------- WiFi 网页仪表层实现 ----------------
// AP 热点 (config.h 配置 SSID/密码) + 异步 Web 服务器:
//   GET /     → 内嵌网页 (PROGMEM, 三页面: 频闪仪仪表 / LED 控制 / 教室测量)
//   WS  /ws   → 每报告窗口广播一次 JSON 快照 (指标 + 波形包络 + 频谱 + 教室进度)
//   GET /led  → 查询/设置 LED 输出模式 (?mode=const|50hz|100hz|500hz|1khz|5khz|off)
//   GET  /api/classroom        → 教室测量状态 + 记录列表
//   POST /api/classroom/start  → 开始测量 (?label=灯具标签&dur=30|60|600 秒&t=Unix秒)
//   POST /api/classroom/clear  → 清空记录 (同时清云端同步位图)
//   GET  /setup  POST /setup   → OneNET 云端配网页 (WiFi/产品/设备/Token 存 NVS)
//   GET  /api/cloud            → 云端配置回显 + 连接状态
//   GET  /api/wifiscan         → WiFi 异步扫描 (首次触发, 轮询取结果)
#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include "config.h"
#include "led_pwm.h"
#include "flicker.h"
#include "classroom.h"
#include "cloud.h"
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
.cls-row select{background:var(--bg);border:1px solid var(--edge);color:var(--txt);border-radius:8px;padding:9px 8px;font-size:14px;cursor:pointer}
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
/* ---- 大屏演示模式 (body.big): 投影/大屏展示时放大字号、曲线与表格 ---- */
.bigbtn{margin-left:auto;background:var(--card);border:1px solid var(--edge);color:var(--dim);font-size:12px;padding:6px 12px;border-radius:8px;cursor:pointer;font-weight:600}
.bigbtn.on{border-color:#f0883e;color:#f0883e;background:rgba(240,136,62,.12)}
body.big{padding:22px 26px;max-width:1800px}
body.big h1{font-size:30px;margin-bottom:18px}
body.big .card{padding:16px 20px}
body.big .cards{grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:14px}
body.big .label{font-size:17px;margin-bottom:8px}
body.big .value{font-size:38px!important}
body.big .value small{font-size:18px}
body.big .meta{font-size:16px}
body.big .wide{margin-bottom:14px}
body.big canvas{height:340px}
body.big .nav button{font-size:21px;padding:16px 0}
body.big .btns{grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:12px}
body.big .btns button{font-size:21px;padding:22px 0}
body.big .tbl{font-size:17px}
body.big .tbl th{font-size:16px}
body.big .tbl th,body.big .tbl td{padding:10px 6px}
body.big .cls-row input,body.big .cls-row select{font-size:19px;padding:13px 12px}
body.big .cls-row button{font-size:19px;padding:13px 28px}
body.big .btn2 button{font-size:17px;padding:11px 20px}
/* ---- 灯具趋势分析 (同一标签 = 同一灯具; 时间轴折线 + 稳健回归 + 越限预测) ---- */
#trend{height:240px}
body.big #trend{height:360px}
.trend-w{font-size:13px;padding:9px 11px;border-radius:8px;margin-top:8px;border:1px solid var(--edge);color:var(--dim)}
.w-red{border-color:#f85149;color:#f85149;background:rgba(248,81,73,.10)}
.w-org{border-color:#f0883e;color:#f0883e;background:rgba(240,136,62,.10)}
.w-grn{border-color:#3fb950;color:#3fb950;background:rgba(63,185,80,.10)}
body.big .trend-w{font-size:17px;padding:12px 14px}
</style>
</head>
<body>
<h1>LightSense 频闪仪<span id="conn" class="dot"></span><button id="bigbtn" class="bigbtn" onclick="toggleBig()">演示模式</button></h1>
<nav class="nav">
  <button id="tab-meter" class="act" onclick="showPage('meter')">频闪仪</button>
  <button id="tab-led" onclick="showPage('led')">LED 控制</button>
  <button id="tab-cls" onclick="showPage('cls')">教室测量</button>
</nav>
<section id="pg-meter">
<div class="cards">
  <div class="card"><div class="label">波动深度 (Percent Flicker)</div><div class="value"><span id="flk">--</span><small>%</small></div></div>
  <div class="card"><div class="label">Flicker Index</div><div class="value"><span id="fi">--</span></div></div>
  <div class="card"><div class="label">Pst<sup>LM</sup> (1s 窗值)</div><div class="value"><span id="pst">--</span></div></div>
  <div class="card"><div class="label">SVM (频闪可见度)</div><div class="value"><span id="svm">--</span></div><div class="meta" style="margin-top:4px">限值: ≤0.4 (EU 2024) / ≤1 可见阈</div></div>
  <div class="card"><div class="label">主频</div><div class="value"><span id="freq">--</span><small>Hz</small></div></div>
  <div class="card"><div class="label">综合判定 (光照 + Pst<sup>LM</sup> + SVM)</div><div class="value" id="zone">--</div><div class="meta" style="margin-top:4px">光照均值 400~3000 码 / Pst<sup>LM</sup> ≤ 1 / SVM ≤ 0.4</div></div>
</div>
<div class="card wide"><div class="label">时域波形 (末块包络, 204.8 ms, ADC 码)</div><canvas id="wave"></canvas></div>
<div class="card wide"><div class="label">频谱 (窗口平均, 对数轴 4.9 Hz ~ 5 kHz)</div><canvas id="spec"></canvas></div>
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
    <div class="label">LED 输出模式 (D7 = GPIO17, 闪烁模式占空比 50%, 常亮 100%, 常灭 0%)</div>
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
    <div class="label">测量时长与灯具标签 (留空自动编号)</div>
    <div class="cls-row">
      <select id="clsdur">
        <option value="30">30 s</option>
        <option value="60">1 min</option>
        <option value="600">10 min</option>
      </select>
      <input id="clslbl" maxlength="20" list="lbllist" placeholder="例如: 第3排靠窗 / 讲台正中">
      <datalist id="lbllist"></datalist>
      <button id="clsbtn" onclick="clsStart()">开始测量</button>
    </div>
    <div class="pwrap"><div class="pbar" id="clspbar"></div></div>
    <div class="meta" id="clsstat">待机 · 时长可选 30 s / 1 min / 10 min</div>
  </div>
  <div class="card wide">
    <div class="label">最近结果 (光照 + Pst^LM ≤ 1 + SVM ≤ 0.4)</div>
    <div class="value" id="clsv">--</div>
    <div class="cards" style="margin:10px 0 0">
      <div><div class="label">波动深度 均值</div><div class="value" style="font-size:18px"><span id="clsm">--</span><small>%</small></div></div>
      <div><div class="label">波动深度 峰值</div><div class="value" style="font-size:18px"><span id="clsmx">--</span><small>%</small></div></div>
      <div><div class="label">主频</div><div class="value" style="font-size:18px"><span id="clsf">--</span><small>Hz</small></div></div>
      <div><div class="label">Flicker Index</div><div class="value" style="font-size:18px" id="clsfi">--</div></div>
      <div><div class="label">Pst^LM 均值</div><div class="value" style="font-size:18px" id="clsp">--</div></div>
      <div><div class="label">Pst^LM 峰值</div><div class="value" style="font-size:18px" id="clspx">--</div></div>
      <div><div class="label">SVM 均值</div><div class="value" style="font-size:18px" id="clssvm">--</div></div>
      <div><div class="label">SVM 峰值</div><div class="value" style="font-size:18px" id="clssvmx">--</div></div>
      <div><div class="label">光照均值</div><div class="value" style="font-size:18px" id="clsvavg">--</div></div>
    </div>
    <div class="meta" id="clslbl2" style="margin-top:8px">尚未测量</div>
  </div>
  <div class="card wide">
    <div class="label">灯具趋势分析 (同一标签 = 同一灯具 · 随时间变化与恶化预警)</div>
    <div class="cls-row">
      <select id="trLbl" onchange="trPick()"></select>
      <select id="trMet" onchange="trPick()">
        <option value="p">Pst^LM 均值 (限值 1.0)</option>
        <option value="s">SVM 均值 (限值 0.4)</option>
        <option value="m">波动深度 m 均值</option>
        <option value="va">光照均值 ADC</option>
      </select>
    </div>
    <canvas id="trend"></canvas>
    <div class="meta" id="trStat" style="margin-top:8px">--</div>
    <div id="trWarn"></div>
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
    <div class="meta">判定口径 (v1.1): 光照前置检查 (窗口均值 ADC 码 &lt;400 光线过暗 / &gt;3000 光线太亮, 此时 Pst/SVM 无效) → Pst^LM 取本次测量均值 ≤ 1 (IEC TR 61547-1), SVM 取本次测量均值 ≤ 0.4 (IEC TR 63158, EU 2024 生态设计限值); 波动深度与主频仅展示不判定。局限: ① 单测点只反映测点位置光照, 教室验收应多点测量 (中心+四角) 并用标签区分; ② 30s/1min 档 Pst^LM 为短时估计, 标准口径 10min (可选 10min 档); ③ 4.88Hz 频率分辨率, &lt;15Hz 主频精度有限; ④ 旧版本记录为旧口径判定或无 SVM/光照, 建议清空重测; ⑤ 同一灯具定期复查请填写完全相同的标签 (输入框提供历史标签下拉), 才会被趋势分析归为同一灯具; 趋势分析只用带日期的记录 —— 网页发起的测量自动带浏览器日期, 联网 (已配 OneNET WiFi) 时按键发起的测量也带日期。</div>
  </div>
</section>
<script>
const $=id=>document.getElementById(id);
let ws=null,tmr=null,lastFreq=0,lastD=null;

// ---------------- 页面切换 ----------------
function showPage(p){
  for(const id of ['meter','led','cls']){
    $('pg-'+id).style.display=(id===p)?'':'none';
    $('tab-'+id).classList.toggle('act',id===p);
  }
  if(p==='cls')drawTrend();                 // 趋势图在隐藏状态下无法量取尺寸, 切到本页时重绘
}

// ---------------- LED 模式控制 ----------------
const MODES=[{p:'off',n:'常灭',f:0},{p:'const',n:'常亮',f:0},{p:'50hz',n:'50 Hz',f:50},{p:'100hz',n:'100 Hz',f:100},{p:'500hz',n:'500 Hz',f:500},{p:'1khz',n:'1 kHz',f:1000},{p:'5khz',n:'5 kHz',f:5000}];
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
  $('ledmeta').textContent=m?('当前模式: '+m.n+(m.p==='off'?' (GPIO 恒灭 0%)':(m.f?(' @ '+m.f+' Hz, 占空比 50%'):' (GPIO 恒亮 100%)'))):'--';
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
const VNAME={0:'合规 PASS',1:'超标 · 波动深度',2:'超标 · Pst^LM',3:'超标 · 双项',4:'高频豁免',5:'超标 · SVM',6:'光线过暗',7:'光线太亮'};
const VCLS={0:'z-pass',1:'z-fail',2:'z-fail',3:'z-fail',4:'z-exempt',5:'z-fail',6:'z-fail',7:'z-fail'};
const durName=w=>w>=586?'10min':(w>=58?'1min':'30s');
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
  $('clssvm').textContent=(r.s!=null)?r.s.toFixed(3):'--';
  $('clssvmx').textContent=(r.sx!=null)?r.sx.toFixed(3):'--';
  $('clsvavg').textContent=(r.va!=null)?r.va:'--';
  const dt=r.tea?new Date(r.tea*1000):null;
  $('clslbl2').textContent='标签: '+r.lb+' · 测量于 '+(dt?(ymd(dt)+' '+dt.toTimeString().slice(0,5)):'开机 T+'+r.ts+' s')+
                            ' · 主频取频谱能量最大窗口';
}
function paintClsTable(){
  let h='<tr><th>#</th><th>日期</th><th>标签</th><th>时长</th><th>主频</th><th>m 均值</th><th>m 峰值</th><th>Pst 均值</th><th>SVM 均值</th><th>光照</th><th>判定</th></tr>';
  clsList.forEach((r,i)=>{
    h+='<tr><td>'+(i+1)+'</td><td>'+(r.tea?ymd(new Date(r.tea*1000)):'--')+'</td><td>'+esc(r.lb)+'</td><td>'+durName(r.w||29)+'</td><td>'+r.f.toFixed(1)+' Hz</td><td>'+r.m.toFixed(2)+'%</td><td>'+r.mx.toFixed(2)+'%</td><td>'+r.p.toFixed(3)+'</td><td>'+(r.s!=null?r.s.toFixed(3):'--')+'</td><td>'+(r.va!=null?r.va:'--')+'</td><td class="vchip '+(VCLS[r.v]||'')+'">'+(VNAME[r.v]||'--')+'</td></tr>';
  });
  $('clstbl').innerHTML=h;
  $('clscnt').textContent=clsList.length;
  const ls=[...new Set(clsList.map(r=>r.lb))];      // 历史标签下拉: 同一灯具复查请填写完全相同的标签
  $('lbllist').innerHTML=ls.map(l=>'<option value="'+esc(l)+'"></option>').join('');
}

// ---------------- 灯具趋势分析 (同一标签 = 同一灯具; 稳健回归 + 越限预测) ----------------
// 只用带真实日期 (tea) 的记录; 斜率用 Theil-Sen (两两斜率取中位数, 抗单点异常),
// 变化速率按"每周"表述 (与测量周期无关), 越限预测 = 线性外推到限值的预计天数
const TRM={
  p :{n:'Pst^LM',u:'',lim:1.0,noise:0.03,dec:3,get:r=>r.p},
  s :{n:'SVM',u:'',lim:0.4,noise:0.015,dec:3,get:r=>r.s},
  m :{n:'波动深度 m',u:'%',lim:null,noise:1.0,dec:2,get:r=>r.m},
  va:{n:'光照均值',u:' ADC',lim:null,noise:25,dec:0,get:r=>r.va}
};
let trLbl='',trMet='p',trCur=null,trCurM=TRM.p;

const pad2=n=>String(n).padStart(2,'0');
const ymd=d=>d?(d.getFullYear()+'-'+pad2(d.getMonth()+1)+'-'+pad2(d.getDate())):'';
const md=d=>d?((d.getMonth()+1)+'-'+pad2(d.getDate())):'--';
function median(a){const b=a.slice().sort((x,y)=>x-y);return b.length?b[b.length>>1]:0;}
function theilSen(xs,ys){
  const sl=[];
  for(let i=0;i<xs.length;i++)for(let j=i+1;j<xs.length;j++){
    const dx=xs[j]-xs[i];
    if(dx>0)sl.push((ys[j]-ys[i])/dx);
  }
  const b=sl.length?median(sl):0;
  return {b:b,a:median(xs.map((x,i)=>ys[i]-b*x))};
}
function trGroups(){
  const g={};
  clsList.forEach(r=>{if(!r.tea)return;(g[r.lb]=g[r.lb]||[]).push(r);});
  for(const k in g)g[k].sort((a,b)=>a.tea-b.tea);
  return g;
}
// 拟合: pts=[{d:Date,y:数值,r:记录}]; 样本 <3 或跨度 0 时 ok=false (只画点, 不给趋势结论)
function trFit(pts,M){
  const t0=pts[0].d.getTime();
  const xs=pts.map(p=>(p.d.getTime()-t0)/86400000),ys=pts.map(p=>p.y);
  const o={pts:pts,xs:xs,ys:ys,span:xs[xs.length-1],n:pts.length,d0:pts[0].d,d1:pts[pts.length-1].d,last:ys[ys.length-1],ok:false};
  if(pts.length>=3&&o.span>0){
    const f=theilSen(xs,ys),res=ys.map((y,i)=>y-(f.a+f.b*xs[i]));
    const ym=ys.reduce((a,b)=>a+b,0)/ys.length;
    let ss=0,st=0;
    res.forEach((e,i)=>{ss+=e*e;st+=(ys[i]-ym)*(ys[i]-ym);});
    o.a=f.a;o.b=f.b;o.perWeek=f.b*7;o.mad=median(res.map(Math.abs));
    o.r2=st>0?1-ss/st:1;o.ok=true;
    o.level=trLevel(o,M);o.days=trPredict(o,M);
  }
  return o;
}
function trLevel(f,M){
  const w=f.perWeek;
  if(Math.abs(w)<M.noise)return {k:'flat',t:'稳定 (无显著变化)'};
  const adv=w>0;
  if(Math.abs(w)>=3*M.noise)return {k:adv?'bad':'good',t:adv?'明显恶化':'明显改善'};
  return {k:adv?'warn':'good',t:adv?'缓慢恶化':'缓慢改善'};
}
function trPredict(f,M){
  if(!f.ok||M.lim==null||f.b<=0)return null;   // 未上升 → 不作越限预测
  if(f.last>=M.lim)return 0;                   // 已越限
  return (M.lim-f.last)/f.b;                   // 按当前速率越限所需天数
}
function trPick(){trLbl=$('trLbl').value;trMet=$('trMet').value;trAnalyze();}
function trRefresh(){
  const g=trGroups(),keys=Object.keys(g).sort();
  if(!keys.length){
    $('trLbl').innerHTML='<option value="">— 无带日期的记录 —</option>';
    trLbl='';trCur=null;trAnalyze();return;
  }
  if(!keys.includes(trLbl))trLbl=keys[keys.length-1];       // 默认选最近一次测量的灯具
  $('trLbl').innerHTML=keys.map(k=>'<option value="'+esc(k)+'">'+esc(k)+' ('+g[k].length+' 次)</option>').join('');
  $('trLbl').value=trLbl;
  trAnalyze();
}
function trAnalyze(){
  const g=trGroups(),rs=g[trLbl]||[],M=TRM[trMet];
  const pts=rs.filter(r=>M.get(r)!=null).map(r=>({d:new Date(r.tea*1000),y:+M.get(r),r:r}));
  trCurM=M;
  trCur=pts.length?trFit(pts,M):null;
  const fx=n=>n.toFixed(M.dec);
  if(!trCur){
    $('trStat').textContent='该灯具暂无带日期的记录 (从网页开始测量会自动记录日期)';
  }else{
    const f=trCur;
    let s='样本 '+f.n+' 次 · 跨度 '+f.span.toFixed(1)+' 天 ('+ymd(f.d0)+' ~ '+ymd(f.d1)+')';
    if(f.ok){
      s+=' · 变化速率 '+(f.perWeek>=0?'+':'')+fx(f.perWeek)+M.u+'/周 · R² '+f.r2.toFixed(2)+' · 趋势: '+f.level.t;
      if(M.lim!=null)s+=' · 限值 '+M.lim;
    }else{
      s+=' · 样本不足, 暂不出趋势结论 (需 ≥3 次且时间跨度 >0 天)';
    }
    $('trStat').textContent=s;
  }
  // 预警条: 超标 / 恶化趋势与越限预测 / 光衰 / 稳定
  const W=[],f=trCur,last=rs[rs.length-1];
  if(last&&(last.v===1||last.v>=2))
    W.push(['w-red','最近一次测量 ('+ymd(new Date(last.tea*1000))+') 判定为「'+(VNAME[last.v]||'--')+'」, 建议现场复核']);
  if(f&&f.ok&&f.level.k!=='flat'){
    const rate=(f.perWeek>=0?'+':'')+fx(f.perWeek)+M.u+'/周';
    if(f.perWeek>0){
      let t=M.n+' 呈上升趋势 ('+rate+')';
      if(f.days===0)t+=' · 已越过限值 '+M.lim;
      else if(f.days!=null){
        const dd=new Date(Date.now()+f.days*86400000);
        t+=' · 按此速率预计 '+Math.max(1,Math.round(f.days/7))+' 周后 ('+ymd(dd)+') 越过限值 '+M.lim;
      }
      W.push([f.level.k==='bad'?'w-red':'w-org',t]);
    }else{
      W.push(['w-grn',M.n+' 呈下降趋势 ('+rate+'), 指标在改善']);
    }
  }
  // 光衰检查 (与所选指标无关, 始终执行): 光照均值下降往往早于频闪指标恶化出现
  const vp=rs.filter(r=>r.va!=null).map(r=>({d:new Date(r.tea*1000),y:+r.va,r:r}));
  const vf=vp.length?trFit(vp,TRM.va):null;
  if(vf&&vf.ok&&vf.perWeek<0&&Math.abs(vf.perWeek)>=TRM.va.noise)
    W.push(['w-org','光照均值以 '+vf.perWeek.toFixed(0)+' ADC/周 下降 (累计 '+Math.abs(vf.b*vf.span).toFixed(0)+
            ' ADC), 提示灯具光衰或积灰, 建议清洁灯具后复测确认']);
  if(!W.length&&f&&f.ok)
    W.push(['w-grn','近 '+f.n+' 次测量指标稳定 (跨度 '+f.span.toFixed(1)+' 天), 未见恶化迹象']);
  $('trWarn').innerHTML=W.map(x=>'<div class="trend-w '+x[0]+'">'+esc(x[1])+'</div>').join('');
  drawTrend();
}
function drawTrend(){
  const cv=$('trend');if(!cv)return;
  const c=setup(cv),ctx=c.ctx,w=c.w,h=c.h;
  if(!w||!h)return;
  const S=SC(),M=trCurM,f=trCur;
  const L=Math.round(46*S),R=Math.round(14*S),T=Math.round(12*S),B=Math.round(22*S),gw=w-L-R,gh=h-T-B;
  ctx.font=(10*S)+'px monospace';ctx.lineWidth=1;
  if(!f){
    ctx.fillStyle='#8b949e';ctx.fillText('无数据 (该标签暂无带日期的记录)',L,T+gh/2);
    return;
  }
  const span=Math.max(f.span,1e-9),day=dt=>(dt.getTime()-f.d0.getTime())/86400000;
  let lo=Math.min(...f.ys),hi=Math.max(...f.ys);
  if(M.lim!=null){lo=Math.min(lo,M.lim);hi=Math.max(hi,M.lim);}
  let pad=(hi-lo)*0.18;if(!(pad>0))pad=Math.max(0.1,Math.abs(hi)*0.1);
  lo-=pad;hi+=pad;
  if(trMet==='va')lo=Math.max(0,lo);
  const X=dt=>L+gw*Math.min(1,Math.max(0,day(dt)/span)),Y=v=>T+gh*(1-(v-lo)/(hi-lo));
  ctx.strokeStyle='#21262d';
  for(let g=0;g<=4;g++){                                  // 横向网格 + Y 轴刻度
    const v=lo+(hi-lo)*g/4,yy=Math.round(Y(v))+.5;
    ctx.beginPath();ctx.moveTo(L,yy);ctx.lineTo(w-R,yy);ctx.stroke();
    ctx.fillStyle='#8b949e';ctx.fillText(v.toFixed(M.dec),3,yy+3);
  }
  ctx.fillStyle='#8b949e';
  ctx.fillText(md(f.d0),L,h-6*S);                         // 首末日期 (X 轴)
  ctx.fillText(md(f.d1),w-R-Math.round(30*S),h-6*S);
  if(M.lim!=null&&M.lim>=lo&&M.lim<=hi){                  // 限值线
    const yy=Math.round(Y(M.lim))+.5;
    ctx.strokeStyle='#f85149';ctx.setLineDash([5,4]);
    ctx.beginPath();ctx.moveTo(L,yy);ctx.lineTo(w-R,yy);ctx.stroke();ctx.setLineDash([]);
    ctx.fillStyle='#f85149';ctx.fillText('限值 '+M.lim,w-R-Math.round(58*S),yy-4);
  }
  if(f.ok){                                               // 拟合线 (橙色虚线)
    ctx.strokeStyle='#f0883e';ctx.setLineDash([5,4]);
    ctx.beginPath();
    ctx.moveTo(X(f.d0),Y(f.a));
    ctx.lineTo(X(f.d1),Y(f.a+f.b*f.span));
    ctx.stroke();ctx.setLineDash([]);
  }
  if(f.n>1){                                              // 时间轴折线
    ctx.strokeStyle='#58a6ff';ctx.lineWidth=1.5;ctx.beginPath();
    f.pts.forEach((p,i)=>{const x=X(p.d),y=Y(p.y);i?ctx.lineTo(x,y):ctx.moveTo(x,y);});
    ctx.stroke();ctx.lineWidth=1;
  }
  f.pts.forEach(p=>{                                      // 数据点: 合规绿 / 超标红
    ctx.beginPath();ctx.arc(X(p.d),Y(p.y),3*S,0,Math.PI*2);
    ctx.fillStyle=(p.r.v===0||p.r.v===4)?'#3fb950':'#f85149';
    ctx.fill();
  });
}

async function clsRefresh(){
  try{
    const r=await fetch('/api/classroom');
    const j=await r.json();
    clsList=j.records||[];
    paintClsTable();
    if(j.latest)paintClsRes(j.latest);
    $('clsbtn').disabled=!!j.run;
    if(!j.run)$('clspbar').style.width=(j.nwin>=(j.tot||29)?'100%':'0%');
    trRefresh();
  }catch(e){}
}
async function clsStart(){
  const lbl=$('clslbl').value.trim(),dur=$('clsdur').value;
  try{
    // t: 浏览器时间 (Unix 秒) → 记录真实测量日期, 供趋势分析使用 (设备离线也能带上日期)
    const r=await fetch('/api/classroom/start?label='+encodeURIComponent(lbl)+'&dur='+dur+
                        '&t='+Math.floor(Date.now()/1000),{method:'POST'});
    if(!r.ok){const j=await r.json().catch(()=>({}));throw new Error(j.err||r.status);}
    $('clsbtn').disabled=true;
    const wn={30:29,60:58,600:586}[dur]||29,dl=dur>=60?(dur/60)+' min':dur+' s';
    $('clsstat').textContent='测量中 0 / '+wn+' 窗口 (~'+dl+')...';
  }catch(e){alert('开始失败: '+e);}
}
async function clsClear(){
  if(!confirm('确定清空全部测量记录?'))return;
  try{await fetch('/api/classroom/clear',{method:'POST'});await clsRefresh();
      $('clsv').textContent='--';$('clsv').className='value';
      $('clslbl2').textContent='尚未测量';}catch(e){alert('清空失败: '+e);}
}
function clsCsv(){
  const rows=[['#','标签','日期','时长','T+秒','主频Hz','m均值%','m峰值%','FlickerIndex','Pst均值','Pst峰值','SVM均值','SVM峰值','光照ADC','判定']];
  clsList.forEach((r,i)=>rows.push([i+1,r.lb,(r.tea?ymd(new Date(r.tea*1000)):''),durName(r.w||29),r.ts,r.f.toFixed(2),r.m.toFixed(2),r.mx.toFixed(2),r.fi.toFixed(3),r.p.toFixed(3),r.px.toFixed(3),(r.s!=null?r.s.toFixed(3):''),(r.sx!=null?r.sx.toFixed(3):''),(r.va!=null?r.va:''),VNAME[r.v]||'']));
  const csv='\ufeff'+rows.map(r=>r.map(c=>'"'+String(c).replace(/"/g,'""')+'"').join(',')).join('\r\n');
  const a=document.createElement('a');
  a.href=URL.createObjectURL(new Blob([csv],{type:'text/csv;charset=utf-8'}));
  a.download='classroom.csv';
  a.click();
  URL.revokeObjectURL(a.href);
}
function clsUpdate(c){
  const pct=c.res?100:Math.round(100*c.win/(c.tot||29));
  $('clspbar').style.width=pct+'%';
  if(c.run){
    $('clsbtn').disabled=true;
    $('clsstat').textContent='测量中 '+c.win+' / '+c.tot+' 窗口 (~'+durName(c.tot)+') · 实时 m '+c.m.toFixed(2)+'% · '+c.freq.toFixed(1)+' Hz';
  }else{
    $('clsbtn').disabled=false;
    if(c.res){
      paintClsRes(c.res);
      clsRefresh();
      $('clsstat').textContent='测量完成 ('+durName(c.res.w||29)+'): '+(VNAME[c.res.v]||'');
    }else if($('clsstat').textContent.startsWith('测量中')){
      $('clsstat').textContent='待机 · 时长可选 30 s / 1 min / 10 min';
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

// ---------------- 大屏演示模式 (投影展示: 放大字号/曲线/表格) ----------------
function applyBig(on){
  document.body.classList.toggle('big',on);
  $('bigbtn').classList.toggle('on',on);
  try{localStorage.setItem('lsBig',on?'1':'0');}catch(e){}
  if(lastD)render(lastD);                 // 立即按新尺寸重绘 canvas (setup() 会重取 clientHeight)
  drawTrend();
}
function toggleBig(){applyBig(!document.body.classList.contains('big'));}
// URL 参数 ?big=1 / ?big=0 优先, 否则沿用上次选择
(function(){
  let q=null,saved=null;
  try{q=new URLSearchParams(location.search).get('big');}catch(e){}
  try{saved=localStorage.getItem('lsBig');}catch(e){}
  applyBig(q==='1'||(q!=='0'&&saved==='1'));
})();

connect();

function render(d){
  lastD=d;                                                 // 缓存末帧, 供大屏模式切换时立即重绘
  if(d.led!=null&&MODES[d.led])paintLed(MODES[d.led].p);   // 按键切换 LED 模式后同步高亮
  $('flk').textContent=d.flk.toFixed(2);
  $('fi').textContent=d.fi.toFixed(3);
  $('pst').textContent=d.pst.toFixed(3);
  $('svm').textContent=(d.svm||0).toFixed(3);
  $('freq').textContent=d.freq.toFixed(2);
  $('vmax').textContent=d.vmax;
  $('vmin').textContent=d.vmin;
  $('vavg').textContent=d.vavg.toFixed(1);
  lastFreq=d.freq;
  const z=$('zone');
  z.textContent=VNAME[d.zcode]||d.zone;
  z.className='value '+(VCLS[d.zcode]||'z-pass');
  drawWave(d.wmin,d.wmax);
  drawSpec(d.spec);
  $('meta').textContent='窗口 #'+d.seq+' · '+d.samples+' 样本 · DMA 溢出 '+d.ovf+
    (d.ovf>0?' (数据丢失!)':'')+' · 更新于 '+new Date().toLocaleTimeString();
  if(d.cls)clsUpdate(d.cls);
}

// 大屏模式绘图缩放系数 (canvas 内字号与边距随 body.big 放大)
const SC=()=>document.body.classList.contains('big')?1.6:1;

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
  const L=Math.round(46*SC()),R=10,T=Math.round(10*SC()),B=Math.round(20*SC()),gw=w-L-R,gh=h-T-B;
  const y=v=>T+gh*(1-v/4095);
  ctx.font=(10*SC())+'px monospace';ctx.lineWidth=1;
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
  ctx.fillText('0 ms',L,h-6*SC());
  ctx.fillText('204.8 ms',w-R-Math.round(52*SC()),h-6*SC());
}

function drawSpec(sp){
  const c=setup($('spec')),ctx=c.ctx,w=c.w,h=c.h;
  const L=Math.round(46*SC()),R=10,T=Math.round(10*SC()),B=Math.round(20*SC()),gw=w-L-R,gh=h-T-B;
  // 对数横轴: 后端频谱点已按对数频带分组 (4.883 Hz ~ 5 kHz), 组号线性 = 对数线性
  const FMIN=4.883,FMAX=5000,xg=f=>L+gw*Math.log(f/FMIN)/Math.log(FMAX/FMIN);
  ctx.font=(10*SC())+'px monospace';ctx.lineWidth=1;
  ctx.strokeStyle='#21262d';
  ctx.beginPath();ctx.moveTo(Math.round(L)+.5,T);ctx.lineTo(Math.round(L)+.5,T+gh);
  ctx.moveTo(w-R-.5,T);ctx.lineTo(w-R-.5,T+gh);ctx.stroke();
  for(const f of [10,100,1000]){
    const x=Math.round(xg(f))+.5;
    ctx.beginPath();ctx.moveTo(x,T);ctx.lineTo(x,T+gh);ctx.stroke();
    ctx.fillStyle='#8b949e';ctx.fillText(f>=1000?'1k':''+f,x-6*SC(),h-6*SC());
  }
  ctx.fillStyle='#8b949e';ctx.fillText('5k',w-R-14*SC(),h-6*SC());
  if(!sp||!sp.length)return;
  let mx=0;for(const v of sp)if(v>mx)mx=v;
  if(mx<=0)mx=1;
  const n=sp.length;
  ctx.beginPath();ctx.moveTo(L,T+gh);
  for(let i=0;i<n;i++)ctx.lineTo(L+gw*i/(n-1),T+gh*(1-sp[i]/mx));
  ctx.lineTo(L+gw,T+gh);ctx.closePath();
  ctx.fillStyle='rgba(63,185,80,0.15)';ctx.fill();
  ctx.strokeStyle='#3fb950';ctx.lineWidth=1.2;ctx.stroke();
  if(lastFreq>=FMIN&&lastFreq<=FMAX){
    const x=Math.round(xg(lastFreq))+.5;
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

// ---------------- 云端配网页 (/setup, 独立小页, 密钥只存设备 NVS) ----------------
static const char SETUP_HTML[] PROGMEM = R"rawliteral(
<!doctype html><html lang=zh><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>LightSense 云端配置</title>
<style>
body{font-family:system-ui,'Microsoft YaHei',sans-serif;background:#0f172a;color:#e2e8f0;display:flex;justify-content:center;padding:24px;margin:0}
form{background:#1e293b;padding:20px;border-radius:12px;max-width:420px;width:100%}
h2{margin:0 0 12px;font-size:18px}p{font-size:12px;color:#94a3b8;margin:6px 0}
label{display:block;margin:10px 0 4px;font-size:13px;color:#94a3b8}
input,select{width:100%;box-sizing:border-box;padding:8px;border-radius:6px;border:1px solid #334155;background:#0f172a;color:#e2e8f0}
.sc{display:flex;gap:8px;align-items:center;margin-top:6px}
.sc button{margin-top:0;width:auto;padding:6px 12px;font-size:13px}
button{margin-top:16px;width:100%;padding:10px;border:0;border-radius:8px;background:#2b6cb0;color:#fff;font-size:15px}
button:disabled{opacity:.5}.msg{margin-top:10px;font-size:13px;color:#7dd3fc}
.st{margin-top:14px;font-size:13px}
</style></head><body><form id=f>
<h2>OneNET 云端配置</h2>
<p>参数存设备本地 NVS, 不上传; Token 生成方法见仓库 src/cloud_cfg.h.example</p>
<label>WiFi 名称 (SSID)</label>
<select id=ssidsel><option value="">— 点击下方按钮扫描 WiFi —</option></select>
<div class=sc><button id=scant type=button>扫描 WiFi</button><div class=msg id=scanm style="margin:0"></div></div>
<input id=ssid maxlength=32 placeholder="也可直接手动输入 WiFi 名称">
<label>WiFi 密码 (可空 = 开放网络)</label><input id=pass maxlength=64>
<label>产品 ID (product_id)</label><input id=pid maxlength=24>
<label>设备名称 (device_name)</label><input id=dev maxlength=24>
<label>设备 Token (MQTT 密码, 整串)</label><input id=tok maxlength=255>
<button id=b type=submit>保存并重启</button>
<div class=msg id=m></div>
<div class=st id=s></div>
</form>
<script>
fetch('/api/cloud').then(r=>r.json()).then(c=>{
 ssid.value=c.ssid||'';pass.value=c.pass||'';pid.value=c.pid||'';
 dev.value=c.dev||'';tok.value=c.tok||'';
 s.textContent='连接状态: '+c.stn+(c.cfg?' (已配置)':' (未配置)');
});
let scanning=false;
scant.onclick=()=>{
 if(scanning)return;scanning=true;scanm.textContent='扫描中...';scant.disabled=true;
 let tries=0;
 const poll=()=>fetch('/api/wifiscan').then(r=>r.json()).then(d=>{
  if(!d.done){if(++tries>12){scanm.textContent='扫描超时,请重试';scanning=false;scant.disabled=false;return;}
   setTimeout(poll,1000);return;}
  ssidsel.innerHTML='<option value="">— 选择扫描到的 WiFi —</option>';
  d.nets.forEach(n=>{const o=document.createElement('option');
   o.value=n.ssid;o.textContent=n.ssid+' ('+n.rssi+'dBm'+(n.enc?'':' 开放')+')';
   ssidsel.appendChild(o);});
  scanm.textContent=d.nets.length+' 个网络';
  scanning=false;scant.disabled=false;
 }).catch(()=>{scanm.textContent='请求失败';scanning=false;scant.disabled=false;});
 poll();
};
ssidsel.onchange=()=>{if(ssidsel.value)ssid.value=ssidsel.value;};
f.onsubmit=async e=>{
 e.preventDefault();b.disabled=true;m.textContent='保存中...';
 const b2=new URLSearchParams();
 b2.set('ssid',ssid.value);b2.set('pass',pass.value);b2.set('pid',pid.value);
 b2.set('dev',dev.value);b2.set('tok',tok.value);
 try{const r=await fetch('/setup',{method:'POST',body:b2});
     m.textContent=await r.text();}
 catch(err){m.textContent='已发送, 设备重启中...';}
};
</script></body></html>)rawliteral";

static String postParam(AsyncWebServerRequest *req, const char *k)
{
    if (req->hasParam(k, true))
        return req->getParam(k, true)->value();
    return String();
}

bool webdashBegin(void)
{
    if (WiFi.getMode() != WIFI_AP_STA)   // cloudBegin 已配置时处于 AP+STA, 勿覆盖
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
    //                    GET /led?mode=X → 切换模式 (const|50hz|100hz|500hz|1khz|5khz|off)
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
        static char cb[12 * 1024];      // 仅 async_tcp 任务调用, 单线程安全 (50 条记录 + epoch 字段留余量)
        uint16_t nR = 0;
        const ClassroomRecord *rs = classroomRecords(&nR);
        int n = snprintf(cb, sizeof(cb),
                         "{\"run\":%u,\"nwin\":%lu,\"tot\":%u,\"n\":%u,\"records\":[",
                         (unsigned)classroomIsRunning(),
                         (unsigned long)classroomWindowsDone(),
                         (unsigned)classroomWindowsTarget(), (unsigned)nR);
        for (uint16_t i = 0; i < nR && n < (int)sizeof(cb) - 192; i++)
        {
            char lb[80];
            jsonEscape(lb, sizeof(lb), rs[i].label);
            char sv[88];
            sv[0] = 0;
            if (rs[i].svmMean > 0.0f)    // 旧版本记录 svmMean==0 → 不输出 s/sx, 前端显示 --
                snprintf(sv, sizeof(sv), ",\"s\":%.3f,\"sx\":%.3f", rs[i].svmMean, rs[i].svmMax);
            if (rs[i].vavgMean > 0)      // v1.0 及以前记录无光照 → 不输出 va, 前端显示 --
                snprintf(sv + strlen(sv), sizeof(sv) - strlen(sv), ",\"va\":%u",
                         (unsigned)rs[i].vavgMean);
            if (rs[i].epoch)             // 无真实日期 (旧记录/未校时) → 不输出 tea, 前端排除在趋势外
                snprintf(sv + strlen(sv), sizeof(sv) - strlen(sv), ",\"tea\":%lu",
                         (unsigned long)rs[i].epoch);
            n += snprintf(cb + n, sizeof(cb) - n,
                          "%s{\"i\":%u,\"lb\":\"%s\",\"ts\":%lu,\"w\":%u,\"m\":%.2f,\"mx\":%.2f,"
                          "\"fi\":%.3f,\"p\":%.3f,\"px\":%.3f,\"f\":%.2f,\"v\":%u%s}",
                          (i ? "," : ""), (unsigned)i, lb,
                          (unsigned long)rs[i].ts, (unsigned)rs[i].windows,
                          rs[i].mMean, rs[i].mMax,
                          rs[i].fi, rs[i].pstMean, rs[i].pstMax, rs[i].freq,
                          (unsigned)rs[i].verdict, sv);
        }
        n += snprintf(cb + n, sizeof(cb) - n, "],\"latest\":");
        if (nR > 0)
        {
            const ClassroomRecord *L = &rs[nR - 1];
            char lb[80];
            jsonEscape(lb, sizeof(lb), L->label);
            char sv[88];
            sv[0] = 0;
            if (L->svmMean > 0.0f)
                snprintf(sv, sizeof(sv), ",\"s\":%.3f,\"sx\":%.3f", L->svmMean, L->svmMax);
            if (L->vavgMean > 0)
                snprintf(sv + strlen(sv), sizeof(sv) - strlen(sv), ",\"va\":%u",
                         (unsigned)L->vavgMean);
            if (L->epoch)
                snprintf(sv + strlen(sv), sizeof(sv) - strlen(sv), ",\"tea\":%lu",
                         (unsigned long)L->epoch);
            n += snprintf(cb + n, sizeof(cb) - n,
                          "{\"lb\":\"%s\",\"m\":%.2f,\"mmax\":%.2f,\"fi\":%.3f,"
                          "\"pst\":%.3f,\"pstmax\":%.3f,\"freq\":%.2f,\"v\":%u,\"ts\":%lu,\"w\":%u%s}",
                          lb, L->mMean, L->mMax, L->fi, L->pstMean, L->pstMax,
                          L->freq, (unsigned)L->verdict, (unsigned long)L->ts,
                          (unsigned)L->windows, sv);
        }
        else
        {
            n += snprintf(cb + n, sizeof(cb) - n, "null");
        }
        snprintf(cb + n, sizeof(cb) - n, "}");
        req->send(200, "application/json", cb);
    });

    // 教室测量: 开始一次测量 (?label=灯具标签, ?dur=30|60|600 秒, ?t=Unix秒, 均可省略, 默认 30s 档)
    // t 由网页用浏览器时间填入 (设备离线也能记录真实测量日期); 缺省时完成时回落 NTP 校时
    server.on("/api/classroom/start", HTTP_POST, [](AsyncWebServerRequest *req)
    {
        String label = "";
        if (req->hasParam("label"))
            label = req->getParam("label")->value();
        utf8Trunc(label, 31);
        uint16_t wins = CLS_WINDOWS;
        if (req->hasParam("dur"))
        {
            long dur = req->getParam("dur")->value().toInt();
            if (dur >= 300)      wins = CLS_WINDOWS_10MIN;
            else if (dur >= 60)  wins = CLS_WINDOWS_1MIN;
        }
        uint32_t epoch = 0;                       // 合理区间 (2020-09 ~ 2106) 之外的取值视为无效
        if (req->hasParam("t"))
        {
            long tv = req->getParam("t")->value().toInt();
            if (tv > 1600000000L) epoch = (uint32_t)tv;
        }
        if (!classroomStart(label.c_str(), wins, epoch))
        {
            req->send(409, "application/json", "{\"err\":\"busy or records full\"}");
            return;
        }
        req->send(200, "application/json", "{\"ok\":1}");
    });

    // 教室测量: 清空全部记录 (同时清云端同步位图, 记录索引重新从 0 对应)
    server.on("/api/classroom/clear", HTTP_POST, [](AsyncWebServerRequest *req)
    {
        classroomClear();
        cloudResetSync();
        req->send(200, "application/json", "{\"ok\":1}");
    });

    // OneNET 云端配置: 配网页 + 配置回显 (保存后 onDisconnect 时重启生效)
    server.on("/setup", HTTP_GET, [](AsyncWebServerRequest *req)
              { req->send_P(200, "text/html", SETUP_HTML); });
    server.on("/setup", HTTP_POST, [](AsyncWebServerRequest *req)
    {
        String ssid = postParam(req, "ssid");
        String pass = postParam(req, "pass");
        String pid = postParam(req, "pid");
        String dev = postParam(req, "dev");
        String tok = postParam(req, "tok");
        if (ssid.length() == 0 || pid.length() == 0 || dev.length() == 0 || tok.length() == 0)
        {
            req->send(400, "text/plain; charset=utf-8", "WiFi/产品ID/设备名/Token 不能为空");
            return;
        }
        if (ssid.length() > 32 || pass.length() > 64 || pid.length() > 24 ||
            dev.length() > 24 || tok.length() > 256)
        {
            req->send(400, "text/plain; charset=utf-8", "参数过长");
            return;
        }
        cloudCfgSave(ssid, pass, pid, dev, tok);
        req->onDisconnect([]()
                          { ESP.restart(); });
        req->send(200, "text/plain; charset=utf-8", "已保存, 设备正在重启...");
    });
    server.on("/api/cloud", HTTP_GET, [](AsyncWebServerRequest *req)
    {
        char ssid[33], pass[65], pid[25], dev[25], tok[257];
        bool cfg = cloudCfgGet(ssid, sizeof(ssid), pass, sizeof(pass), pid, sizeof(pid),
                               dev, sizeof(dev), tok, sizeof(tok));
        char e1[67], e2[131], e3[51], e4[51], e5[519], buf[960];
        jsonEscape(e1, sizeof(e1), ssid);
        jsonEscape(e2, sizeof(e2), pass);
        jsonEscape(e3, sizeof(e3), pid);
        jsonEscape(e4, sizeof(e4), dev);
        jsonEscape(e5, sizeof(e5), tok);
        snprintf(buf, sizeof(buf),
                 "{\"ok\":1,\"cfg\":%u,\"state\":%u,\"stn\":\"%s\","
                 "\"ssid\":\"%s\",\"pass\":\"%s\",\"pid\":\"%s\",\"dev\":\"%s\",\"tok\":\"%s\"}",
                 (unsigned)cfg, (unsigned)cloudGetState(), cloudStateName(),
                 e1, e2, e3, e4, e5);
        req->send(200, "application/json", buf);
    });

    // WiFi 异步扫描: 首次调用触发后台扫描, 前端每秒轮询, 完成后返回网络列表
    // (扫描由 WiFi 驱动异步执行, 不阻塞主循环, 测量节拍不受影响)
    server.on("/api/wifiscan", HTTP_GET, [](AsyncWebServerRequest *req)
    {
        int16_t n = WiFi.scanComplete();
        if (n < 0)                       // FAILED(-2)→启动异步扫描; RUNNING(-1)→继续等
        {
            if (n == WIFI_SCAN_FAILED)
                WiFi.scanNetworks(true, false);
            req->send(200, "application/json", "{\"ok\":1,\"done\":0,\"nets\":[]}");
            return;
        }
        String json = "{\"ok\":1,\"done\":1,\"nets\":[";
        for (int16_t i = 0; i < n && i < 20; i++)   // 限前 20 个 (按信号强度排序)
        {
            char e[67], item[176];
            jsonEscape(e, sizeof(e), WiFi.SSID(i).c_str());
            snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"enc\":%u}",
                     i ? "," : "", e, (int)WiFi.RSSI(i), (unsigned)WiFi.encryptionType(i));
            json += item;
        }
        json += "]}";
        WiFi.scanDelete();
        req->send(200, "application/json", json);
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
// 格式: {"seq","samples","ovf","vmax","vmin","vavg","flk","fi","pst","svm","freq","zcode","zone",
//        "cls":{"run","win","tot","m","freq"[,"res"]},"wmin":[...],"wmax":[...],"spec":[...]}
static char jsonBuf[12 * 1024];

static size_t buildJson(const FlickerSnapshot *s)
{
    static const char *zoneNames[] = {"PASS", "FAIL-M", "FAIL-PST", "FAIL-BOTH",
                                      "EXEMPT", "FAIL-SVM", "DARK", "BRIGHT"};
    const char *zone = zoneNames[s->zone];

    int n = snprintf(jsonBuf, sizeof(jsonBuf),
                     "{\"seq\":%lu,\"samples\":%lu,\"ovf\":%lu,"
                     "\"vmax\":%u,\"vmin\":%u,\"vavg\":%.1f,"
                     "\"flk\":%.2f,\"fi\":%.3f,\"pst\":%.3f,"
                     "\"svm\":%.3f,\"freq\":%.2f,"
                     "\"zcode\":%u,\"zone\":\"%s\","
                     "\"wmin\":[",
                     (unsigned long)s->seq, (unsigned long)s->samples,
                     (unsigned long)s->overflow,
                     (unsigned)s->vmax, (unsigned)s->vmin, s->vavg,
                     s->flickerPercent, s->flickerIndex, s->pstLM, s->svm,
                     s->freqHz,
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
                  (unsigned)classroomWindowsTarget(), s->flickerPercent, s->freqHz);
    ClassroomRecord cr;
    if (classroomPopResult(&cr))
    {
        char lb[80];
        jsonEscape(lb, sizeof(lb), cr.label);
        char sv[56];
        sv[0] = 0;
        if (cr.svmMean > 0.0f)
            snprintf(sv, sizeof(sv), ",\"s\":%.3f,\"sx\":%.3f", cr.svmMean, cr.svmMax);
        if (cr.vavgMean > 0)
            snprintf(sv + strlen(sv), sizeof(sv) - strlen(sv), ",\"va\":%u",
                     (unsigned)cr.vavgMean);
        n += snprintf(jsonBuf + n, sizeof(jsonBuf) - n,
                      ",\"res\":{\"lb\":\"%s\",\"m\":%.2f,\"mx\":%.2f,\"fi\":%.3f,"
                      "\"p\":%.3f,\"px\":%.3f,\"f\":%.2f,\"v\":%u,\"ts\":%lu,\"w\":%u%s}",
                      lb, cr.mMean, cr.mMax, cr.fi, cr.pstMean, cr.pstMax,
                      cr.freq, (unsigned)cr.verdict, (unsigned long)cr.ts,
                      (unsigned)cr.windows, sv);
    }

    // LED 当前模式 (v1.3: 按键切换后网页 LED 页按钮高亮同步) + 云端状态 (v1.4)
    n += snprintf(jsonBuf + n, sizeof(jsonBuf) - n, ",\"led\":%u,\"cloud\":\"%s\"}}",
                  (unsigned)ledPwmGetMode(), cloudStateName());

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
