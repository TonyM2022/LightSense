// ---------------- WiFi 网页仪表层实现 ----------------
// AP 热点 (config.h 配置 SSID/密码) + 异步 Web 服务器:
//   GET /     → 内嵌网页 (PROGMEM, 三页面: 频闪仪仪表 / LED 控制 / 教室测量)
//   WS  /ws   → 每报告窗口广播一次 JSON 快照 (指标 + 波形包络 + 频谱 + 教室进度)
//   GET /led  → 查询/设置 LED 输出模式 (?mode=const|50hz|100hz|500hz|1khz|5khz|off)
//   GET  /api/classroom        → 教室测量状态 + 记录列表
//   POST /api/classroom/start  → 开始测量 (?label=灯具标签&dur=30|60|600 秒&t=Unix秒)
//                                ?pt=1..5 = 巡检测点 (标签由设备按 <教室号>-<点位码> 生成)
//   POST /api/classroom/clear  → 清空记录 (同时清云端同步位图与巡检任务)
//   GET  /api/inspect          → 教室巡检: 当前任务 + 任务列表 + 教室级汇总 sum (记录带 tid/pc 归属)
//   POST /api/inspect/start    → 新建巡检 (?room=教室号&scene=场景&ins=巡检人&t=Unix秒)
//   POST /api/inspect/abort    → 中止当前巡检任务
//   POST /api/inspect/finish   → 结束当前巡检任务
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
/* ---- 教室巡检 (5 点标准测点 + 教室级汇总 + 整改前后复测对比) ---- */
.ins-grid{display:flex;flex-wrap:wrap;gap:8px;margin:10px 0}
.ptchip{border:1px solid var(--edge);border-radius:8px;padding:8px 10px;font-size:13px;color:var(--dim);min-width:96px;line-height:1.5}
.ptchip b{display:block;font-size:15px;color:inherit}
.ptchip.done{border-color:#3fb950;color:#3fb950;background:rgba(63,185,80,.10)}
.ptchip.fail{border-color:#f85149;color:#f85149;background:rgba(248,81,73,.10)}
.ptchip.cur{border-color:#58a6ff;color:#58a6ff;background:rgba(88,166,255,.10)}
.vbig{font-size:20px;font-weight:600;margin-bottom:6px}
.adv{font-size:13px;padding:9px 11px;border-radius:8px;margin-top:6px;border:1px solid var(--edge);color:var(--dim)}
/* .adv 与 .w-* 同为单类选择器且 .adv 在后, 需用 .adv.w-* 显式覆盖配色 */
.adv.w-red{border-color:#f85149;color:#f85149;background:rgba(248,81,73,.10)}
.adv.w-org{border-color:#f0883e;color:#f0883e;background:rgba(240,136,62,.10)}
.adv.w-grn{border-color:#3fb950;color:#3fb950;background:rgba(63,185,80,.10)}
body.big .ptchip{font-size:17px;min-width:132px}
body.big .ptchip b{font-size:19px}
body.big .vbig{font-size:32px}
body.big .adv{font-size:17px;padding:12px 14px}
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
    <div class="label">教室巡检 (5 点标准测点: 中心 C / 左前 LF / 右前 RF / 左后 LB / 右后 RB)</div>
    <div class="cls-row">
      <input id="insroom" maxlength="12" placeholder="教室号, 如 A302">
      <select id="insscene">
        <option value="白天上课">白天上课</option>
        <option value="夜间自习">夜间自习</option>
        <option value="阴天">阴天</option>
        <option value="其他">其他</option>
      </select>
      <input id="insins" maxlength="8" placeholder="巡检人 (可空)">
      <select id="insdur">
        <option value="30">30 s</option>
        <option value="60">1 min</option>
        <option value="600">10 min</option>
      </select>
      <button id="insbtn" onclick="insCreate()">新建巡检</button>
    </div>
    <div class="ins-grid" id="insflow"></div>
    <div class="btn2">
      <button id="insmeas" onclick="insMeasure()" disabled style="border-color:#2ea043;color:#3fb950">测量待测点位</button>
      <button id="insfin" onclick="insFinish()" disabled>完成巡检</button>
      <button id="insabort" onclick="insAbort()" disabled>中止巡检</button>
    </div>
    <div class="meta" id="insstat" style="margin-top:8px">无进行中的巡检任务 (填教室号后点「新建巡检」)</div>
  </div>
  <div class="card wide" id="inssumcard" style="display:none">
    <div class="label">教室级汇总 (5 点测完自动给出结论与整改建议)</div>
    <div class="vbig" id="inssumv">--</div>
    <div class="meta" id="inssums">--</div>
    <table class="tbl" id="insptbl" style="margin-top:10px"></table>
    <div id="insadv"></div>
    <div class="btn2" style="margin-top:8px"><button onclick="insReport()">生成巡检报告 (一页摘要)</button></div>
  </div>
  <div class="card wide">
    <div class="label">整改前后复测对比 (同一教室的两次巡检)</div>
    <div class="cls-row">
      <select id="inscA" onchange="insCompare()" style="flex:1"></select>
      <select id="inscB" onchange="insCompare()" style="flex:1"></select>
    </div>
    <div class="meta" id="inscmp" style="margin-top:8px">--</div>
    <div id="inscmpw"></div>
  </div>
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
    <div class="meta">判定口径 (v1.1): 光照前置检查 (窗口均值 ADC 码 &lt;400 光线过暗 / &gt;3000 光线太亮, 此时 Pst/SVM 无效) → Pst^LM 取本次测量均值 ≤ 1 (IEC TR 61547-1), SVM 取本次测量均值 ≤ 0.4 (IEC TR 63158, EU 2024 生态设计限值); 波动深度与主频仅展示不判定。局限: ① 单测点只反映测点位置光照, 教室验收应多点测量 (中心+四角) 并用标签区分; ② 30s/1min 档 Pst^LM 为短时估计, 标准口径 10min (可选 10min 档); ③ 4.88Hz 频率分辨率, &lt;15Hz 主频精度有限; ④ 旧版本记录为旧口径判定或无 SVM/光照, 建议清空重测; ⑤ 同一灯具定期复查请填写完全相同的标签 (输入框提供历史标签下拉), 才会被趋势分析归为同一灯具; 趋势分析只用带日期的记录 —— 网页发起的测量自动带浏览器日期, 联网 (已配 OneNET WiFi) 时按键发起的测量也带日期; ⑥ 记录上限 80 条 (约 16 次 5 点巡检), 记录满后无法开始新的测量, 请先「导出 CSV」再「清空记录」; 清空会同时清除全部巡检任务 (记录清空后任务归属无意义)。</div>
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

// ---------------- 教室巡检 (5 点标准测点; 设备端任务 + 前端教室级汇总) ----------------
// 点位模板 (与设备 CLS_PT_* 一致): C 中心 / LF 左前 / RF 右前 / LB 左后 / RB 右后
// 设备端 InspectionTask: 任务号/教室号/场景/巡检人/已完成点位掩码; 记录带 tid+pc 归属
// 前端: 5 点聚合 → 教室级结论 (A 合规 / B 局部异常 / C 整体异常) + 最差点位 + 整改建议 + 两次巡检对比
const PTS=[['C','中心',1],['LF','左前',2],['RF','右前',3],['LB','左后',4],['RB','右后',5]];
const PTN={C:'中心',LF:'左前',RF:'右前',LB:'左后',RB:'右后'};
const ptKey=n=>{const p=PTS.find(x=>x[2]===n);return p?p[0]:'';};
const ptName=n=>{const p=PTS.find(x=>x[2]===n);return p?p[1]:'';};
const sevOf=v=>v===0?0:(v===3?3:((v===6||v===7)?1:2));   // 严重度: 双项 > 单项 > 光照异常 > 合规
let ins={active:false,id:0,mask:0,done:0,next:0,sum:null},insTasks=[],clsRun=false,insViewId=0;

async function insApi(path,opt){
  const r=await fetch(path,opt);
  const j=await r.json().catch(()=>({}));
  if(!r.ok)throw new Error(j.err||r.status);
  return j;
}
function startErr(e){
  const s=String(e);
  if(/busy|full|task/i.test(s))
    return '开始失败: 设备忙碌 / 记录已满 (上限 80 条) / 无进行中巡检任务。\n请先「导出 CSV」再「清空记录」后重试。';
  return '开始失败: '+s;
}
// 某点位的记录 (按当前查看的巡检任务 insViewId 归组; 同一任务内多次复测取最后一条)
// 用 insViewId 而非 ins.id: 任务结束后 ins.id 归零, 但汇总视图需保留上一次巡检
function insRec(pc){let out=null;clsList.forEach(r=>{if(r.tid===insViewId&&r.pc===pc)out=r;});return out;}
function insMask(){
  if(ins.active)return ins.mask||0;
  const t=insTasks.find(x=>x.id===insViewId);
  return t?(t.mask||0):0;
}

function paintIns(){
  const m=insMask();
  $('insflow').innerHTML=PTS.map((p,i)=>{
    const done=!!(m&(1<<i)),rec=done?insRec(p[2]):null,bad=done&&rec&&rec.v!==0;
    const cur=ins.active&&!done&&ins.next===p[2];
    const cls=done?(bad?' fail':' done'):(cur?' cur':'');
    const st=done?(rec?String(VNAME[rec.v]||'--').replace(/ /g,''):'已测'):(cur?'待测':'未测');
    return '<div class="ptchip'+cls+'">'+p[0]+' '+p[1]+'<b>'+st+'</b></div>';
  }).join('');
  const nx=ins.active?ins.next:0;
  const tv=insTasks.find(x=>x.id===insViewId);       // 当前查看的任务 (任务结束后仍保留汇总视图)
  $('insstat').textContent=ins.active
    ?('巡检中: 教室 '+ins.room+' · 任务 #'+ins.id+' · 已完成 '+ins.done+'/5 · 待测 '+
      (nx?(ptName(nx)+' ('+ptKey(nx)+')'):'无 (5 点已测完)'))
    :(tv?('已完成: 教室 '+tv.room+' · 任务 #'+tv.id+' · '+(tv.done||0)+'/5 (下方为汇总; 重新填教室号可开始下次巡检)')
       :'无进行中的巡检任务 (填教室号后点「新建巡检」)');
  $('insmeas').textContent=ins.active?('测量待测点位: '+(nx?ptName(nx)+' '+ptKey(nx):'无')):'测量待测点位';
  $('insbtn').disabled=ins.active||clsRun;
  $('insmeas').disabled=!ins.active||!nx||clsRun;
  $('insfin').disabled=!ins.active||clsRun;
  $('insabort').disabled=!ins.active||clsRun;
  $('insroom').disabled=$('insscene').disabled=$('insins').disabled=ins.active;
  if(ins.active||tv)                          // 进行中填入任务信息; 已结束预填教室号便于下次巡检
  {
    $('insroom').value=(ins.active?ins.room:(tv?tv.room:''))||'';
    if(ins.active){
      if(ins.scene)$('insscene').value=ins.scene;
      if(ins.ins)$('insins').value=ins.ins;
    }
  }
}

// 教室级汇总: 设备端 sum (M2 判定/建议位) + 前端点位表; 无 sum 时回落本地规则
function clsSum(){
  const got=PTS.map(p=>insRec(p[2])).filter(Boolean),card=$('inssumcard');
  if(!got.length){card.style.display='none';return;}
  card.style.display='';
  const sm=(ins.sum&&ins.sum.id===insViewId)?ins.sum:null;
  const fails=got.filter(r=>r.v!==0),nPts=sm?sm.pts:got.length,nFail=sm?sm.fail:fails.length;
  let vt,vc,desc;
  if(nPts<5){vt='待完成 '+nPts+'/5';vc='';desc='已完成 '+nPts+' 个点位, 5 点测完自动给出教室级结论';}
  else if(sm&&sm.v===1){vt='A 合规';vc='z-pass';desc='5 个点位全部合规';}
  else if(sm&&sm.v===2){vt='B 局部异常';vc='z-fail';desc=nFail+' 个点位异常 (局部问题), 建议先整改异常点位后复测';}
  else if(sm&&sm.v===3){vt='C 整体异常';vc='z-fail';desc=nFail+' 个点位异常 (涉及面广), 疑似共性供电/驱动问题';}
  else if(!fails.length){vt='A 合规';vc='z-pass';desc='5 个点位全部合规';}
  else if(fails.length<=2){vt='B 局部异常';vc='z-fail';desc=fails.length+' 个点位异常 (局部问题), 建议先整改异常点位后复测';}
  else {vt='C 整体异常';vc='z-fail';desc=fails.length+' 个点位异常 (涉及面广), 疑似共性供电/驱动问题';}
  let worst=null;
  if(sm&&sm.worst){const r=insRec(sm.worst);if(r)worst={s:1,r:r,k:ptKey(sm.worst)};}
  if(!worst)PTS.forEach(p=>{const r=insRec(p[2]);if(!r)return;const s=sevOf(r.v);if(!worst||s>worst.s)worst={s:s,r:r,k:p[0]};});
  const v=$('inssumv');v.textContent=vt;v.className='vbig '+(vc||'');
  $('inssums').textContent=desc+(worst&&worst.s>0?(', 最差点位: '+PTN[worst.k]+' ('+worst.k+') '+(VNAME[worst.r.v]||'')):'');
  let h='<tr><th>点位</th><th>光照</th><th>m 均值</th><th>Pst 均值</th><th>SVM 均值</th><th>判定</th></tr>';
  PTS.forEach(p=>{
    const r=insRec(p[2]);
    h+='<tr><td>'+p[0]+' '+p[1]+'</td>'+(r
      ?('<td>'+(r.va!=null?r.va:'--')+'</td><td>'+r.m.toFixed(2)+'%</td><td>'+r.p.toFixed(3)+
        '</td><td>'+(r.s!=null?r.s.toFixed(3):'--')+'</td><td class="vchip '+(VCLS[r.v]||'')+'">'+(VNAME[r.v]||'--')+'</td>')
      :'<td colspan="5" class="meta">未测</td>')+'</tr>';
  });
  $('insptbl').innerHTML=h;
  $('insadv').innerHTML=insAdvice(got,fails,sm);
}

// 整改建议 (规则表, 设备端 CLS_ADV_* 位驱动; 无 sum 时按本地条件): 指标超标 / 光照量程 / 单点 vs 多点
function insAdvice(got,fails,sm){
  const am=sm?(sm.am||0):0,use=!!sm;
  const on=(bit,local)=>use?!!(am&bit):local;
  const dark=got.filter(r=>r.v===6),bright=got.filter(r=>r.v===7);
  const cAbn=got.find(r=>r.pc===1&&r.v!==0);
  const A=[];
  if(on(0x0001,got.length>=5&&!fails.length))         // 仅当 5 点全部测完且均合规, 才给"保持现状"结论
    A.push(['w-grn','各点位均合规 (Pst^LM ≤ 1, SVM ≤ 0.4, 光照量程正常)。建议保持现状并按巡检周期定期复测。']);
  if(on(0x0002,fails.some(r=>r.p>1.0)))
    A.push(['w-red','Pst^LM 超标 (低频闪): 多来自电感镇流器荧光灯或驱动电源低频调制。建议更换为高频/无频闪 LED 驱动, 并核实是否使用可控硅调光。']);
  if(on(0x0004,fails.some(r=>r.s!=null&&r.s>0.4)))
    A.push(['w-red','SVM 超标: 与驱动电源纹波和调光方式强相关。建议核实驱动器纹波抑制能力, 避免低成本 PWM 调光。']);
  if(on(0x0008,dark.length))
    A.push(['w-org','光照偏暗 ('+dark.map(r=>PTN[ptKey(r.pc)]||r.pc).join('/')+'): 核实灯具数量/功率是否不足或灯具老化遮挡, 必要时增加光源。']);
  if(on(0x0010,bright.length))
    A.push(['w-org','光照偏亮 ('+bright.map(r=>PTN[ptKey(r.pc)]||r.pc).join('/')+'): 传感器可能过曝或直射灯光, 建议降低测点高度/避开直射后复测。']);
  if(on(0x0020,fails.some(r=>r.p<=1.0&&(r.s==null||r.s<=0.4)&&r.m>=30)))
    A.push(['w-org','部分点位波动深度较大但 Pst/SVM 合规: 属高频调制, 可见度风险低, 建议核实同批驱动器一致性。']);
  if(on(0x0040,fails.length>=3))
    A.push(['w-red','多数点位同时异常: 优先排查同一供电回路/总驱动器与线路接触, 而非逐个更换灯具。']);
  if(on(0x0080,cAbn&&fails.length<=2))
    A.push(['w-org','中心点位异常而四角正常: 多为单灯具故障或该点位驱动异常, 建议先检查中心灯具。']);
  if(on(0x0100,fails.length&&fails.length<=2&&!cAbn))
    A.push(['w-org','局部点位异常 ('+fails.map(r=>PTN[ptKey(r.pc)]||r.pc).join('/')+'): 建议针对该点位对应灯具整改后复测。']);
  if(on(0x0200,got.length<5))
    A.push(['w-org','尚有 '+(5-got.length)+' 个点位未测, 教室级结论暂不完整。']);
  return A.map(x=>'<div class="adv '+x[0]+'">'+esc(x[1])+'</div>').join('');
}

// 复测对比: 同一教室两次巡检逐点对比 (Pst/SVM/光照), 变化超过噪声阈值才判定恶化/改善
const TRD={p:0.03,s:0.015,va:25};
function insCmpRecs(id){const o={};clsList.forEach(r=>{if(r.tid===id&&r.pc)o[r.pc]=r;});return o;}
function insCmpFill(){
  const opts=insTasks.map(t=>'<option value="'+t.id+'">'+esc(t.room)+' #'+t.id+
    (t.epoch?(' '+ymd(new Date(t.epoch*1000))):'')+' ('+t.done+'/5)</option>').join('');
  ['inscA','inscB'].forEach(id=>{
    const el=$(id),cur=el.value;
    el.innerHTML='<option value="">— 选择 —</option>'+opts;
    if(cur&&[...el.options].some(o=>o.value===cur))el.value=cur;
  });
  if(!$('inscA').value&&!$('inscB').value&&insTasks.length){
    const last=insTasks[insTasks.length-1];
    $('inscB').value=last.id;
    for(let i=insTasks.length-2;i>=0;i--)
      if(insTasks[i].room===last.room){$('inscA').value=insTasks[i].id;break;}
    if(!$('inscA').value&&insTasks.length>=2)$('inscA').value=insTasks[insTasks.length-2].id;
  }
  insCompare();
}
function insCompare(){
  const A=+$('inscA').value,B=+$('inscB').value;
  const ta=insTasks.find(t=>t.id===A),tb=insTasks.find(t=>t.id===B),box=$('inscmpw');
  if(!ta||!tb){$('inscmp').textContent='选择两次巡检任务 (同一教室) 进行整改前后对比';box.innerHTML='';return;}
  if(ta.room!==tb.room){$('inscmp').textContent='两次任务教室不同 ('+ta.room+' vs '+tb.room+'), 无法对比';box.innerHTML='';return;}
  const ra=insCmpRecs(A),rb=insCmpRecs(B),keys=Object.keys(rb);
  if(!keys.length){$('inscmp').textContent='后一次巡检 ('+tb.room+' #'+tb.id+') 尚无记录';box.innerHTML='';return;}
  let rows='<tr><th>点位</th><th>Pst (前→后)</th><th>SVM (前→后)</th><th>光照 (前→后)</th><th>结论</th></tr>';
  keys.sort().forEach(k=>{
    const a=ra[k],b=rb[k],pn=PTN[ptKey(+k)]||k;
    if(!a){rows+='<tr><td>'+pn+'</td><td colspan="3" class="meta">前一次无记录</td><td class="vchip '+(VCLS[b.v]||'')+'">'+(VNAME[b.v]||'--')+'</td></tr>';return;}
    const dp=b.p-a.p,ds=(b.s||0)-(a.s||0),dva=(b.va!=null?b.va:0)-(a.va!=null?a.va:0);
    let st,sc='';
    if(a.v!==0&&b.v===0){st='整改有效 (异常 → 合规)';sc='w-grn';}
    else if(a.v===0&&b.v!==0){st='出现新异常';sc='w-red';}
    else if(dp>TRD.p||ds>TRD.s){st='指标恶化';sc='w-red';}
    else if(dp<-TRD.p||ds<-TRD.s){st='指标改善';sc='w-grn';}
    else if(dva<-TRD.va){st='光照下降 (疑似光衰/积灰)';sc='w-org';}
    else st='基本持平';
    rows+='<tr><td>'+pn+'</td><td>'+a.p.toFixed(3)+' → '+b.p.toFixed(3)+
      '</td><td>'+(a.s!=null?a.s.toFixed(3):'--')+' → '+(b.s!=null?b.s.toFixed(3):'--')+
      '</td><td>'+(a.va!=null?a.va:'--')+' → '+(b.va!=null?b.va:'--')+
      '</td><td class="vchip '+(sc||'')+'">'+st+'</td></tr>';
  });
  $('inscmp').textContent='对比: '+ta.room+' #'+ta.id+(ta.epoch?(' ('+ymd(new Date(ta.epoch*1000))+')'):'')+
                          ' → #'+tb.id+(tb.epoch?(' ('+ymd(new Date(tb.epoch*1000))+')'):'');
  box.innerHTML='<table class="tbl" style="margin-top:10px">'+rows+'</table>';
}

// ---------------- M3: 一页式巡检摘要报告 (设计文档 13 章; 纯前端生成, 浏览器打印/存 PDF) ----------------
// 结构: 基本信息 → 教室级结论 → 点位结果表 → 整改建议 → 复测对比(如有) → 附注
function insSumLocal(got){                       // 设备端 sum 缺失时的本地汇总 (与 classroomSummarize 同规则)
  const fails=got.filter(r=>r.v!==0),cAbn=got.some(r=>r.pc===1&&r.v!==0);
  const v=got.length<5?0:(!fails.length?1:(fails.length<=2&&!cAbn?2:3));
  let worst=0,ws=0;
  got.forEach(r=>{if(r.v!==0){const s=sevOf(r.v);if(s>ws){ws=s;worst=r.pc;}}});
  return {id:insViewId,pts:got.length,fail:fails.length,v:v,worst:worst,
          pf:got.filter(r=>r.p>1.0).length,sf:got.filter(r=>r.s!=null&&r.s>0.4).length,
          dark:got.filter(r=>r.v===6).length,bright:got.filter(r=>r.v===7).length};
}
function insRiskTxt(sm){
  const a=[];
  if(sm.pf)a.push('低频频闪 (Pst^LM)');
  if(sm.sf)a.push('高频可见度 (SVM)');
  if(sm.dark)a.push('光照不足');
  if(sm.bright)a.push('光照过亮');
  return a.length?a.join(' + '):'未发现';
}
function insPrevTask(t){                         // 同一教室上一次巡检 (用于报告复测对比)
  let prev=null;
  insTasks.forEach(x=>{if(x.id<t.id&&x.room===t.room&&(!prev||x.id>prev.id))prev=x;});
  return prev;
}
function insReport(){
  const t=insTasks.find(x=>x.id===insViewId);
  if(!t){alert('没有可导出的巡检任务');return;}
  const got=PTS.map(p=>insRec(p[2])).filter(Boolean);
  if(!got.length){alert('该任务还没有测量记录, 无法生成报告');return;}
  const sm=(ins.sum&&ins.sum.id===insViewId)?ins.sum:insSumLocal(got);
  const fails=got.filter(r=>r.v!==0);
  const date=t.epoch?ymd(new Date(t.epoch*1000)):(got[0].tea?ymd(new Date(got[0].tea*1000)):'日期未知');
  const dur=durName(got[0].w||29);
  const vc=sm.v===1?'pass':(sm.v===2||sm.v===3)?'fail':'na';
  const vt=sm.v===1?'A 教室合规':sm.v===2?'B 教室局部风险':sm.v===3?'C 教室整体风险':'结论待定 (点位未测全)';
  const worst=sm.worst?ptKey(sm.worst)+' '+ptName(sm.worst):'--';
  const worstV=sm.worst?insRec(sm.worst):null;
  // 复测对比 (同一教室上一次巡检, 判定规则与页面 insCompare 一致)
  const prev=insPrevTask(t),ra=prev?insCmpRecs(prev.id):null;
  let cmp='',st={fix:0,good:0,bad:0,nw:0,light:0,flat:0};
  if(prev){
    cmp='<h2>三、复测对比 (与 '+esc(prev.room)+' #'+prev.id+(prev.epoch?' '+ymd(new Date(prev.epoch*1000)):'')+' 巡检)</h2>'+
        '<table><tr><th>点位</th><th>Pst^LM (前→后)</th><th>SVM (前→后)</th><th>光照 (前→后)</th><th>结论</th></tr>';
    Object.keys(insCmpRecs(t.id)).sort().forEach(k=>{
      const a=ra[+k],b=insRec(+k),pn=ptKey(+k)+' '+ptName(+k);
      if(!a){cmp+='<tr><td>'+pn+'</td><td colspan="3">前次未测</td><td class="c '+(VCLS[b.v]||'')+'">'+(VNAME[b.v]||'--')+'</td></tr>';return;}
      const dp=b.p-a.p,ds=(b.s||0)-(a.s||0),dva=(b.va!=null?b.va:0)-(a.va!=null?a.va:0);
      let txt,cls='';
      if(a.v!==0&&b.v===0){txt='整改有效 (异常→合规)';cls='grn';st.fix++;}
      else if(a.v===0&&b.v!==0){txt='出现新异常';cls='red';st.nw++;}
      else if(dp>TRD.p||ds>TRD.s){txt='指标恶化';cls='red';st.bad++;}
      else if(dp<-TRD.p||ds<-TRD.s){txt='指标改善';cls='grn';st.good++;}
      else if(dva<-TRD.va){txt='光照下降 (疑似光衰/积灰)';cls='org';st.light++;}
      else{txt='基本持平';st.flat++;}
      cmp+='<tr><td>'+pn+'</td><td>'+a.p.toFixed(3)+' → '+b.p.toFixed(3)+
        '</td><td>'+(a.s!=null?a.s.toFixed(3):'--')+' → '+(b.s!=null?b.s.toFixed(3):'--')+
        '</td><td>'+(a.va!=null?a.va:'--')+' → '+(b.va!=null?b.va:'--')+
        '</td><td class="c '+cls+'">'+txt+'</td></tr>';
    });
    cmp+='</table>';
    const s2=[];
    if(st.fix)s2.push('整改有效 '+st.fix+' 点');
    if(st.good)s2.push('改善 '+st.good+' 点');
    if(st.nw||st.bad)s2.push('恶化/新异常 '+(st.nw+st.bad)+' 点');
    if(st.light)s2.push('光照下降 '+st.light+' 点');
    if(st.flat)s2.push('持平 '+st.flat+' 点');
    if(s2.length)cmp+='<div class="note">对比摘要: '+s2.join(' · ')+'。</div>';
  }
  // 点位结果表 (13.2: 点位 | Pst^LM | SVM | 波动深度 | 光照 | 判定)
  let rows='';
  PTS.forEach(p=>{
    const r=insRec(p[2]);
    rows+=r
      ?('<tr><td>'+p[0]+' '+p[1]+'</td><td>'+r.p.toFixed(3)+'</td><td>'+(r.s!=null?r.s.toFixed(3):'--')+
        '</td><td>'+r.m.toFixed(2)+'%</td><td>'+(r.va!=null?r.va:'--')+
        '</td><td class="c '+(VCLS[r.v]||'')+'">'+(VNAME[r.v]||'--')+'</td></tr>')
      :('<tr class="dim"><td>'+p[0]+' '+p[1]+'</td><td colspan="5">未测</td></tr>');
  });
  const adv=insAdvice(got,fails,sm);
  const h='<!doctype html><html lang="zh"><head><meta charset="utf-8"><title>教室巡检摘要 '+esc(t.room)+' '+date+'</title><style>'+
    'body{font-family:"Microsoft YaHei",system-ui,sans-serif;background:#e8eaed;color:#1a1a1a;margin:0;padding:18px;font-size:13px}'+
    '.pg{max-width:820px;margin:0 auto;background:#fff;padding:26px 32px;border-radius:6px;box-shadow:0 1px 6px rgba(0,0,0,.15)}'+
    'h1{font-size:20px;margin:0 0 2px}h2{font-size:14px;margin:16px 0 6px;border-bottom:1px solid #ddd;padding-bottom:3px}'+
    '.sub{color:#777;font-size:11px;margin-bottom:12px}'+
    'table{border-collapse:collapse;width:100%}th,td{border:1px solid #ccc;padding:4px 7px;text-align:center;font-size:12px}'+
    'th{background:#f2f3f5}td.c{font-weight:600}td.grn{color:#137333}td.red{color:#c5221f}td.org{color:#b06000}tr.dim td{color:#999}'+
    '.info td{text-align:left;border:none;padding:2px 4px;font-size:12.5px}'+
    '.verdict{margin:14px 0 4px;padding:10px 14px;border-radius:5px;font-size:17px;font-weight:700}'+
    '.verdict.pass{background:#e6f6ea;color:#137333;border:1px solid #9adba9}'+
    '.verdict.fail{background:#fde8e8;color:#c5221f;border:1px solid #f2a6a6}'+
    '.verdict.na{background:#f1f3f4;color:#555;border:1px solid #ccc}'+
    '.kv{color:#444;font-size:12px;margin:2px 0 6px}'+
    '.adv{border:1px solid #e0e0e0;border-left:4px solid #888;border-radius:4px;padding:6px 10px;margin:5px 0;background:#fafafa}'+
    '.adv.w-red{border-left-color:#d93025;background:#fdf0ef}.adv.w-org{border-left-color:#e8710a;background:#fdf5ee}.adv.w-grn{border-left-color:#188038;background:#eef7f0}'+
    '.note{color:#555;font-size:11px;margin:8px 0;line-height:1.6}.ft{margin-top:14px;color:#999;font-size:10.5px;border-top:1px solid #eee;padding-top:6px}'+
    '.nop{position:fixed;right:20px;top:16px;padding:8px 16px;background:#1a73e8;color:#fff;border:none;border-radius:5px;font-size:13px;cursor:pointer}'+
    '@page{size:A4;margin:12mm}@media print{.nop{display:none}body{background:#fff;padding:0}.pg{box-shadow:none;max-width:none;padding:0}}'+
    '</style></head><body><button class="nop" onclick="window.print()">打印 / 存为 PDF</button><div class="pg">'+
    '<h1>教室照明频闪巡检摘要</h1><div class="sub">LightSense 教室照明频闪检测仪 · 判定依据: Pst^LM ≤ 1 (IEC TR 61547-1) · SVM ≤ 0.4 (IEC TR 63158)</div>'+
    '<table class="info"><tr><td><b>教室:</b> '+esc(t.room)+'</td><td><b>巡检日期:</b> '+date+'</td><td><b>巡检人:</b> '+esc(t.ins||'--')+'</td></tr>'+
    '<tr><td><b>巡检类型:</b> '+esc(t.scene||'--')+'</td><td><b>任务号:</b> #'+t.id+'</td><td><b>单点测量时长:</b> '+dur+' × 5 点</td></tr></table>'+
    '<div class="verdict '+vc+'">'+vt+'</div>'+
    '<div class="kv">异常点位: '+sm.fail+'/'+sm.pts+' · 最差点位: '+worst+(worstV?(' ('+(VNAME[worstV.v]||'--')+')'):'')+' · 风险类型: '+insRiskTxt(sm)+'</div>'+
    '<h2>一、点位结果</h2><table><tr><th>点位</th><th>Pst^LM (≤1.0)</th><th>SVM (≤0.4)</th><th>波动深度 m</th><th>光照 (ADC)</th><th>判定</th></tr>'+rows+'</table>'+
    '<h2>二、整改建议</h2>'+(adv||'<div class="note">无</div>')+
    cmp+
    '<div class="note">附注: ① 判定口径: 光照量程正常 (ADC 400~3000) 且 Pst^LM ≤ 1 且 SVM ≤ 0.4 判合规; ② 教室级结论由设备端 5 点汇总自动生成 (A = 全部合规, B = 1~2 点异常且中心正常, C = ≥3 点异常或中心异常); ③ 本摘要为现场快速普查结果, 正式验收应依据 GB/T 5700 等标准测量方法。</div>'+
    '<div class="ft">生成时间: '+new Date().toLocaleString('zh-CN')+' · LightSense v1.6 教室巡检 · 任务 #'+t.id+'</div>'+
    '</div></body></html>';
  const w=window.open('','_blank');
  if(w){w.document.open();w.document.write(h);w.document.close();}
  else{                                               // 弹窗被拦截 → 下载 HTML 文件
    const a=document.createElement('a');
    a.href=URL.createObjectURL(new Blob([h],{type:'text/html;charset=utf-8'}));
    a.download='巡检摘要_'+t.room+'_'+date+'.html';
    a.click();URL.revokeObjectURL(a.href);
  }
}

async function insCreate(){
  const room=$('insroom').value.trim();
  if(!room){alert('请先填写教室号, 例如 A302');return;}
  try{
    await insApi('/api/inspect/start?room='+encodeURIComponent(room)+
                 '&scene='+encodeURIComponent($('insscene').value)+
                 '&ins='+encodeURIComponent($('insins').value.trim())+
                 '&t='+Math.floor(Date.now()/1000),{method:'POST'});
    await insRefresh();
  }catch(e){alert('新建巡检失败: '+e);}
}
async function insMeasure(){
  if(!ins.active||!ins.next){alert('当前没有待测点位');return;}
  const dur=$('insdur').value,wn={30:29,60:58,600:586}[dur]||29;
  $('insmeas').disabled=true;
  try{
    await insApi('/api/classroom/start?pt='+ins.next+'&dur='+dur+'&t='+Math.floor(Date.now()/1000),{method:'POST'});
    $('clsstat').textContent='测量中 (巡检点位 '+ptKey(ins.next)+' '+ptName(ins.next)+') 0 / '+wn+' 窗口...';
  }catch(e){alert(startErr(e));await insRefresh();}
}
async function insFinish(){
  if(!ins.active)return;
  if(!confirm('结束当前巡检 (教室 '+ins.room+')?\n未测的点位不会计入教室级汇总。'))return;
  try{await insApi('/api/inspect/finish',{method:'POST'});await insRefresh();}catch(e){alert('结束失败: '+e);}
}
async function insAbort(){
  if(!ins.active)return;
  if(!confirm('中止当前巡检 (教室 '+ins.room+')?\n已测点位的记录保留, 任务标记为已中止。'))return;
  try{await insApi('/api/inspect/abort',{method:'POST'});await insRefresh();}catch(e){alert('中止失败: '+e);}
}
async function insRefresh(){
  try{
    ins=await insApi('/api/inspect');
    insTasks=ins.tasks||[];
  }catch(e){ins={active:false,id:0,mask:0,done:0,next:0};insTasks=[];}
  // 汇总视图任务号: 进行中 → 当前任务; 已结束 → 最近一次任务 (页面刷新后仍能看到上次巡检汇总)
  if(ins.active)insViewId=ins.id;
  else if(!insTasks.some(t=>t.id===insViewId))
    insViewId=insTasks.length?insTasks[insTasks.length-1].id:0;
  paintIns();clsSum();insCmpFill();
}

async function clsRefresh(){
  try{
    const r=await fetch('/api/classroom');
    const j=await r.json();
    clsList=j.records||[];
    paintClsTable();
    $('clsbtn').disabled=!!j.run;
    clsRun=!!j.run;                           // 测量状态供巡检按钮禁用 (测点测量会占用设备)
    if(!j.run)$('clspbar').style.width=(j.nwin>=(j.tot||29)?'100%':'0%');
    trRefresh();
    await insRefresh();                       // 必须 await: 汇总/建议依赖 clsList + ins.id, 否则本帧可能用旧数据
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
  }catch(e){alert(startErr(e));}
}
async function clsClear(){
  if(!confirm('确定清空全部测量记录?'))return;
  try{await fetch('/api/classroom/clear',{method:'POST'});await clsRefresh();}catch(e){alert('清空失败: '+e);}
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
  clsRun=!!c.run;                            // 实时测量状态 → 巡检按钮禁用同步
  const pct=c.res?100:Math.round(100*c.win/(c.tot||29));
  $('clspbar').style.width=pct+'%';
  if(c.run){
    $('clsbtn').disabled=true;
    $('clsstat').textContent='测量中 '+c.win+' / '+c.tot+' 窗口 (~'+durName(c.tot)+') · 实时 m '+c.m.toFixed(2)+'% · '+c.freq.toFixed(1)+' Hz';
  }else{
    $('clsbtn').disabled=false;
    if(c.res){
      clsRefresh();                           // 刷新记录 + 巡检汇总 (不再有独立"最近结果"卡片)
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
  // 巡检状态实时同步 (设备端任务掩码/待测点位随测量完成变化; 无任务时不下发, 由 /api/inspect 兜底)
  if(d.ins){
    ins.active=true;ins.id=d.ins.id;ins.room=d.ins.room;ins.mask=d.ins.mask;ins.done=d.ins.done;ins.next=d.ins.next;
    if(d.sm)ins.sum=d.sm;                     // 设备端教室级汇总 (M2): 点位完成即出结论
    insViewId=d.ins.id;
    paintIns();clsSum();                      // 同步点位芯片 + 汇总 (记录在随后 clsRefresh 帧补齐)
  }
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

// 教室级汇总 JSON (M2: 设备端判定 v + 建议位掩码 am; /api/inspect 与 WS ins 帧共用)
static int jsonSummary(char *dst, size_t cap, const ClassroomSummary &sm)
{
    return snprintf(dst, cap,
                    "{\"id\":%u,\"pts\":%u,\"pass\":%u,\"fail\":%u,\"dark\":%u,\"bright\":%u,"
                    "\"pf\":%u,\"sf\":%u,\"worst\":%u,\"v\":%u,\"am\":%u}",
                    (unsigned)sm.taskId, (unsigned)sm.points, (unsigned)sm.passCnt,
                    (unsigned)sm.failCnt, (unsigned)sm.darkCnt, (unsigned)sm.brightCnt,
                    (unsigned)sm.pstFailCnt, (unsigned)sm.svmFailCnt,
                    (unsigned)sm.worstCode, (unsigned)sm.verdict, (unsigned)sm.advice);
}

// ---------------- 云端配网页 (/setup, 独立小页, 密钥只存设备 NVS) ----------------
static const char SETUP_HTML[] PROGMEM = R"rawliteral(
<!doctype html><html lang=zh><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>LightSense WiFi 配置</title>
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
<h2>WiFi 联网配置</h2>
<p>参数存设备本地 NVS, 不上传; 用于教室现场 STA 联网与 NTP 校时 (OneNET 暂停使用)</p>
<label>WiFi 名称 (SSID)</label>
<select id=ssidsel><option value="">— 点击下方按钮扫描 WiFi —</option></select>
<div class=sc><button id=scant type=button>扫描 WiFi</button><div class=msg id=scanm style="margin:0"></div></div>
<input id=ssid maxlength=32 placeholder="也可直接手动输入 WiFi 名称">
<label>WiFi 密码 (可空 = 开放网络)</label><input id=pass maxlength=64>
<!-- OneNET 暂停使用: 输入框隐藏, 保留元素以兼容 JS 回显/提交; 恢复时去掉外层 display:none -->
<div style="display:none">
<label>产品 ID (product_id)</label><input id=pid maxlength=24>
<label>设备名称 (device_name)</label><input id=dev maxlength=24>
<label>设备 Token (MQTT 密码, 整串)</label><input id=tok maxlength=255>
</div>
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
        uint16_t nR = 0;
        const ClassroomRecord *rs = classroomRecords(&nR);
        // 响应缓冲按需堆分配 (v1.6.1): 原先 24KB 常驻 BSS 持续挤压堆, 使 WebSocket 广播 new[] 在堆
        // 紧张时抛 std::bad_alloc → std::terminate → abort → 重启 (实测崩溃点); 改按需 malloc + 用后
        // free, 常驻占用归零 (记录 ≤80 条 × ≤288B + 尾部 640B 余量)
        size_t cap = 640 + (size_t)nR * 288;
        char *cb = (char *)malloc(cap);
        if (!cb)
        {
            req->send(503, "application/json", "{\"err\":\"low mem\"}");
            return;
        }
        int n = snprintf(cb, cap,
                         "{\"run\":%u,\"nwin\":%lu,\"tot\":%u,\"n\":%u,\"records\":[",
                         (unsigned)classroomIsRunning(),
                         (unsigned long)classroomWindowsDone(),
                         (unsigned)classroomWindowsTarget(), (unsigned)nR);
        for (uint16_t i = 0; i < nR && n < (int)cap - 640; i++)
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
            if (rs[i].taskId)            // 巡检测量 → 输出归属任务号 + 点位码 (前端按任务聚合成教室结论)
                snprintf(sv + strlen(sv), sizeof(sv) - strlen(sv), ",\"tid\":%u,\"pc\":%u",
                         (unsigned)rs[i].taskId, (unsigned)rs[i].pointCode);
            n += snprintf(cb + n, cap - n,
                          "%s{\"i\":%u,\"lb\":\"%s\",\"ts\":%lu,\"w\":%u,\"m\":%.2f,\"mx\":%.2f,"
                          "\"fi\":%.3f,\"p\":%.3f,\"px\":%.3f,\"f\":%.2f,\"v\":%u%s}",
                          (i ? "," : ""), (unsigned)i, lb,
                          (unsigned long)rs[i].ts, (unsigned)rs[i].windows,
                          rs[i].mMean, rs[i].mMax,
                          rs[i].fi, rs[i].pstMean, rs[i].pstMax, rs[i].freq,
                          (unsigned)rs[i].verdict, sv);
        }
        n += snprintf(cb + n, cap - n, "],\"latest\":");
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
            if (L->taskId)
                snprintf(sv + strlen(sv), sizeof(sv) - strlen(sv), ",\"tid\":%u,\"pc\":%u",
                         (unsigned)L->taskId, (unsigned)L->pointCode);
            n += snprintf(cb + n, cap - n,
                          "{\"lb\":\"%s\",\"m\":%.2f,\"mmax\":%.2f,\"fi\":%.3f,"
                          "\"pst\":%.3f,\"pstmax\":%.3f,\"freq\":%.2f,\"v\":%u,\"ts\":%lu,\"w\":%u%s}",
                          lb, L->mMean, L->mMax, L->fi, L->pstMean, L->pstMax,
                          L->freq, (unsigned)L->verdict, (unsigned long)L->ts,
                          (unsigned)L->windows, sv);
        }
        else
        {
            n += snprintf(cb + n, cap - n, "null");
        }
        snprintf(cb + n, cap - n, "}");
        req->send(200, "application/json", cb);   // send() 同步拷贝内容到响应对象, 拷贝后即可释放
        free(cb);
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
        // pt = 巡检测点 (1..5): 标签由设备按当前巡检任务生成 <教室号>-<点位码>, 完成时自动推进
        bool ok;
        if (req->hasParam("pt"))
        {
            long pc = req->getParam("pt")->value().toInt();
            ok = (pc >= CLS_PT_C && pc <= CLS_PT_RB)
                 && classroomInspectStartPoint((uint8_t)pc, wins, epoch);
        }
        else
        {
            ok = classroomStart(label.c_str(), wins, epoch);
        }
        if (!ok)
        {
            req->send(409, "application/json", "{\"err\":\"busy, records full or no active task\"}");
            return;
        }
        req->send(200, "application/json", "{\"ok\":1}");
    });

    // 教室巡检: 当前任务 + 任务列表 + 教室级汇总 (记录经 /api/classroom 的 tid/pc 归属到任务)
    server.on("/api/inspect", HTTP_GET, [](AsyncWebServerRequest *req)
    {
        static char ib[4 * 1024];      // 仅 async_tcp 任务调用, 单线程安全
        const InspectionTask *t = classroomInspectTask();
        uint16_t nt = 0;
        const InspectionTask *ts = classroomTasks(&nt);
        int n = snprintf(ib, sizeof(ib), "{\"active\":%u",
                         (unsigned)classroomInspectActive());
        if (t)
        {
            char rb[80], sb[56], nb[56];
            jsonEscape(rb, sizeof(rb), t->room);
            jsonEscape(sb, sizeof(sb), t->scene);
            jsonEscape(nb, sizeof(nb), t->inspector);
            n += snprintf(ib + n, sizeof(ib) - n,
                          ",\"id\":%u,\"room\":\"%s\",\"scene\":\"%s\",\"ins\":\"%s\","
                          "\"epoch\":%lu,\"status\":%u,\"mask\":%u,\"done\":%u,\"next\":%u",
                          (unsigned)t->id, rb, sb, nb, (unsigned long)t->epoch,
                          (unsigned)t->status, (unsigned)t->mask,
                          (unsigned)t->pointsCount, (unsigned)classroomInspectNextPoint());
        }
        // 教室级汇总 (M2): 进行中任务优先, 否则最近一次任务; 设备端判定 + 建议位掩码
        uint16_t smId = t ? t->id : (nt ? ts[nt - 1].id : 0);
        ClassroomSummary sm;
        char sumBuf[192] = "null";
        if (smId)
        {
            classroomSummarize(smId, &sm);
            jsonSummary(sumBuf, sizeof(sumBuf), sm);
        }
        n += snprintf(ib + n, sizeof(ib) - n, ",\"sum\":%s,\"tasks\":[", sumBuf);
        for (uint16_t i = 0; i < nt && n < (int)sizeof(ib) - 160; i++)
        {
            char rb[80], sb[56], nb[56];
            jsonEscape(rb, sizeof(rb), ts[i].room);
            jsonEscape(sb, sizeof(sb), ts[i].scene);
            jsonEscape(nb, sizeof(nb), ts[i].inspector);
            n += snprintf(ib + n, sizeof(ib) - n,
                          "%s{\"id\":%u,\"room\":\"%s\",\"scene\":\"%s\",\"ins\":\"%s\",\"status\":%u,"
                          "\"mask\":%u,\"done\":%u,\"epoch\":%lu}",
                          (i ? "," : ""), (unsigned)ts[i].id, rb, sb, nb,
                          (unsigned)ts[i].status, (unsigned)ts[i].mask,
                          (unsigned)ts[i].pointsCount, (unsigned long)ts[i].epoch);
        }
        n += snprintf(ib + n, sizeof(ib) - n, "]}");
        req->send(200, "application/json", ib);
    });

    // 教室巡检: 新建任务 (?room=教室号&scene=场景&ins=巡检人&t=Unix秒)
    server.on("/api/inspect/start", HTTP_POST, [](AsyncWebServerRequest *req)
    {
        String room, scene, ins;
        if (req->hasParam("room"))  room  = req->getParam("room")->value();
        if (req->hasParam("scene")) scene = req->getParam("scene")->value();
        if (req->hasParam("ins"))   ins   = req->getParam("ins")->value();
        utf8Trunc(room, 23);
        utf8Trunc(scene, 15);
        utf8Trunc(ins, 15);
        if (!room.length())
        {
            req->send(400, "application/json", "{\"err\":\"room required\"}");
            return;
        }
        uint32_t epoch = 0;
        if (req->hasParam("t"))
        {
            long tv = req->getParam("t")->value().toInt();
            if (tv > 1600000000L) epoch = (uint32_t)tv;
        }
        if (!classroomInspectCreate(room.c_str(), scene.c_str(), ins.c_str(), epoch))
        {
            req->send(409, "application/json", "{\"err\":\"busy or task full\"}");
            return;
        }
        req->send(200, "application/json", "{\"ok\":1}");
    });

    // 教室巡检: 中止 / 结束当前任务
    server.on("/api/inspect/abort", HTTP_POST, [](AsyncWebServerRequest *req)
    {
        if (!classroomInspectAbort())
        {
            req->send(409, "application/json", "{\"err\":\"no active task or measuring\"}");
            return;
        }
        req->send(200, "application/json", "{\"ok\":1}");
    });
    server.on("/api/inspect/finish", HTTP_POST, [](AsyncWebServerRequest *req)
    {
        if (!classroomInspectFinish())
        {
            req->send(409, "application/json", "{\"err\":\"no active task or measuring\"}");
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
static char jsonBuf[8 * 1024];   // WS 单帧 ≈5KB (256 波形 + 256 频谱 + 元数据); 常驻 BSS 吃紧, 不再冗余

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

    // 巡检任务 (v1.6): 当前任务号/教室/已完成点位掩码/待测点位 → 网页巡检区实时更新
    const InspectionTask *it = classroomInspectTask();
    if (it)
    {
        char rb[64];
        jsonEscape(rb, sizeof(rb), it->room);
        n += snprintf(jsonBuf + n, sizeof(jsonBuf) - n,
                      ",\"ins\":{\"id\":%u,\"room\":\"%s\",\"mask\":%u,\"done\":%u,\"next\":%u}",
                      (unsigned)it->id, rb, (unsigned)it->mask,
                      (unsigned)it->pointsCount, (unsigned)classroomInspectNextPoint());

        // 教室级汇总 (M2): 点位完成瞬间即可更新结论/建议, 不必等 /api/inspect 刷新
        ClassroomSummary sm;
        classroomSummarize(it->id, &sm);
        if (sm.points > 0)
        {
            char sb[192];
            jsonSummary(sb, sizeof(sb), sm);
            n += snprintf(jsonBuf + n, sizeof(jsonBuf) - n, ",\"sm\":%s", sb);
        }
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
    uint16_t nCli = ws.count();
    if (nCli == 0)
        return;                       // 无客户端时不构建 JSON, 节省 CPU

    size_t len = buildJson(snap);
    if (len == 0)
        return;

    // 堆保护 (v1.6.1): AsyncWebSocket::textAll 为每个客户端 new[] 一份副本 (len+1), 堆不足时
    // operator new 抛 std::bad_alloc → std::terminate → abort → 整机重启 (实测崩溃点)。
    // 先估算所需堆, 不足则跳过本次广播 (下一窗口自动重试), 避免重启。
    uint32_t need = (uint32_t)(len + 512) * (uint32_t)nCli + 2048;
    if ((uint32_t)ESP.getFreeHeap() < need)
    {
        static uint32_t lastWarn = 0;
        if (millis() - lastWarn > 5000)
        {
            lastWarn = millis();
            Serial.printf("WARN: WS broadcast skipped, heap %d < need %u (min %d)\n",
                          (int)ESP.getFreeHeap(), (unsigned)need, (int)ESP.getMinFreeHeap());
        }
        return;
    }
    ws.textAll(jsonBuf, len);
}
