# EDV11P 网页版轨迹工具 — 自测报告（track.html）

| 项 | 内容 |
|---|---|
| 被测文件 | `release/track.html` |
| 设计说明 | `docs/project/edv11p/web-track-tool-design.md` |
| 测试时间 | 2026-10-18 |
| 测试环境 | Windows + Node.js v20.19.5（无需安装任何 npm 包） |
| 硬件 | **无**（本次不涉及真实串口） |
| 总判定 | **通过**：帧解析+只读计划用例 6/6；静态 + 桩环境检查 29/29；**布局契约 26/26**；其中 Gate-1 P1-06 关键断言「未点击按钮时发出帧的 `op=0xA1` 条数 = 0」通过 |
| 未覆盖 | **无头浏览器截图（本环境被沙箱阻断，见第 7.3 节）**、真实硬件联调（见第 5 节） |
| 版本说明 | 本报告为 **应用壳布局改造 + P1-06 整改后**版本（满视口两列、默认只读 + 显式确认 + 逐条等应答） |

---

## 1. 结论摘要

| 验收要求 | 结果 | 证据 |
|---|---|---|
| 单文件、无 CDN/外链、离线可用 | ✅ 通过 | 静态扫描：`src`/`href` 引用数 = **0** |
| 中文界面、中文注释 | ✅ | 全页中文；脚本段落含中文注释与配置说明 |
| Web Serial 连接（921600）+ 状态/帧率/坏帧 | ✅ 代码就绪，⚠ 未上板 | 见第 5 节；无硬件无法验证真实吞吐 |
| 帧解析：0x45 0x44 同步 / 长度 / 校验 / 坏帧计数 / 容忍杂散字节 | ✅ 通过 | 用例 C1–C5，5/5 |
| **默认只读：连接后只发 GET，一条 SET 都不发** | ✅ 通过 | 用例 C6 + 第 3 节第 22–23 项（假串口实测 16 条帧全为 `op=0xA0`） |
| **写入需显式确认：固定警示文案 + `confirm()` 二次确认** | ✅ 通过 | 第 3 节第 24–25 项（取消时 0 帧） |
| **逐条 SET 逐条等应答；500ms 超时即中止并列出失败项** | ✅ 通过 | 第 3 节第 26–28 项 |
| **ACK 错误码按 `ACK_MESSAGES` 显示中文** | ✅ 通过 | 第 3 节第 29 项（0x05 → 参数错误） |
| 0x70 → 目标列表 → 侧装坐标口径（**不旋转**）→ 可调几何输入 | ✅ 通过 | 用例 C4；`projectionBounds` / `heightAboveGround` 断言；界面 4 个参数输入 |
| Canvas 俯视轨迹：网格、每目标一色、拖尾、消失淡出、人数/累计 | ✅ 通过 | 桩环境断言：`#1..#4` 标签各 3 行、`lineTo`=5776、HUD「当前目标 4」、淡出后队列清空 |
| 顶部状态条 7 项 | ✅ 通过 | `st-count`=4、`st-fps`>0 等断言 |
| 模拟演示（无硬件可验收） | ✅ 通过 | 推进 4.5s：当前 4 人 / 累计 4 人 / 轨迹点数 140+ |
| 原始帧日志（hex + 解析结果） | ✅ 通过 | 日志累积 > 20 行，收发/模拟方向标记正常 |
| 所有魔法数字集中在配置区/输入框 | ✅ | `CFG`（协议/档位/渲染/时序/写入/模拟）+ 界面输入 |
| 不使用 localStorage 存敏感数据 | ✅ 通过 | 静态扫描无 localStorage / sessionStorage / indexedDB / cookie |
| **满视口应用壳：顶栏吸顶、左列画布+日志、右列独立滚动、整页不滚动** | ✅ 契约 26/26；⚠ 截图未取到 | 第 7 节（含 6 项 DOM 结构 + 15 项 CSS 契约 + 5 项 JS 契约） |
| **画布高度跟随容器（不再固定像素 / innerHeight 比例）** | ✅ 通过 | `fitCanvas()` 读 `#plotWrap.getBoundingClientRect()`；ResizeObserver 观察 plotWrap 与 logPanel |

---

## 2. 帧解析 + 只读计划自测用例（6 条，全部通过）

用例内容与 `web-track-tool-design.md` 第 6 节完全一致；页面内也内置同一份数据
（按钮「运行帧解析自测」）。C1–C5 验帧层，**C6 验「默认只读」**。

| 用例 | 场景 | 期望 | 实测 | 判定 |
|---|---|---|---|---|
| C1 | `GET 0x02`（11 字节空载荷） | 1 帧，`msgId=1 op=A0 type=02 len=0`，坏帧 0 | 一致 | ✅ |
| C2 | `SET 0x05=1`（12 字节） | 1 帧，载荷 `01`，坏帧 0 | 一致 | ✅ |
| C3 | `SET 0x22=200`（13 字节） | 1 帧，载荷 `00 C8`，坏帧 0 | 一致 | ✅ |
| C4 | `STATUS 0x70`：2 区 / 5 目标 / 含负数 | 区1 `(2,-120,111,149)(3,-28,38,203)(4,36,-184,199)`；区2 `(5,-16,94,176)(6,-139,-182,211)` | 一致 | ✅ |
| C5 | 噪声3B + 好帧 + 坏校验和帧 + 好帧 | 2 帧、坏帧 1、丢弃 14B | 一致 | ✅ |
| C6 | 只读计划（P1-06） | 16 条，全部 `op=0xA0`，`op=0xA1` 条数 **0**，含 0x02/05/10/20/21/22 | 一致 | ✅ |

页面内自测输出（原文）：

```
自测结果：6/6 通过
  [通过] C1 GET 模块信息 0x02（空载荷）
  [通过] C2 SET 工作模式=1（0x05，1 字节）
  [通过] C3 SET 挂高=200cm（0x22，uint16 大端 0x00C8）
  [通过] C4 STATUS 0x70 目标上报：2 个感应区 / 5 个目标（含负数坐标）
  [通过] C5 重同步：噪声 3 字节 + 好帧 + 坏校验和帧 + 好帧
  [通过] C6 只读计划：16 条查询，全部 op=0xA0（无任何 SET）
```

> 注：C2/C3 仍然保留——它们验证的是**帧构造**能力（用户确认后确实能造出合法的 SET 帧），
> 不代表连接流程会发 SET。是否发出由 C6 与第 3 节的运行期断言把关。

### 2.1 C5 的 `noiseBytes` 为什么是 14 而不是 3

解析器遇到坏校验和时按 .py 的策略「只滑过 1 字节再找帧头」，因此除了 3 字节前导噪声，
坏帧剩余的 11 字节也会被逐字节跳过并计入丢弃量：`3 + 11 = 14`。
这是有意保留的语义（与 .py 第 268-271 行一致），用例断言按 14 写。

---

## 3. 静态与桩环境检查（29 项，全部通过）

### 3.1 静态与纯逻辑（1–21）

| # | 检查项 | 结果 |
|---|---|---|
| 1 | 整段内联脚本可编译（`new Function`） | ✅ |
| 2 | 只有一个内联 `<script>`，无外部脚本 | ✅ |
| 3 | 无外部 `src`/`href` 引用（离线可用） | ✅ 0 个 |
| 4 | 无 localStorage / sessionStorage / indexedDB / cookie 使用 | ✅ |
| 5 | CFG + CORE 区可独立抽取执行（不依赖 DOM） | ✅ |
| 6 | `projectionBounds(800, 侧装)` = x[-500,500] y[0,800] | ✅ |
| 7 | `projectionBounds(350, 朝下)` = ±350 | ✅ |
| 8 | `tickValues(0, 800)` = 0,100,…,800（9 条） | ✅ |
| 9 | `tickValues(-500, 500)` = -400,-200,0,200,400（5 条，与 .py 同算法） | ✅ |
| 10 | 离地高度：侧装 `z` 直用 / 朝下 `挂高 − z` | ✅ 149→149 / 149→151 |
| 11 | 颜色 = `palette[id % 24]`，调色板 24 色 | ✅ |
| 12 | 模拟载荷往返：`buildTargetPayload` → `createFrame` → `feed` → `parseTargetReport` 目标表一致 | ✅ |
| 13 | 逐字节喂入（模拟串口分包）仍解析出 1 帧、坏帧 0 | ✅ |
| 14 | 整页在 DOM/Canvas 桩环境下执行 `boot()` 无异常 | ✅ |
| 15 | 模拟 4.5 s：当前 4 人、累计 4 人、轨迹点数 140–200（受 50 点上限约束） | ✅ |
| 16 | 定时目标 id4 到点消失 → 进入淡出队列 → 1.5 s 后队列清空、累计不回退 | ✅ |
| 17 | 画布输出：`#1..#4` 标签各 3 行、两轴刻度、标题、HUD；`lineTo`=5776、`arc`=452 | ✅ |
| 18 | 交互：写入 C4 样例帧、运行自测、切换侧装/朝下、改显示范围、清空轨迹、停止模拟 | ✅ |
| 19 | `SET 0x10=2` 帧与**协议文档第 168 行示例逐字节一致**（`45 44 00 0D 00 01 A1 10 00 02 00 02 4C`，校验和 0x4C） | ✅ |
| 20 | 上报频率默认值 = **2**（`CFG.defaults.reportFreq` 覆盖界面初值），提示显示「当前 = 100ms」 | ✅ |
| 21 | 快捷周期按钮 100/200/500 ms → 写入 2/4/10，提示同步换算 | ✅ |

### 3.2 Gate-1 P1-06 专项：默认只读 / 显式确认 / 逐条等应答（22–29）

桩环境里用「假串口 + `confirm()` 桩 + 可推进的假时钟」驱动整条链路（脚本见 §4.3）。

| # | 检查项 | 结果 |
|---|---|---|
| 22 | **连接后（未点任何按钮）发出的 16 条帧全部 `op=0xA0`；其中 `op=0xA1` 条数 = 0**；串口实际写入的字节里也无 0xA1 | ✅ **0 条 SET** |
| 23 | 运行期兜底计数 `state.unexpectedSets` = 0（非确认流程发 SET 会计数） | ✅ |
| 24 | 确认框文案含「将改写雷达模组当前配置（工作模式 / 挂高 / 上报频率）」 | ✅ |
| 25 | 确认框点「取消」→ 一帧都不发（`sentFrames` 无新增） | ✅ |
| 26 | 确认后按 0x05 → 0x22 → 0x10 顺序发出 **3 条 SET**，载荷分别 `02` / `00 FA`(250) / `00 04` | ✅ |
| 27 | 每条 SET 都拿到应答（时序日志出现 3 条「0xA2 回报」），`pending` 清空，设备回读变为 2 / 250 / 4 | ✅ |
| 28 | 设备不应答（真串口无回包）：**只发第 1 条 SET 即中止**，失败项含「超时」，`pending` 清空，按钮恢复可点 | ✅ |
| 29 | 灌入 `0xA9 ACK=0x05`：显示中文「参数错误」、中止、后续项不发送；同时验证按 type 兜底匹配（msgid 不匹配也能结算） | ✅ |
| 附 | 模拟模式可离线演示完整时序：3 条 SET 逐条应答，模拟设备内部值被改写，流程耗时 ≥ 3×应答时延（证明在等应答而非盲发）；离线时两个写入按钮自动禁用 | ✅ |

桩环境绘制调用统计（一次 4.5 s 会话、46 帧重绘）：

```
setTransform 46  clearRect 46  fillRect 46  setLineDash 452
beginPath 2494  moveTo 2042  lineTo 5776  stroke 2448
arc 452  fill 452  arcTo 904  closePath 226  fillText 1460
```

> 说明：这是逻辑级验证——确认每个该画的元素都被请求绘制，且坐标换算/淡出/计数符合预期；
> 它**不能**替代真实浏览器里的像素级目检（见第 5 节）。

---

## 4. 复现命令

PowerShell 下直接可用（Node 路径按本机实际情况替换）。脚本内一律用单引号、
正则里的双引号用 `\x22` 转义，避免 PowerShell 传参被改写。

### 4.1 帧解析用例（抽取 `CORE-BEGIN/END` 区独立执行）

```powershell
$node = "C:\Users\fuhua\AppData\Local\Temp\docusaurus-node20\node-v20.19.5-win-x64\node.exe"
$js = @'
const fs = require('node:fs');
const HTML = 'C:/Users/fuhua/sync/easydetek/0.project/OK/20261014-ed_edk60_demos/ed_edk60_demos/release/track.html';
const html = fs.readFileSync(HTML, 'utf8');
const code = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)][0][1];
let fails = 0;
const ok = (n, c, x) => { console.log((c ? 'PASS  ' : 'FAIL  ') + n + (x && !c ? '  << ' + x : '')); if (!c) fails++; };

// 1) 整段脚本语法可编译
try { new Function(code); ok('整段脚本语法可编译', true); }
catch (e) { ok('整段脚本语法可编译', false, e.message); }

// 2) 无外链、无本地存储
const ext = [...html.matchAll(/(?:src|href)\s*=\s*[\x22\x27]([^\x22\x27]+)[\x22\x27]/g)].map(m => m[1]);
ok('无外部 src/href 引用', ext.length === 0, JSON.stringify(ext));
ok('无 localStorage/indexedDB/cookie', !/localStorage|sessionStorage|indexedDB|document\.cookie/.test(code));

// 3) 抽取 CFG + CORE + 自测用例并执行（不依赖 DOM）
const start = code.indexOf('const CFG = {');
const end = code.indexOf('const state = {');
const M = new Function(code.slice(start, end) +
  '\nreturn { CFG, EdvParser, createFrame, parseTargetReport, tickValues,' +
  ' projectionBounds, heightAboveGround, colorFor, runSelfTest };')();
const r = M.runSelfTest(true);
console.log(r.text);
ok('帧解析 + 只读计划用例 6/6', r.pass === 6 && r.total === 6, r.pass + '/' + r.total);

const b = M.projectionBounds(800, true);
ok('侧装 800cm -> x[-500,500] y[0,800]',
   b.x0 === -500 && b.x1 === 500 && b.y0 === 0 && b.y1 === 800, JSON.stringify(b));
ok('tickValues(0,800)', M.tickValues(0, 800).join(',') === '0,100,200,300,400,500,600,700,800');
ok('离地高度 ceiling = 挂高 - z', M.heightAboveGround(149, false, 300) === 151);

// 交叉验证：上报频率=2 的 SET 0x10 帧应与协议文档第 168 行示例逐字节一致（含校验和）
const fSet = M.createFrame(0xA1, 0x10, Uint8Array.from([0x00, 0x02]), 1);
const hexSet = [...fSet].map(v => v.toString(16).toUpperCase().padStart(2, '0')).join(' ');
ok('SET 0x10=2 与协议文档示例一致',
   hexSet === '45 44 00 0D 00 01 A1 10 00 02 00 02 4C', hexSet);
console.log('\n结果：' + (fails === 0 ? '全部通过' : fails + ' 项失败'));
process.exit(fails === 0 ? 0 : 1);
'@
& $node -e $js
```

### 4.2 模拟演示 + 渲染链路（DOM / Canvas 桩）

```powershell
$node = "C:\Users\fuhua\AppData\Local\Temp\docusaurus-node20\node-v20.19.5-win-x64\node.exe"
$js = @'
const fs = require('node:fs');
const HTML = 'C:/Users/fuhua/sync/easydetek/0.project/OK/20261014-ed_edk60_demos/ed_edk60_demos/release/track.html';
const code = [...fs.readFileSync(HTML, 'utf8').matchAll(/<script>([\s\S]*?)<\/script>/g)][0][1];
let fails = 0;
const ok = (n, c, x) => { console.log((c ? 'PASS  ' : 'FAIL  ') + n + (x && !c ? '  << ' + x : '')); if (!c) fails++; };
let clock = 0;
const reg = { text: [], counts: {} };
const methods = ['setTransform','clearRect','fillRect','beginPath','moveTo','lineTo','stroke','fill',
  'arc','arcTo','closePath','save','restore','translate','rotate','strokeRect','setLineDash','fillText'];
const ctx2d = new Proxy({}, {
  get(t, p) {
    if (p === 'measureText') return s => ({ width: String(s).length * 6.2 });
    if (p in t) return t[p];
    if (methods.includes(p)) return (...a) => { reg.counts[p] = (reg.counts[p] || 0) + 1;
      if (p === 'fillText') reg.text.push(String(a[0])); };
    return () => {};
  },
  set(t, p, v) { t[p] = v; return true; }
});
function mkEl(id, attrs) {
  return { id, style: {}, dataset: {}, hidden: false, disabled: false, checked: false, value: '',
    textContent: '', innerHTML: '', scrollTop: 0, clientHeight: 200, scrollHeight: 400,
    children: [], _h: {}, _attrs: attrs || {},
    getAttribute(n) { return this._attrs[n] === undefined ? null : this._attrs[n]; },
    classList: { _s: new Set(), add(c){this._s.add(c);}, remove(c){this._s.delete(c);}, contains(c){return this._s.has(c);} },
    addEventListener(t, f) { (this._h[t] = this._h[t] || []).push(f); },
    appendChild(c) { this.children.push(c); return c; },
    removeChild(c) { const i = this.children.indexOf(c); if (i >= 0) this.children.splice(i, 1); return c; },
    get firstChild() { return this.children[0] || null; },
    get childElementCount() { return this.children.length; },
    getBoundingClientRect() { return { width: 1000, height: 700, left: 0, top: 0 }; },
    getContext() { return ctx2d; }, focus() {} };
}
// 三个「快捷周期」按钮（data-freq 即 0x10 原始值）
const presetBtns = ['2', '4', '10'].map(v => mkEl('btn-freq-' + v, { 'data-freq': v }));
const els = new Map();
globalThis.document = { readyState: 'complete',
  getElementById(id) { if (!els.has(id)) els.set(id, mkEl(id)); return els.get(id); },
  querySelectorAll(sel) { return sel === 'button[data-freq]' ? presetBtns : []; },
  createElement(t) { return mkEl(t); }, createTextNode(t) { return { textContent: t }; }, addEventListener() {} };
const setVal = (id, v) => { document.getElementById(id).value = v; };
// 故意把界面初值写成「任务书原值 10」，验证 boot() 会用 CFG.defaults 覆盖成 2
setVal('inp-mount','200'); setVal('inp-view','800'); setVal('inp-history','50'); setVal('inp-freq','10');
setVal('sel-work','1'); setVal('sel-install','tilted');
const rafQ = [];
globalThis.window = { devicePixelRatio: 1, innerHeight: 900, addEventListener() {} };
globalThis.requestAnimationFrame = cb => { rafQ.push(cb); return rafQ.length; };
globalThis.ResizeObserver = class { observe() {} disconnect() {} };
globalThis.performance = { now: () => clock };
globalThis.navigator = { serial: {} };
globalThis.setTimeout = () => 1; globalThis.__sim = null;
globalThis.setInterval = fn => { globalThis.__sim = fn; return 1; };
globalThis.clearInterval = () => { globalThis.__sim = null; };
const fire = (id, type, ev) => { const el = document.getElementById(id); (el._h[type] || []).forEach(f => f(ev || { target: el })); };
const frame = () => { clock += 34; rafQ.splice(0).forEach(cb => cb(clock)); };

try { new Function(code)(); ok('整页脚本 boot 无异常', true); }
catch (e) { ok('整页脚本 boot 无异常', false, e.stack); process.exit(1); }
const api = globalThis.window.EDV11P;
ok('window.EDV11P 已挂载', !!api);

// 默认值：boot() 用 CFG.defaults.reportFreq 覆盖界面初值 → 2（=100ms）
ok('上报频率默认值被改为 2', document.getElementById('inp-freq').value === '2',
   document.getElementById('inp-freq').value);
ok('频率提示显示 100ms', document.getElementById('hint-freq').textContent.indexOf('当前 = 100ms') >= 0,
   document.getElementById('hint-freq').textContent);
// 快捷周期按钮：点 500ms → 10，提示同步
presetBtns[2]._h.click.forEach(f => f({ target: presetBtns[2] }));
ok('快捷按钮 500ms 生效', document.getElementById('inp-freq').value === '10' &&
   document.getElementById('hint-freq').textContent.indexOf('当前 = 500ms') >= 0,
   document.getElementById('hint-freq').textContent);
presetBtns[0]._h.click.forEach(f => f({ target: presetBtns[0] }));
ok('快捷按钮 100ms 复位', document.getElementById('inp-freq').value === '2');

fire('chk-sim', 'change', { target: { checked: true } });
for (let i = 0; i < 45; i++) { clock += 100; if (globalThis.__sim) globalThis.__sim(); frame(); }
ok('模拟 4.5s：当前 4 个目标', api.state.current.size === 4, String(api.state.current.size));
ok('累计出现过 4 个目标', api.state.seen.size === 4, String(api.state.seen.size));
let pts = 0; for (const [, h] of api.state.history) pts += h.length;
ok('轨迹点数累积且 <= 4*50', pts >= 140 && pts <= 200, String(pts));
ok('画布画了 #1..#4 各 3 行标签',
   [1,2,3,4].every(id => reg.text.filter(t => t.indexOf('#' + id + '  Z') === 0).length >= 3));
ok('画布画了目标数 HUD', reg.text.some(t => t.indexOf('当前目标 4') === 0));
ok('轨迹折线段 lineTo > 100', (reg.counts.lineTo || 0) > 100, String(reg.counts.lineTo));

let sawGone = false, retiredAtGone = 0;
for (let i = 0; i < 60; i++) { clock += 100; if (globalThis.__sim) globalThis.__sim(); frame();
  if (!sawGone && !api.state.current.has(4)) { sawGone = true; retiredAtGone = api.state.retired.length; } }
ok('id4 到点消失并进入淡出队列', sawGone && retiredAtGone > 0, String(retiredAtGone));
ok('淡出结束后被清理', api.state.retired.length === 0, String(api.state.retired.length));
ok('累计计数不回退', api.state.seen.size === 4);

fire('btn-logmock', 'click');
ok('写入 C4 样例帧后 5 个目标', api.state.current.size === 5, String(api.state.current.size));
fire('btn-selftest', 'click');
const st = document.getElementById('selftestOut');
ok('自测面板显示 6/6', st.children.map(c => c.textContent).join(' ').indexOf('6/6') >= 0);
fire('chk-sim', 'change', { target: { checked: false } });
ok('模拟可停止', !globalThis.__sim);
console.log('\n结果：' + (fails === 0 ? '全部通过' : fails + ' 项失败'));
process.exit(fails === 0 ? 0 : 1);
'@
& $node -e $js
```

### 4.3 Gate-1 P1-06 专项（默认只读 / 显式确认 / 逐条等应答）

假串口 + `confirm()` 桩 + 可推进的假时钟，驱动完整写入链路：

```powershell
$node = "C:\Users\fuhua\AppData\Local\Temp\docusaurus-node20\node-v20.19.5-win-x64\node.exe"
$js = @'
const fs = require('node:fs');
const HTML = 'C:/Users/fuhua/sync/easydetek/0.project/OK/20261014-ed_edk60_demos/ed_edk60_demos/release/track.html';
const code = [...fs.readFileSync(HTML, 'utf8').matchAll(/<script>([\s\S]*?)<\/script>/g)][0][1];
let fails = 0;
const ok = (n, c, x) => { console.log((c ? 'PASS  ' : 'FAIL  ') + n + (x && !c ? '  << ' + x : '')); if (!c) fails++; };
let now = 0, timerSeq = 0;
const timers = [];
const ctx2d = new Proxy({}, { get(t,p){ if(p==='measureText') return s=>({width:60}); if(p in t) return t[p]; return ()=>{}; },
  set(t,p,v){ t[p]=v; return true; } });
function mkEl(id){ return { id, style:{}, dataset:{}, hidden:false, disabled:false, checked:false, value:'',
  textContent:'', innerHTML:'', scrollTop:0, clientHeight:200, scrollHeight:400, children:[], _h:{}, _attrs:{},
  getAttribute(n){ return this._attrs[n]===undefined?null:this._attrs[n]; },
  classList:{ add(){}, remove(){}, contains(){ return false; } },
  addEventListener(t,f){ (this._h[t]=this._h[t]||[]).push(f); },
  appendChild(c){ this.children.push(c); return c; },
  removeChild(c){ const i=this.children.indexOf(c); if(i>=0) this.children.splice(i,1); return c; },
  get firstChild(){ return this.children[0]||null; }, get childElementCount(){ return this.children.length; },
  getBoundingClientRect(){ return {width:1000,height:700,left:0,top:0}; }, getContext(){ return ctx2d; }, focus(){} }; }
const presetBtns = ['2','4','10'].map((v,i) => { const e = mkEl('f'+i); e._attrs['data-freq'] = v; return e; });
const els = new Map();
globalThis.document = { readyState:'complete',
  getElementById(id){ if(!els.has(id)) els.set(id, mkEl(id)); return els.get(id); },
  querySelectorAll(s){ return s === 'button[data-freq]' ? presetBtns : []; },
  createElement(t){ return mkEl(t); }, createTextNode(t){ return { textContent:t }; }, addEventListener(){} };
const setVal=(id,v)=>{ document.getElementById(id).value=v; };
setVal('inp-mount','200'); setVal('inp-view','800'); setVal('inp-history','50');
setVal('inp-freq','10'); setVal('sel-work','1'); setVal('sel-install','tilted');
globalThis.setTimeout = (fn, ms) => { const t={at:now+(ms||0), fn, id:++timerSeq}; timers.push(t); return t.id; };
globalThis.clearTimeout = id => { const i=timers.findIndex(t=>t.id===id); if(i>=0) timers.splice(i,1); };
globalThis.setInterval = () => 1; globalThis.clearInterval = () => {};
globalThis.performance = { now: () => now };
const rafQ = []; globalThis.requestAnimationFrame = cb => { rafQ.push(cb); return rafQ.length; };
globalThis.ResizeObserver = class { observe(){} disconnect(){} };
let written = [];
function makePort(){ return { getInfo: () => ({ usbVendorId:1, usbProductId:2 }), async open(){}, async close(){},
  get readable(){ return { getReader: () => ({ read: () => new Promise(()=>{}), cancel: async()=>{}, releaseLock(){} }) }; },
  get writable(){ return { getWriter: () => ({ write: async b => { written.push(Uint8Array.from(b)); return b.length; }, releaseLock(){} }) }; } }; }
let confirmAnswer = true;
globalThis.window = { devicePixelRatio:1, innerHeight:900, addEventListener(){}, confirm(){ return confirmAnswer; } };
globalThis.navigator = { serial: { requestPort: async () => makePort() } };
const fire=(id,t,ev)=>{ const el=document.getElementById(id); (el._h[t]||[]).forEach(f=>f(ev||{target:el})); };
const drain = () => new Promise(r => setImmediate(r));
const frame = () => { now += 34; rafQ.splice(0).forEach(cb => cb(now)); };
async function runTimers(limit){ for (let g=0; g<(limit||600); g++){ await drain(); await drain();
  if(!timers.length){ await drain(); if(!timers.length) return; }
  timers.sort((a,b)=>a.at-b.at||a.id-b.id); const t=timers.shift(); now=Math.max(now,t.at); t.fn(); } }
const seqLog = () => document.getElementById('writeSeqLog').children.map(c=>c.textContent).join('\n');
const sets = () => globalThis.window.EDV11P.state.sentFrames.filter(f => f.op === 0xA1);

(async () => {
  new Function(code)();
  const api = globalThis.window.EDV11P;

  // A. 只连接、不点按钮 → 一条 SET 都不许有
  fire('btn-connect', 'click');
  await runTimers();
  ok('A) 未点击按钮：发出帧里 op=0xA1 条数 = 0', sets().length === 0, String(sets().length));
  ok('A) 全部为 op=0xA0 GET（>=16 条）',
     api.state.sentFrames.length >= 16 && api.state.sentFrames.every(f => f.op === 0xA0),
     String(api.state.sentFrames.length));
  ok('A) 串口实际写入字节无 0xA1', written.every(b => b[6] !== 0xA1));
  ok('A) 兜底计数 unexpectedSets = 0', api.state.unexpectedSets === 0);

  // B. 点按钮但取消 → 0 帧
  const before = api.state.sentFrames.length;
  confirmAnswer = false;
  fire('btn-write', 'click');
  await runTimers(30);
  ok('B) 取消确认后未新增任何帧', api.state.sentFrames.length === before);

  // F. 真串口不应答 → 第 1 条即超时中止
  written = [];
  confirmAnswer = true;
  fire('btn-write', 'click');
  await runTimers();
  ok('F) 设备不应答时只发 1 条 SET 即中止', written.filter(b => b[6] === 0xA1).length === 1);
  ok('F) 失败项含「超时」', seqLog().indexOf('超时') >= 0, seqLog().slice(-160));

  // C. 模拟设备在线 → 确认 → 逐条 SET → 逐条等应答
  fire('btn-disconnect', 'click');
  await runTimers(10);
  fire('chk-sim', 'change', { target: { checked: true } });
  await runTimers();
  ok('C0) 模拟模式启动后仍无 0xA1', sets().length === 0);
  setVal('sel-work','2'); setVal('inp-mount','250'); setVal('inp-freq','4');
  api.state.sentFrames.length = 0;
  confirmAnswer = true;
  fire('btn-write', 'click');
  await runTimers();
  ok('C1) 确认后共 3 条 SET，顺序 0x05→0x22→0x10', sets().length === 3 &&
     sets().map(f => f.type).join(',') === '5,34,16', JSON.stringify(sets().map(f => f.type)));
  ok('C2) 三条 SET 载荷为 02 / 00FA / 0004',
     sets()[0].hex.slice(-5,-3) === '02' && sets()[1].hex.indexOf('00 FA') >= 0 &&
     sets()[2].hex.indexOf('00 04') >= 0, JSON.stringify(sets().map(f => f.hex.slice(-8))));
  ok('C3) 每条 SET 都收到应答（3 条 0xA2 回报）',
     (seqLog().match(/0xA2 回报/g) || []).length === 3, seqLog().slice(-160));
  ok('C4) 设备按新值回读 2 / 250 / 4', api.state.dev.mode === 2 && api.state.dev.mount === 250 &&
     api.state.dev.freq === 4, JSON.stringify([api.state.dev.mode, api.state.dev.mount, api.state.dev.freq]));
  ok('C5) pending 清空 + unexpectedSets=0', api.pending.size === 0 && api.state.unexpectedSets === 0);

  // ACK 错误码：真串口 + 灌入 ACK 0x05（参数错误）
  fire('chk-sim', 'change', { target: { checked: false } });
  await runTimers(10);
  fire('btn-connect', 'click');
  await runTimers();
  written = [];
  fire('btn-write', 'click');
  for (let i = 0; i < 4; i++) await drain();
  api.feedInbound(api.createFrame(0xA9, 0x05, Uint8Array.from([0x05]), 0x7777));
  await runTimers(); frame();
  ok('ACK) 只发 1 条 SET 即中止', written.filter(b => b[6] === 0xA1).length === 1);
  ok('ACK) 显示中文原因「参数错误」', seqLog().indexOf('参数错误') >= 0, seqLog().slice(-160));
  ok('ACK) 状态栏同步记录', String(document.getElementById('dev-ack').textContent).indexOf('参数错误') >= 0);

  console.log('\n结果：' + (fails === 0 ? '全部通过' : fails + ' 项失败'));
  process.exit(fails === 0 ? 0 : 1);
})();
'@
& $node -e $js
```

### 4.4 布局契约静态校验（26 项）

检查 DOM 结构、CSS 高度链、`fitCanvas()`/`ResizeObserver` 是否仍满足「满视口 + 各自滚动」：

```powershell
$node = "C:\Users\fuhua\AppData\Local\Temp\docusaurus-node20\node-v20.19.5-win-x64\node.exe"
$js = @'
const fs = require('node:fs');
const Q = String.fromCharCode(34);
const HTML = 'C:/Users/fuhua/sync/easydetek/0.project/OK/20261014-ed_edk60_demos/ed_edk60_demos/release/track.html';
const html = fs.readFileSync(HTML, 'utf8');
const style = (html.match(/<style>([\s\S]*?)<\/style>/) || [,''])[1];
const script = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)][0][1];
const body = (html.match(/<body>([\s\S]*?)<script>/) || [,''])[1];
let fails = 0;
const ok = (n, c, x) => { console.log((c ? 'PASS  ' : 'FAIL  ') + n + (x && !c ? '  << ' + x : '')); if (!c) fails++; };
const idAt = id => body.indexOf('id=' + Q + id + Q);
const clsAt = c => body.indexOf('class=' + Q + c + Q);

ok('A1 body 只有 .app 外壳（旧 .wrap 已移除）', /<div class=.app.>/.test(body) && body.indexOf('class=' + Q + 'wrap' + Q) < 0);
ok('A2 顶栏 .topbar 在 .main 之前', clsAt('topbar') >= 0 && clsAt('topbar') < clsAt('main'));
ok('A3 顶栏含状态条 + 控件行 + 日志折叠按钮',
   /class=.tb-row statusbar./.test(body) && /class=.tb-row tb-controls./.test(body) && idAt('btn-logtoggle') >= 0);
ok('A4 顶栏含 连接/模拟/安装方式/显示范围/挂高/轨迹点数',
   ['btn-connect','chk-sim','sel-install','inp-view','inp-mount','inp-history'].every(id => idAt(id) >= 0));
const leftAt = clsAt('col-left'), rightAt = clsAt('col-right');
const canvasAt = body.indexOf('canvas-panel'), logAt = idAt('logPanel');
ok('A5 左列内顺序 = 画布 → 日志，且日志在右列之前',
   leftAt >= 0 && canvasAt > leftAt && logAt > canvasAt && logAt < rightAt);
ok('A6 右列包含全部配置面板',
   ['writePanel','targetTableWrap','dev-info','sel-work','writeSeqLog'].every(id => idAt(id) > clsAt('col-right')));

ok('B1 html,body 高度 100%', /html,\s*body\s*\{[^}]*height:\s*100%/.test(style));
ok('B2 body overflow:hidden', /body\s*\{[^}]*overflow:\s*hidden/.test(style));
ok('B3 .app = 100vh + 纵向 flex', /\.app\s*\{[^}]*height:\s*100vh[^}]*flex-direction:\s*column/.test(style));
ok('B4 .topbar flex:none', /\.topbar\s*\{[^}]*flex:\s*none/.test(style));
ok('B5 .main flex:1 + min-height:0 + 两列 grid',
   /\.main\s*\{[^}]*flex:\s*1[^}]*min-height:\s*0[^}]*grid-template-columns:\s*minmax\(0,\s*1fr\)\s+var\(--right-col\)/.test(style));
ok('B6 .col-right 独立纵向滚动', /\.col-right\s*\{[^}]*min-height:\s*0[^}]*overflow-y:\s*auto/.test(style));
ok('B7 .col-left 纵向 flex + min-height:0', /\.col-left\s*\{[^}]*min-height:\s*0[^}]*flex-direction:\s*column/.test(style));
ok('B8 .canvas-panel flex:1 + min-height:0', /\.canvas-panel\s*\{[^}]*flex:\s*1[^}]*min-height:\s*0/.test(style));
ok('B9 #plotWrap relative + flex:1', /#plotWrap\s*\{[^}]*position:\s*relative[^}]*flex:\s*1/.test(style));
ok('B10 #plot 绝对定位填满容器', /#plot\s*\{[^}]*position:\s*absolute[^}]*inset:\s*0[^}]*width:\s*100%[^}]*height:\s*100%/.test(style));
ok('B11 .logpanel 定高 + 不伸缩 + 可折叠',
   /\.logpanel\s*\{[^}]*flex:\s*none[^}]*height:\s*var\(--log-h\)/.test(style) && /\.logpanel\.collapsed/.test(style));
ok('B12 .logbox overflow:auto', /\.logbox\s*\{[^}]*overflow:\s*auto/.test(style));
ok('B13 矮屏媒体查询压缩日志', /@media\s*\(max-height:\s*800px\)/.test(style));
ok('B14 窄屏媒体查询单列', /@media\s*\(max-width:\s*1100px\)[\s\S]*?grid-template-columns:\s*1fr/.test(style));
ok('B15 无旧布局规则残留', !/\.wrap\s*\{/.test(style) && !/\.grid\s*\{/.test(style) && !/^\s*\.bar\s*\{/m.test(style));

const fit = (script.match(/function fitCanvas\(\)\s*\{[\s\S]*?\n\}/) || [''])[0];
ok('C1 fitCanvas 读容器 rect 宽高', /getBoundingClientRect\(\)/.test(fit) && /rect\.width/.test(fit) && /rect\.height/.test(fit));
ok('C2 fitCanvas 不再用 innerHeight / 固定比例', !/innerHeight/.test(fit) && !/0\.66/.test(fit) && !/720/.test(fit));
ok('C3 ResizeObserver 观察 plotWrap 与 logPanel',
   /observe\(plotWrap\)/.test(script) && /observe\(\$\('logPanel'\)\)/.test(script));
ok('C4 折叠按钮绑定 toggleLogPanel',
   /\$\('btn-logtoggle'\)\.addEventListener\('click'/.test(script) && /function toggleLogPanel/.test(script));
ok('C5 drawScene 边距随尺寸自适应', /const pad = \{[\s\S]{0,200}W \* 0\.075[\s\S]{0,200}H \* 0\.05/.test(script));

console.log('\n结果：' + (fails === 0 ? '全部通过' : fails + ' 项失败'));
process.exit(fails === 0 ? 0 : 1);
'@
& $node -e $js
```

四段脚本本次均返回 `exit=0`。

---

## 5. 未覆盖项与上板/上浏览器验收清单

### 5.1 为什么没做

- 本环境没有可用浏览器（无 GUI 会话 / 未安装 Chromium），无法做真实 Canvas 像素目检；
  Web Serial 也必须有真实设备与用户授权才能触发。
- 因此本次只做到「逻辑正确性 + 绘制请求正确性」级别，视觉与硬件留待实机一次通过。

### 5.2 浏览器目检清单（约 4 分钟）

打开 `release/track.html`（双击即可，或任意本地静态服务），把窗口调到 1280×720 再调到 1920×1080，依次确认：

**布局（本轮改造项）**

- [ ] 顶栏（标题/状态条/按钮与参数）**始终吸顶**，滚动右栏时它不动
- [ ] 页面右侧**没有整页滚动条**；滚动只发生在右栏内部（右栏出现自己的竖向滚动条）
- [ ] 左列画布**随窗口变宽变高**（拖大窗口画布同步变大，不是固定像素）
- [ ] 「原始帧日志」就在**画布正下方**、高度约 170–200px、内部可滚动，**不需要往下翻页就能看到**
- [ ] 点「收起日志」→ 日志只剩标题行、画布立刻变高；再点「展开日志」→ 恢复
- [ ] 把窗口压到 ~600px 高：右栏仍只在自身滚动、画布与日志都还在可视区内
- [ ] 把窗口压窄到 <1100px：退化为单列堆叠（画布在上、面板在下），可整页滚动

**功能**

- [ ] 首屏无报错；`F12` Console 无红色异常；日志区首行显示「启动自检：6/6 通过」
- [ ] 副标题明确写了「本页默认只读」与「模组型号分支未定版，以设备回读为准」
- [ ] 「上报频率 ×50ms」输入框默认显示 **2**，下方提示为「当前 = 100ms」
- [ ] 点快捷按钮 200ms / 500ms → 输入框变 4 / 10，提示同步变「当前 = 200ms / 500ms」
- [ ] 勾选「模拟演示」：出现 4 条不同颜色的移动轨迹（虚线拖尾），画布右下 HUD 显示目标数
- [ ] 约 9 秒后第 4 个目标消失 → 其轨迹 1.5 秒内淡出 → 目标数从 4 变 3、累计仍为 4
- [ ] 模拟启动约 1 秒后「设备回报」与「当前回读」被填写（来自模拟设备的 GET 应答）
- [ ] **观察日志：模拟启动只出现 `发 … op=A0` 的 GET，一条 `op=A1` 都没有**
- [ ] 点「下发配置…」→ 弹确认框（含三项目标值）→ 点「取消」→ 日志显示「已取消下发配置：未发送任何帧」
- [ ] 再点「下发配置…」→ 确认 → 侧栏「写入配置」面板逐条出现 `→ 第 1/3 条…`、`设备 0xA2 回报 …`
- [ ] 三个写入按钮以外的任何操作（改安装方式、改范围、改轨迹点数）都不产生 SET
- [ ] 顶部状态条：帧率约 10 帧/秒（模拟固定 100ms 一帧）、坏帧 0、最后上报时间在 0.1 秒内跳动
- [ ] 改「显示范围」为 1200 → 网格与刻度随之变化且图形不拉伸变形
- [ ] 切「安装方式 → 朝下安装」→ 标题与「离地」列随之改变（变为 挂高 − z）
- [ ] 展开「原始帧日志」→ 每 100 ms 一条 `模拟 45 44 … A3 70 … → msg=… type=0x70 目标上报`
- [ ] 点「运行帧解析自测」→ 侧栏显示 6/6 与逐条明细
- [ ] 点「写入一条 0x70 样例帧」→ 立刻画出 5 个目标（C4 用例数据）

### 5.3 上板清单（需要 EDK60_VA + EDV11P）

- [ ] 点「连接串口」→ 选择 EDK60 的 USB CDC 端口 → 状态变「已连接」，串口字段显示 VID:PID
- [ ] 日志按 100/300/550 ms 的节奏出现 `GET 0x02`、5 条基础 `GET`、10 条高级 `GET`
- [ ] **连接全程日志里没有任何 `SET`（op=A1）**；设备配置未被改写（可用 .py 上位机复查一遍做对照）
- [ ] 设备回报区被填满（模块信息 `EDV11P_VB vX.Y.Z`、工作模式、挂高、检测范围/高度）
- [ ] 有人走动时出现轨迹；帧率接近 `1000 / (上报频率值 × 50ms)`（默认 2 → 约 10 帧/秒；改成 4 → 约 5 帧/秒）
- [ ] **写入链路**：点「下发配置…」→ 确认 → 每条 SET 后应收到 `REPORT`（0xA2）或 `ACK 0x01`；
      若设备回 `0xA9` 且 code≠0x01，页面要点出中文原因并停止后续写入
- [ ] **超时表现**：若设备对某条 SET 不应答，500 ms 后侧栏应显示「等待应答超时」并列出失败项（不是继续硬发）
- [ ] **验证 U1（已裁决，此处只做实测复核）**：把上报频率依次设为 2 和 4，观察帧率是否约为 10 帧/秒
      与 5 帧/秒（即单位确为 50ms；若两档帧率相同或差 2 倍但对不上，说明该固件单位不同 → 回填 design.md）
- [ ] **验证 U2**：日志里 0x70 帧的 `op` 是否恒为 `0xA3`
- [ ] **验证 U3**：人站在挂高 200 cm 的场地内，目标的「离地」值是否落在 60–180 cm 的人体高度
      （若恒等于雷达直读的斜距高度，说明固件未做姿态转换 → 需回到 design.md U3）
- [ ] 坏帧计数：若长时间为 0 且数据流畅，说明 921600 下的分包与校验处理正常；
      若持续增长，记录日志里的「协议解析警告」原文

---

## 6. 已知偏差记录（与任务书原文的差异，均有依据）

| 项 | 任务书原文 | 实现 | 依据 |
|---|---|---|---|
| 侧装坐标变换 | 「移植 .py 里的旋转函数（绕 +y 倾斜 60°、俯角 30°）」 | **不做任何旋转**；60°/30° 只进文档 | .py 第 12、849 行注释「固件 XYZ 已完成转换，本程序不重复旋转」；PM 2026-10-18 复核纠正 |
| 0x70 的 op | 「A2 REPORT」 | 主路径 **0xA3**，兼容 0xA2 | .py 第 2455 行；协议文档第 61/120 行；C 实现第 333 行 |
| 上报频率默认值 | 「10（即 100ms）」 | **默认 2（=100ms）**，与协议文档示例一致；并给 100/200/500ms 三个快捷按钮 | 任务书原值 10 会得到 500ms；单位 50ms 的证据：.py 第 1562/1994/2581 行、协议文档第 73 行与第 164-169 行示例（100ms → 值 2）。**U1 已由 PM 于 2026-10-18 裁决，默认值改为 2** |
| 历史轨迹点数 | 「约 50 点」 | 默认 50（.py 默认 100，可在界面改 10–1000） | 任务书要求 |
| 轨迹消失 | 「目标消失后轨迹淡出」 | 淡出 1.5 s（.py 是立即删除） | 任务书要求；见 design.md D3 |
| SET 下发时机 | 「连接后按顺序发 GET … 并 SET 工作模式/挂高/频率」 | **改为只读**：连接只发 GET；SET 必须由用户点「下发配置…」/「演示用一键配置」并二次确认，且逐条等应答 | **Gate-1 P1-06**（自动 SET 会改写用户设备配置、且不等应答违反协议异步约定）；见 design.md D1 废止说明与 D9–D11 |
| 页面能力表述 | 「人员轨迹与人数统计」 | 标题改「目标轨迹查看」；状态条改「当前目标 / 累计出现」；并声明模组型号分支（-02 人数统计 / -03 跌倒）未定版、「以设备回读为准」 | PM 2026-10-18 事实核实：型号分支未定，不得下结论性宣称 |
| 页面布局 | 「顶栏 + 单列长页面」（右侧栏过高、日志被推到底部） | **满视口应用壳**：顶栏吸顶不滚动；主区两列 = 左（画布 flex:1 + 正下方定高日志）/ 右（独立 `overflow-y:auto`）；整页 1280×720 起无纵向滚动 | 用户实机截图反馈（右栏过高、左下方大片空白、日志看不到） |

---

## 7. 布局回归（满视口应用壳改造）

只改布局与呈现，**未触碰任何协议 / 解析 / 写入逻辑**（P1-06 的只读与逐条应答行为在第 3.2 节回归通过）。

### 7.1 结构要点

```
body
└─ .app                      height:100vh; display:flex; column   ← 整页不滚动
   ├─ header.topbar           flex:none（吸顶，不随内容滚动）
   │   ├─ .tb-row.tb-brand    标题 + 一行说明（默认只读 / 型号未定版）
   │   ├─ .tb-row.statusbar   7 个状态 chip
   │   ├─ .tb-row.tb-controls 连接/断开/模拟/清空/自测 · 安装方式/显示范围/挂高/轨迹点数 · 收起日志
   │   └─ .note#envNote       环境提示（仅在不支持 Web Serial 时出现）
   └─ main.main               flex:1; min-height:0; grid: minmax(0,1fr) 372px
      ├─ section.col-left     flex column; min-height:0
      │   ├─ .canvas-panel    flex:1; min-height:0  → #plotWrap(flex:1, relative) > canvas(absolute inset:0)
      │   └─ .logpanel#logPanel flex:none; height:var(--log-h, 200px；矮屏 168px)  → .logbox(overflow:auto)
      └─ aside.col-right      overflow-y:auto; min-height:0  ← 所有配置面板只在自己的列内滚动
          ├─ 目标配置值（工作模式 / 上报频率 / 快捷周期）
          ├─ 写入配置（默认只读）
          ├─ 当前目标
          └─ 设备回报（+ 自测结果）
```

- 画布高度链路：`.main(flex:1)` → `.col-left(min-height:0)` → `.canvas-panel(flex:1,min-height:0)` →
  `#plotWrap(flex:1)`；`fitCanvas()` 只读 `#plotWrap.getBoundingClientRect()`，**不再有固定像素高度、
  也不再按 `innerHeight × 0.66` 估算**；`ResizeObserver` 同时观察 `plotWrap` 与 `logPanel`。
- 日志可折叠：「收起日志 / 展开日志」按钮切换 `.logpanel.collapsed`（42px），折叠后画布自动变高。
- 图例从「画布下方一行」改为**叠在画布右上角**，不占布局高度，也不压住 X 轴刻度与右下目标数 HUD。
- 小屏（<1100px）退化为单列堆叠，主区自身可滚动；矮屏（<800px 高）自动把日志压到 168px 保证画布可用。

### 7.2 布局契约静态校验：**26/26 通过**

（脚本见 §4.4；这类检查不能替代像素级目检，但能防止「以后有人把某条 CSS 改回去」）

| 组 | # | 断言 | 结果 |
|---|---|---|---|
| DOM 结构 | A1 | body 只有 `.app` 外壳，旧 `.wrap` 已移除 | ✅ |
| | A2 | `.topbar` 出现在 `.main` 之前 | ✅ |
| | A3 | 顶栏含状态条行 + 控件行 + 日志折叠按钮 | ✅ |
| | A4 | 顶栏含 连接 / 模拟 / 安装方式 / 显示范围 / 挂高 / 轨迹点数 | ✅ |
| | A5 | 左列内顺序为 画布 → 日志，且日志在右列开始之前 | ✅ |
| | A6 | 右列 `.col-right` 包含全部配置面板（写入/目标/设备回报/工作模式） | ✅ |
| CSS 高度链 | B1 | `html, body { height:100% }` | ✅ |
| | B2 | `body { overflow:hidden }`（桌面端无整页滚动） | ✅ |
| | B3 | `.app { height:100vh; flex-direction:column }` | ✅ |
| | B4 | `.topbar { flex:none }`（不参与伸缩） | ✅ |
| | B5 | `.main { flex:1; min-height:0; grid-template-columns:minmax(0,1fr) var(--right-col) }` | ✅ |
| | B6 | `.col-right { min-height:0; overflow-y:auto }` | ✅ |
| | B7 | `.col-left { min-height:0; flex-direction:column }` | ✅ |
| | B8 | `.canvas-panel { flex:1; min-height:0 }`（无固定像素高） | ✅ |
| | B9 | `#plotWrap { position:relative; flex:1 }` | ✅ |
| | B10 | `#plot { position:absolute; inset:0; width/height:100% }` | ✅ |
| | B11 | `.logpanel { flex:none; height:var(--log-h) }` + `.collapsed` 存在 | ✅ |
| | B12 | `.logbox { overflow:auto }`（日志内部滚动） | ✅ |
| | B13 | `@media (max-height:800px)` 矮屏压缩日志 | ✅ |
| | B14 | `@media (max-width:1100px)` 单列退化 | ✅ |
| | B15 | 旧布局规则（`.wrap` / `.grid` / `.bar`）已无残留 | ✅ |
| JS 跟随 | C1 | `fitCanvas()` 读 `getBoundingClientRect()` 的 width/height | ✅ |
| | C2 | `fitCanvas()` 不再出现 `innerHeight` / `0.66` / 固定 720 | ✅ |
| | C3 | `ResizeObserver` 观察 `plotWrap` 与 `logPanel` | ✅ |
| | C4 | 折叠按钮绑定 `toggleLogPanel()` | ✅ |
| | C5 | `drawScene()` 边距随画布尺寸自适应（小画布不被边距吃空） | ✅ |

### 7.3 ⚠ 截图：**本环境无法产出（沙箱阻断），需在有桌面的机器上补**

要求的两张「无头 Edge 1280×720 / 1920×1080」截图**没有取到**，原因是本会话的文件/进程沙箱禁止
Chromium 所需的**命名管道（mojo IPC）**，Edge 一启动就 FATAL 退出，四种参数组合（`--headless=new`、
`--single-process`、`--no-zygote`、`--no-sandbox`、独立 `--user-data-dir`）结果一致：

```
FATAL:mojo\public\cpp\platform\platform_channel.cc:187] Check failed: . : 拒绝访问。 (0x5)
exit=-2147483645  →  未生成 PNG
```

这不是页面问题（同样的页面在普通桌面 Edge 里可正常打开），属于**执行环境限制**，无法在本会话绕过。

**请在有桌面的机器上执行下面两条命令补图**（Edge 已装即可，无需任何依赖）：

```powershell
$edge = "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
$html = "C:\Users\fuhua\sync\easydetek\0.project\OK\20261014-ed_edk60_demos\ed_edk60_demos\release\track.html"

# 1280×720
& $edge --headless=new --disable-gpu --hide-scrollbars=false `
        --screenshot="...\release\track-layout-1280x720.png" `
        --window-size=1280,720 "file:///$($html -replace '\\','/')"

# 1920×1080
& $edge --headless=new --disable-gpu --hide-scrollbars=false `
        --screenshot="...\release\track-layout-1920x1080.png" `
        --window-size=1920,1080 "file:///$($html -replace '\\','/')"
```

截图后请按这张表逐项确认并回填：

| 视口 | 预期画面 | 判定 |
|---|---|---|
| 1280×720 | 顶栏占满宽度且不滚动；左列画布高度 ≈ 720 − 顶栏(~150) − 日志(168) − 间距；日志紧贴画布下方且可见；右列出现**竖向滚动条**（面板超出列高）；**页面右侧无整页滚动条** | ⬜ 待补 |
| 1920×1080 | 同上；画布随窗口变宽变高（不再是固定像素）；右列面板一屏内基本可见但仍按需滚动 | ⬜ 待补 |

> 若截图里右列没有滚动条，说明该视口下右列内容没有超出列高 —— 属正常（内容装得下），
> 但只要把窗口高度压到 ~600px 就应出现列内滚动条。可用这条快速判断。

### 7.4 附：本轮为截图尝试产生的残留（**待用户确认后删除**）

`release\.layout-profile\` —— Edge 启动失败时留下的临时 profile 目录（145 个文件 / 约 3.5 MB）。
它不是交付物，也不被任何文件引用。按团队守则删除需用户确认，故保留未动：

```powershell
Remove-Item -Recurse -Force "...\ed_edk60_demos\release\.layout-profile"
```

