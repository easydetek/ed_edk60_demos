# EDV163 网页版水位监测工具 — 自测报告（level.html）

| 项 | 内容 |
|---|---|
| 被测文件 | `release/level.html`（单文件，2631 行，118 537 字节） |
| 设计说明 | `docs/project/edv163/web-level-tool-design.md` |
| 环境 | Windows + Node v20.19.5（`C:\Users\fuhua\AppData\Local\Temp\docusaurus-node20\node-v20.19.5-win-x64\node.exe`） |
| 未使用 | 真实浏览器（本机沙箱禁止 Chromium 命名管道）、真实 EDV163 硬件 |
| 报告时间 | 2026-10-22 |
| 结论 | **无硬件自测全部通过**：帧解析用例 **10/10**、整页桩环境 **44/44**、布局与不变量契约 **32/32**；待上板验收（见第 5 节） |

---

## 1. 结论摘要

| 检查项 | 结果 |
|---|---|
| 整段脚本语法可编译（`new Function(code)`） | 通过 |
| 页内「运行帧解析自测」10 条用例 | **10/10 通过** |
| 组帧/转义与**独立参考实现**逐字节一致（5 例，含跨转义边界） | 通过 |
| 含 `0x09/0x0A/0x45/0x44` 的载荷往返一致 | 通过 |
| 逐字节切片喂入（模拟串口逐字节到达）仍还原出帧 | 通过 |
| 模拟水位全程在量程内（0~300 cm） | 通过 |
| 整页桩环境（假串口/假时钟/DOM+Canvas 桩）44 项 | **44/44 通过** |
| 默认只读：连接后 8 条查询全为 `op=0xA0`，SET 数 = 0 | 通过（真串口路径 + 模拟路径各测一遍） |
| 取消确认框 → 一条帧都不发 | 通过 |
| 逐条 SET 等应答；500 ms 超时中止且不硬发后续 | 通过（假串口不应答场景） |
| 一键配置 6 条 SET 全部收到 0xA2 | 通过（模拟设备） |
| 运行期「未确认 SET」计数恒为 0 | 通过 |
| 布局契约与不变量静态校验 32 项 | **32/32 通过** |

---

## 2. 帧解析用例（10 条，页内按钮与 Node 抽取两条路径都跑过）

| # | 用例 | 输入要点 | 关键期望 | 结果 |
|---|---|---|---|---|
| C1 | 正常 0x81 单通道 `tlv_len=8` | distance=500 mm，挂高 2000 mm | 组帧 = `45 44 00 14 00 11 A3 81 00 08 01 00 01 01 00 01 F4 00 D2 0A`；水位 **1500 mm** | 通过 |
| C2 | 0x81 双通道 `tlv_len=15` | ch0 运动/1200 mm；ch1 presence=0/state=0/distance=0 | 2 通道；ch0 有效 800 mm、ch1 无效（距离 0）；主通道 = ch0 | 通过 |
| C3 | **含转义字节** | distance=`0x0A09`、angle=`0x09`，挂高 5000 mm | 组帧含 **3 处** `09 01`/`09 00`；**线上 23 B 而长度域 20 B**；还原 3 处；水位 **2431 mm** | 通过 |
| C4 | 坏校验和 | C1 的校验和 `D2` 改成 `D3`，后接一条好帧 | `badChecksum=1`；坏帧 19 B 计入噪声；仍解出后一条好帧（水位 1500 mm） | 通过 |
| C5 | 截断帧 | C1 去掉末 4 字节 + 完整 C2 | `badTrailer=1`；重同步后解出 C2（2 通道、水位 800 mm） | 通过 |
| C6 | 噪声后重同步 | 噪声 `FF 0A 45 FF 0A 00 09 0A`（含**裸 0x0A**与裸 0x45）+ 好帧 | 噪声 8 B 被丢弃；**裸 0x0A 不被误判为帧尾**；好帧正常解出 | 通过 |
| C7 | 非法转义序列 | `09 05` + 好帧 | `badEscape=1`（状态条「转义错误」）；重同步后解出好帧 | 通过 |
| C8 | **旧 8B 口径**（PM 要求） | `tlv_len=9`（= 1+8×1），帧本身长度/校验和合法 | `parseReport81` 判**载荷异常**、不产出通道数据；报错点明「旧文档 8B 口径」；不计入坏帧（帧是好的，载荷是坏的 → 计入 `badPayload`） | 通过 |
| C9 | **静水面**（硬件工程师要求） | `presence=0, state=0, distance=1234`，挂高 2000 | 水位照常算出 **766 mm**，且 `stillWater=true`（界面显示「判定：无人（静水面）」） | 通过 |
| C10 | 只读计划 / 写入计划不变量 | — | 8 条 GET 全 `op=0xA0`（零 SET）、时序单调、覆盖 0x01/0x02/0x11/0x17/0x18/0x25/0x26/0x27；基础写入 3 条形态正确；一键配置 6 条齐全、`0x27`=16 B、`0x26`=4 B 且 **max(3000) ≥ 挂高 3000** | 通过 |

### 2.1 几个数字为什么是这些值（免得下次误判）

- **C3 的「线上 23 B / 长度域 20 B」**：长度域按**转义前**整包算（12 + 8 = 20），
  而正文 3 个字节各被转义成 2 字节（+3），加帧尾 1 字节 → 线上 20+3 = 23 B。
  这是「长度与校验和按还原后字节计算」最直接的证据。
- **C4 的噪声 19 B**：坏校验和帧被丢弃后按 **1 字节滑动**重同步，其 19 字节正文（帧头..校验和）
  全部计为 `noiseBytes`（与 `track-selftest.md` C5 的口径一致：noiseBytes = 被同步逻辑丢弃的字节数）。
- **C5 的 `badTrailer`**：截断帧收满声明长度时借用到了下一条帧的字节，随后「下一个字节不是 0x0A」
  → 判 `badTrailer`；因为只 skip 1 字节，下一条好帧仍能被重新捕获。
- **C8 不计入坏帧**：帧级（长度/校验和/帧尾）全部合法，问题只在载荷语义；
  计入 `stats.badPayload`（也计入状态条「坏帧」总数），但不会影响后续帧。

---

## 3. 整页桩环境检查（44 项，全部通过）

桩环境：假串口（`navigator.serial.requestPort`）/ 可控假时钟（`setTimeout`/`setInterval`/`performance.now`）/
DOM + Canvas 2D 桩 / `window.confirm` 桩 / `requestAnimationFrame` 手动泵。

### 3.1 真串口路径：默认只读（1–16）

| # | 检查 | 结果 |
|---|---|---|
| 1 | 整页脚本 boot 无异常 | 通过 |
| 2 | `window.EDV163` 调试入口已挂载 | 通过 |
| 3 | 启动自检在日志里显示 10/10 | 通过 |
| 4 | 点「连接串口」后状态 = 已连接 | 通过 |
| 5 | 串口标签显示 `USB 0x303A:PID` | 通过 |
| 6 | 波特率显示 **256000** | 通过 |
| 7 | 连接后共发出 **8 条**查询 | 通过 |
| 8 | 8 条**全部** `op = 0xA0`（零 SET） | 通过 |
| 9 | 覆盖 0x01/0x02/0x11/0x17/0x18/0x25/0x26/0x27 | 通过 |
| 10 | 状态条「未确认 SET」= 0 | 通过 |
| 11 | 取消确认框后**一条帧都不发** | 通过 |
| 12 | 确认后先发第 1 条 SET（0x25 挂高） | 通过 |
| 13 | 设备不应答时 500 ms 超时中止，**不硬发后续**（共 1 帧） | 通过 |
| 14 | 下发日志列出「等待应答超时」并标记中止 | 通过 |
| 15 | 超时中止不计入「未确认 SET」 | 通过 |
| 16 | 确认框文案包含「逐条发送 SET」说明 | 通过 |

### 3.2 模拟演示 + 挂高回读校正（17–33）

| # | 检查 | 结果 |
|---|---|---|
| 17 | 断开后状态 = 未连接 | 通过 |
| 18 | 勾选「模拟演示」后状态 = 模拟演示中 | 通过 |
| 19 | 设备回读挂高 = **3000 mm**（出厂值） | 通过 |
| 20 | 未手动改过挂高 → **自动采用** 3000 mm（`mountTouched=false`） | 通过 |
| 21 | 挂高一致后不一致告警**不显示** | 通过 |
| 22 | Y 轴跟随挂高 = 300 cm | 通过 |
| 23 | 回读频率 = 1000 ms、模式 = 正常工作 | 通过 |
| 24 | 曲线累积数据点 ≥ 5 | 通过 |
| 25 | 水位**只显示整数 cm** | 通过 |
| 26 | 主视图水位在 0~300 cm 内 | 通过 |
| 27 | 水位柱百分比已更新 | 通过 |
| 28 | 画布绘制了文字（坐标轴/标签/标题） | 通过 |
| 29 | 画布绘制了曲线折线段 | 通过 |
| 30 | 口径行写明「换算用挂高 = 3000 mm」 | 通过 |
| 31 | **静水面**（presence=0/state=0，distance=1234）照常出水位 **177 cm** | 通过 |
| 32 | 状态标签显示「判定：无人（静水面）」 | 通过 |
| 33 | 口径行提示「静水面…属正常现象」 | 通过 |

### 3.3 写入链路（模拟设备应答）+ 其它按钮（34–44）

| # | 检查 | 结果 |
|---|---|---|
| 34 | 基础下发 3 条 SET | 通过 |
| 35 | SET 顺序 = 挂高(0x25) → 频率(0x17) → 模式(0x18) | 通过 |
| 36 | 3 条均收到 0xA2 应答（日志「下发完成」） | 通过 |
| 37 | 运行时「未确认 SET」仍为 0 | 通过 |
| 38 | 一键配置发出 **6 条** SET | 通过 |
| 39 | 一键配置覆盖 0x25/0x26/0x27/0x11/0x17/0x18 | 通过 |
| 40 | 一键配置后「未确认 SET」= 0 | 通过 |
| 41 | 点自测按钮，面板显示 10/10 | 通过 |
| 42 | 点「写入一条 0x81 样例帧」后主视图有水位 | 通过 |
| 43 | 点「清空曲线」后读数回到 `—` | 通过 |
| 44 | 取消模拟后状态回到未连接 | 通过 |

---

## 4. 布局契约与不变量静态校验（32 项，全部通过）

| # | 契约 | 结果 |
|---|---|---|
| 1–2 | `body{overflow:hidden}` + `.app{height:100vh;flex-direction:column}`（满视口应用壳） | 通过 |
| 3 | 顶栏 `flex:none`（吸顶不随内容滚动） | 通过 |
| 4–5 | 主区两列 grid（左 `minmax(0,1fr)` + 右 `380px`），右列 `overflow-y:auto` + `scrollbar-gutter:stable` | 通过 |
| 6–9 | 左列纵向 flex 可压缩；读数区 `flex:none`；曲线区 `flex:1;min-height:0`；画布 `position:absolute;inset:0` | 通过 |
| 10–12 | 日志定高 + 内部滚动；`collapsed` 隐藏日志体；默认**展开**（HTML 上无 collapsed 类） | 通过 |
| 13 | 矮屏（≤820px）与窄屏（≤1100px）两套断点 | 通过 |
| 14–15 | 左列 DOM 顺序：大字读数 → 曲线 → 日志；日志在右列之前 | 通过 |
| 16–18 | 右列含挂高/频率/模式/下发/一键/回报/自测；顶栏含连接/断开/模拟/自测/日志折叠；状态条含连接/帧率/坏帧/转义错误/最后上报 | 通过 |
| 19–21 | 水位柱（竖向 track+fill+百分比）；画布 `role="img"` + `aria-label`；挂高不一致告警容器 | 通过 |
| 22–23 | 精度说明（±8 cm + 整数 cm）；静水面说明 | 通过 |
| 24–26 | 单文件（1 个 `<script>`、无 `src`）；**无任何外链**；无 localStorage/sessionStorage/indexedDB/cookie | 通过 |
| 27 | 串口固定 **256000** 8N1 | 通过 |
| 28–29 | 全页只有 1 处 `createFrame(CFG.OP.SET…` （在 `sendSetAndWait` 内）；写入必经 `window.confirm` | 通过 |
| 30 | 只读计划函数内只出现 `op: CFG.OP.GET`，无 SET | 通过 |
| 31 | 0x81 步长**钉死 7**（`ch81Stride: 7` + `1 + count * P.ch81Stride`） | 通过 |
| 32 | `computeLevels` 内**没有** `if (c.state === 0) return …`（不按 presence/state 过滤） | 通过 |

---

## 5. 上板 / 上浏览器验收清单（需 EDK60_VA + EDV163 + Chrome/Edge）

### 5.1 上板前 30 秒：确认「默认只读」

- [ ] 打开 `release/level.html`（建议 Chrome/Edge），点「连接串口」，选 EDK60 的 CDC 端口。
- [ ] 状态条应显示：`已连接`、波特率 **256000**、帧率开始跳动（默认 1 帧/秒）。
- [ ] 日志里**只应出现 `发 45 44 …` 且解析摘要为 `op=0xA0`**；逐条核对 8 条 GET
      （0x01/0x02/0x11/0x17/0x18/0x25/0x26/0x27）。
- [ ] **验收硬指标**：日志中 `op=0xA1`（SET）出现次数 = **0**；状态条「未确认 SET」= **0**。
- [ ] 若一条 `收` 帧都没有：先查模组工作模式（页面「设备回报 → 工作模式」应回 `1 正常工作`），
      再查 EDK60 侧 UART 是否为 256000（不是 921600）。

### 5.2 挂高与水位口径

- [ ] 「设备回报 → 挂高」应有值（预期 **3000 mm** 出厂值）。
- [ ] 若页面初始 2000 mm 而后端回读 3000 mm：由于尚未手动改过，页面会**自动采用 3000 mm**，
      日志出现「按设备回读自动采用挂高 3000 mm」。
- [ ] 手动把挂高改成 2500 mm：配置区应**标红**「设备挂高（回读 3000 mm）与设定挂高（2500 mm）不一致…」，
      主视图口径行同步显示「换算用挂高 = 2500 mm」。
- [ ] 点「采用设备回读值」→ 标红消失、曲线清空、口径行回到 3000 mm（**这一步不发任何帧**）。

### 5.3 手点下发 → 逐条应答

- [ ] 点「下发配置…（需确认）」→ 弹出确认框，列出 3 条（挂高/上报频率/工作模式）。
- [ ] 先点「取消」：日志出现「已取消下发配置：未发送任何帧」，**日志里不得新增 `op=0xA1`**。
- [ ] 再点一次并确认：右列「下发顺序」区应逐条出现
      `→ 第 1/3 条：挂高 = …，等待设备应答…` → `设备 0xA2 回报 …`（或 `设备 ACK 确认 …`），
      直到「下发完成：3 条 SET 均收到设备应答」。
- [ ] **验收硬指标**：每一条 SET 都是「先发一条、收到应答、再发下一条」；
      若第 1 条无应答，500 ms 后必须出现「下发中止（已发 1/3 条），失败项：挂高：等待应答超时…」，
      并且**后续 2 条一条都不能发**。
- [ ] 点「演示用一键配置（同样需确认）」→ 确认后应发 **6 条**（0x25→0x26→0x27→0x11→0x17→0x18），
      逐条应答；结束后「设备回报」里频率应回读 10、模式应回读 1。

### 5.4 数据与换算

- [ ] 静水面上方：主视图有整数 cm 水位，曲线每秒推进；水位 = 挂高 − 距离（手算核对 1~2 点）。
- [ ] 若出现「判定：无人（静水面）」而水位**仍在更新** → 说明 presence 不过滤已生效（**预期行为**）。
- [ ] 用尺子/固定挡板把水面（或挡板）抬高约 10 cm：曲线应上升约 10 cm（受 ±8 cm 分辨率影响，
      看**趋势**而不是单点绝对值）。
- [ ] 观察「坏帧」「转义错误」两个计数：正常应保持 0 或极少；
      若「转义错误」持续增长 → 上报 U3（设备侧转义/长度口径与协议文档不符）。
- [ ] 日志里若某帧摘要标注「N 处转义」→ 说明设备确实做转义（U3 得证）。

### 5.5 浏览器目检（约 3 分钟）

- [ ] 1280×720 与 1920×1080 两档：整页**无纵向滚动条**，顶栏吸顶，左列曲线自适应变高变矮。
- [ ] 缩放窗口：曲线随容器变化（`ResizeObserver`），右列始终独立滚动、不被左列挤压。
- [ ] 点「收起日志」：日志面板缩成一行，曲线即时变高；再点「展开日志」恢复。
- [ ] 勾选「模拟演示」：无硬件也能看到注水上涨/偶发排水的水位曲线。

---

## 6. 复现命令

PowerShell 下直接可用。**注意**：脚本内正则/字符串里的双引号请用 `\x22` 表达，
否则 PowerShell 传参会把 `"` 吃掉（本报告的命令已全部按此写好）。
命令较长时建议把 `@'…'@` 内容另存为 `%TEMP%\*.js` 后用 `node 文件` 执行（第 1、2 段即用此法验证过）。

```powershell
$node = "C:\Users\fuhua\AppData\Local\Temp\docusaurus-node20\node-v20.19.5-win-x64\node.exe"
```

### 6.1 帧解析用例 + 独立参考实现交叉验证

```powershell
$js = @'
const fs = require('node:fs');
const HTML = 'C:/Users/fuhua/sync/easydetek/0.project/OK/20261014-ed_edk60_demos/ed_edk60_demos/release/level.html';
const code = [...fs.readFileSync(HTML, 'utf8').matchAll(/<script>([\s\S]*?)<\/script>/g)][0][1];
let fails = 0;
const ok = (n, c, x) => { console.log((c ? 'PASS  ' : 'FAIL  ') + n + (x && !c ? '  << ' + x : '')); if (!c) fails++; };
try { new Function(code); ok('整段脚本语法可编译', true); }
catch (e) { ok('整段脚本语法可编译', false, e.message); }
const start = code.indexOf('const CFG = {'), end = code.indexOf('const state = {');
const M = new Function(code.slice(start, end) +
  '\nreturn { CFG, EdvParser, createFrame, parseReport81, buildReport81Payload, computeLevels, pickPrimary,' +
  ' escapeBytes, unescapeBytes, buildReadOnlyQueryPlan, buildWritePlan, buildPresetWritePlan,' +
  ' simWaterLevel, tickValues, hexToBytes, bytesToHex, runSelfTest };')();
const r = M.runSelfTest(true);
console.log(r.text);
ok('用例全部通过', r.pass === r.total, r.pass + '/' + r.total);
/* 独立参考实现（不调用页面函数）交叉验证组帧与转义 */
function refFrame(op, type, msgId, data) {
  const total = 12 + data.length;
  const body = [0x45, 0x44, (total >> 8) & 0xFF, total & 0xFF, (msgId >> 8) & 0xFF, msgId & 0xFF,
                op, type, (data.length >> 8) & 0xFF, data.length & 0xFF, ...data];
  let s = 0; for (const b of body) s = (s + b) & 0xFF;
  body.push(s);
  const out = [];
  for (const b of body) { if (b === 0x0A) out.push(0x09, 0x01); else if (b === 0x09) out.push(0x09, 0x00); else out.push(b); }
  out.push(0x0A);
  return Uint8Array.from(out);
}
const cases = [
  [0xA3, 0x81, 0x0011, [0x01,0x00,0x01,0x01,0x00,0x01,0xF4,0x00]],
  [0xA3, 0x81, 0x0013, [0x01,0x00,0x01,0x01,0x00,0x0A,0x09,0x09]],
  [0xA0, 0x25, 0x0005, []],
  [0xA1, 0x25, 0x0006, [0x0B,0xB8]],
  [0xA3, 0x81, 0x00FF, [0x02,0x00,0x01,0x00,0x00,0x0A,0x0A,0x09,0x01,0x00,0x00,0x00,0x00,0x00,0x00]]
];
let refOk = true, detail = '';
for (const [op, type, id, data] of cases) {
  const a = M.bytesToHex(M.createFrame(op, type, Uint8Array.from(data), id));
  const b = M.bytesToHex(refFrame(op, type, id, data));
  if (a !== b) { refOk = false; detail += '\n  page=' + a + '\n  ref =' + b; }
}
ok('组帧/转义与独立参考实现逐字节一致（5 例）', refOk, detail);
/* 逐字节切片喂入 */
const p = new M.EdvParser(); let n = 0;
const d = [0x01, 0x09, 0x0A, 0x00, 0xFF, 0x0A, 0x09, 0x45, 0x44];
const fr = M.createFrame(0xA3, 0x81, Uint8Array.from(d), 0x0102);
for (let i = 0; i < fr.length; i++) n += p.feed(fr.subarray(i, i + 1)).length;
ok('逐字节切片喂入仍还原出 1 帧', n === 1, String(n));
/* 模拟水位量程 */
let bad = 0, minv = 1e9, maxv = -1e9;
for (let t = 0; t < 240; t += 0.5) {
  const w = M.simWaterLevel(t, 3000);
  if (!(w.distanceMm >= 0 && w.distanceMm <= 3000)) bad++;
  const lv = (3000 - w.distanceMm) / 10;
  minv = Math.min(minv, lv); maxv = Math.max(maxv, lv);
}
ok('模拟水位全程在量程内', bad === 0 && minv >= 3 && maxv < 300,
   'bad=' + bad + ' range=' + minv.toFixed(1) + '~' + maxv.toFixed(1));
console.log('\n结果：' + (fails === 0 ? '全部通过' : fails + ' 项失败'));
process.exit(fails === 0 ? 0 : 1);
'@
& $node -e $js
```

**本轮实测输出**：`自测结果：10/10 通过` + 4 项交叉验证全 PASS。

### 6.2 整页桩环境（假串口 / 假时钟 / DOM+Canvas 桩）

要点（完整脚本较长，建议存成 `%TEMP%\level-stub.js` 用 `node 文件` 跑）：

1. 假时钟：接管 `setTimeout/clearTimeout/setInterval/clearInterval` + `performance.now`，
   提供 `async advance(ms)` 按顺序触发到期定时器，每次触发后 `await` 若干微任务并泵一次 `requestAnimationFrame`；
2. 假串口：`navigator.serial.requestPort()` 返回 `{ readable.getReader().read() → 永不 resolve,
   writable.getWriter().write(bytes) → 记录到数组 }`（用它测「连接只读 + 超时中止」）；
3. DOM 桩：`getElementById` 返回带 `style/dataset/classList/children/addEventListener/
   getBoundingClientRect/getContext` 的假元素；Canvas 2D 用 `Proxy` 记录 `fillText/lineTo` 调用；
4. `window.confirm` 桩可切换 true/false（测取消与确认两条路径）；
5. 断言里**不要用固定下标**读帧的 op/type —— 用页面自己的 `EdvParser` 解码写出的字节，
   因为 msgid/长度里的 `0x0A` 被转义后下标会错位（这正是本轮发现并修掉的缺陷）。

关键断言（对应第 3 节 44 项）里最该盯的三条：

```js
// ① 连接后一条 SET 都没有
ok('连接后全部 op=0xA0（零 SET）', decodeWritten(written).every(f => f.op === 0xA0));
// ② 取消确认 → 帧数不变
const before = written.length; confirmAnswer = false; fire('btn-write','click');
await advance(60); ok('取消确认后不发任何帧', written.length === before);
// ③ 设备不应答 → 500ms 后中止，只发出 1 帧
confirmAnswer = true; fire('btn-write','click'); await advance(2200);
ok('500ms 无应答即中止，不硬发后续', decodeWritten(written.slice(before)).length === 1);
```

### 6.3 布局契约与不变量静态校验（32 项）

```powershell
$js = @'
const fs = require('node:fs');
const H = 'C:/Users/fuhua/sync/easydetek/0.project/OK/20261014-ed_edk60_demos/ed_edk60_demos/release/level.html';
const html = fs.readFileSync(H, 'utf8');
const code = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)][0][1];
let fails = 0;
const ok = (n, c, x) => { console.log((c ? 'PASS  ' : 'FAIL  ') + n + (x && !c ? '  << ' + x : '')); if (!c) fails++; };
ok('body 不滚动', /body\s*\{[^}]*overflow:hidden/.test(html));
ok('应用壳 100vh 纵向 flex', /\.app\s*\{[^}]*height:100vh[^}]*flex-direction:column/.test(html));
ok('顶栏 flex:none', /\.topbar\s*\{[^}]*flex:none/.test(html));
ok('两列 grid', /grid-template-columns:minmax\(0,1fr\) var\(--right-col\)/.test(html));
ok('右列独立滚动', /\.col-right\s*\{[^}]*overflow-y:auto/.test(html) && /scrollbar-gutter:stable/.test(html));
ok('曲线区 flex:1', /\.canvas-panel\s*\{[^}]*flex:1[^}]*min-height:0/.test(html));
ok('日志定高且可折叠', /\.logpanel\s*\{[^}]*height:var\(--log-h\)/.test(html) && /\.logpanel\.collapsed \.logbox\s*\{\s*display:none/.test(html));
ok('日志默认展开', !/id=\x22logPanel\x22[^>]*class=\x22[^\x22]*collapsed/.test(html));
ok('无外链', [...html.matchAll(/(?:src|href)\s*=\s*[\x22\x27]([^\x22\x27]+)[\x22\x27]/g)].length === 0);
ok('无本地存储', !/localStorage|sessionStorage|indexedDB|document\.cookie/.test(code));
ok('256000 8N1', /baudRate:\s*256000/.test(code) && /dataBits:\s*8/.test(code));
ok('唯一 SET 组帧点', (code.match(/createFrame\(CFG\.OP\.SET/g) || []).length === 1);
ok('只读计划无 SET', !/function buildReadOnlyQueryPlan[\s\S]{0,1200}?OP\.SET/.test(code));
ok('0x81 步长钉死 7', /ch81Stride:\s*7/.test(code) && /1 \+ count \* P\.ch81Stride/.test(code));
console.log('\n结果：' + (fails === 0 ? '全部通过' : fails + ' 项失败'));
process.exit(fails === 0 ? 0 : 1);
'@
& $node -e $js
```

（本轮完整实现共 32 项，全部 PASS；上列为其中最关键的 15 项。）

---

## 7. 未覆盖项与说明

| # | 未覆盖 | 原因 / 补做方式 |
|---|---|---|
| 1 | 真实浏览器像素级目检（含 1280×720 / 1920×1080 截图） | 本环境沙箱**禁止 Chromium 命名管道**，无法启动浏览器；布局只做了静态契约（32 项）。需在有桌面的机器上按 §5.5 目检，并补两张截图 |
| 2 | 真实 EDV163 联调 | 硬件在用户手上，本轮未连接真机；见 §5 上板清单 |
| 3 | 设备侧转义行为（U3） | 只能上板确认：看日志是否出现「N 处转义」标注，以及「转义错误」计数是否增长 |
| 4 | 0x26/0x27 设备是否接受一键配置的值 | 上板点「演示用一键配置」后看 6 条是否都收到 0xA2 / 0xA9 ACK；若被拒绝会自动中止并列出失败项 |
| 5 | 长时间运行（≥2 小时）的内存与曲线性能 | 曲线按 10 分钟窗口裁剪 + 单通道点数上限 6000，理论上有界；建议上板挂 2 小时观察帧率与「坏帧」 |
| 6 | 多通道真机（channel>0） | 模拟与用例已覆盖 2 通道解析与主通道选择；真机通道数以设备上报为准 |

---

## 8. 本轮发现并修复的缺陷（自测价值记录）

| # | 缺陷 | 发现方式 | 修复 |
|---|---|---|---|
| 1 | `createFrame` 的校验和写在 `total-1`（应为 `total-2`），且转义覆盖了含帧尾的整个数组 | C1–C7 组帧期望值全部比实际少一个 `00` | 校验和落 `total-2`，转义只覆盖 `raw[0..total-2]`，帧尾单独追加 |
| 2 | 解析器把「收满 `lenField` 字节」当作帧体，导致把帧尾当成额外字节 | 同一次用例失败暴露 | 帧体 = `lenField-1` 字节（帧头..校验和），其后必须是 `0x0A`；校验和改在 `lenField-2` |
| 3 | `recordSent` 用固定下标 `bytes[6]/bytes[7]` 读 op/type | 桩环境里 SET 被读成 `op=0x00,type=0xA1`（msgid=0x000A 被转义成两字节，下标右移） | 改为用 `EdvParser` 解码后取 `op/type` |
| 4 | 0x81 曾按「7/8 自适应步长」实现（会静默接受旧 8B 口径） | PM 裁决 7B 后复核 | 钉死 `1 + 7×count`；不一致判载荷异常并提示口径，新增用例 C8 |

> 缺陷 1/2 属同一处帧布局认知偏差（「长度域含帧尾」→ 校验和必须前移一位），
> 已按 C 实现 `edv163.c:102-143` 逐行核对确认。
