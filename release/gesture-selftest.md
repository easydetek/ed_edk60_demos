# EDV163 手扫（挥手识别）网页版工具 — 自测报告（gesture.html）

> 对象：`release/gesture.html`（122,643 字节 / 2,912 行，单文件零外链）
> 设计说明：`docs/project/edv163-gesture/web-gesture-tool-design.md`
> 日期：2026-10-24（v1.1：新增挥手动画层 + 动画锁定期）｜全栈工程师
> **浏览器未启动**（沙箱禁 Chromium），全部结论来自 Node 桩环境；截图由项目经理补
> **状态：未上板验证** —— 见第 7 节「上板验收清单」与第 9 节「未覆盖项」

---

## 1. 结论摘要

| 分类 | 通过 | 说明 |
|---|---|---|
| 页内帧解析用例（页面按钮 / 启动静默跑同一套函数） | **17 / 17** | 覆盖 ①正常行 ②`target=0` ③action 各值 ④±3 ⑤科学计数法 ⑥旧格式 ⑦`DBG:` ⑧二进制帧混流 ⑨半行截断 ⑩噪声重同步 + 坏行 + 触发 ⑪只读不变量 ⑫**动画方向表** ⑬**同一次检测去重** ⑭**缓动与轨迹** ⑮**reduced-motion 计划** |
| 模拟演示管线断言 | **13 / 13** | 60 拍 × 200 ms，字节流回灌同一解析器：60 有效行、7 帧二进制跳过、3 次重同步、0 坏行 |
| 阈值输入归一化断言 | **5 / 5** | 越界夹取、空/非数字回退、三输入框同步 |
| UI 交互断言（日志折叠 / 隐藏） | **4 / 4** | 折叠类名与按钮文案、隐藏时按钮置灰、恢复可用 |
| **动画与锁定期断言（v1.1 新增）** | **21 / 21** | ①方向 ②锁定忽略 ③解锁可再触发 ④6 连帧去重 ⑤锁定期解析不停 ⑥`pointer-events:none` ⑦reduced-motion ⑧调试钩子 ⑨0/±3 不触发 |
| 静态不变量与布局契约 | **38 / 38** | 无外链 / 无写入路径 / 无本地存储 / 满视口尺寸链 / **动画层 8 项新契约** |
| **合计** | **98 / 98 PASS** | `node --check` 语法校验 0 错 |

v1.0 的 65 项全部仍通过（其中 1 项措辞按新行为改写，原因见 §3 表注），另新增 33 项。

命令与结果一一对应，可逐字复现（第 8 节）。

---

## 2. 页内帧解析用例（17 条，全通过）

页面右上「运行帧解析自测」/ 右列「运行帧解析自测」触发；`boot()` 时也会静默跑一遍。
每条用例**独立新建** `GestureStreamParser`，互不影响，也不碰会话状态。
C14~C17 是 v1.1 新增的**纯函数**动画用例（`animPlan` / `animClassify` / `animEase` / `animOpacity`），
不碰 DOM、不开定时器 —— 所以启动时的静默自测**不会真的放动画**。

| # | 用例 | 断言 | 结果 |
|---|---|---|---|
| C1 | 正式行 `GESTURE: target=1 x=0.012 y=-0.034 z=0.156 action=1` | 1 行 / `hasTarget=true` / 三个坐标精确相等 / `waveAction=1`（左挥手） | ✅ |
| C2 | `target=0` 行 | `hasTarget=false`，x/y/z 全 0 | ✅ |
| C3 | `action=-2/-1/0/1/2` 五行 | 显示名 下/右/无/左/上；配色 `#0284c7/#2563eb/#64748b/#16a34a/#ea580c` 逐条比对 | ✅ |
| C4 | `action=3` 与 `action=-3` | `waveAction` 均落 0（不显示为动作）；`depthAction=+3/-3`；靠近/远离各计 1 | ✅ |
| C5 | `1.2e-2 / -3.4E-2 / 1.56e-1` 与 `.5 / +.25 / 2E-1` | 解析为 `0.012 / -0.034 / 0.156` 与 `0.5 / 0.25 / 0.2` | ✅ |
| C6 | 旧格式 `… z=0.42 wave=2 depth=3` | 上挥手；`depthAction=3` | ✅ |
| C7 | `DBG: f=10 target=1 pos_m=( 0.012 -0.034 0.156 ) action=-2 lock=7` + `DBG: … target=0 … action=1 lock=0` | 前者取到坐标 + 下挥手；后者 `hasTarget=false`、坐标为 0 | ✅ |
| **C8** | **合法 `0x4544` 帧 + 文本行 + 校验错 `0x4544` 帧 + 两行文本**，字节流在 5 / 31 处切成 3 片（帧中间断开） | `binFrames=1`、`resync≥1`、有效行 3、**坏行 0**、`trimmedLines≥1`，`waveSeq=[1,-1,0]` | ✅ |
| C9 | 三条 `feed`：`…x=0.0` / `12 y=-0.034 z=0.156 action=1\r` / `\nGESTURE: target=1 x=0.5 y=` | 前两片**不出行**；第三片补齐后恰好 1 行且数值正确；末尾残行留在缓冲（`pending≥1`）且**不记坏行** | ✅ |
| C10 | 行首 4 个垃圾字节 `00 FF 13 7A` + 一行 `action=2` | 该行被救回（`trimmed≥1`）、坏行 0、数值精确 | ✅ |
| C11 | `hello world` + `target=2`（越界）+ 一行正常 | 有效行 1、**坏行 2**，最新状态来自正常行（不被污染） | ✅ |
| C12 | 触发判定 10 条断言 | `z=0.156,th=20` → 触发；`z=0.200,th=20` → **不触发**（等于不算）；`z=0.201` → 不触发；`target=0` → 不触发；`th=0` → 不触发；`th=100` → 触发；`'' / 101 / -1` → 阈值非法（`null`）；`'20'` → 20 | ✅ |
| C13 | 只读不变量 9 条断言 | 导出 API 无 `write/send/tx/transmit`；`state` 无 `writer`/`serialWriter`；`GestureStreamParser.prototype` 无 `write/send` | ✅ |
| **C14** | 动画方向表 10 条断言 | `1` → x 轴 8%→92%（y 恒 50%）；`-1` → 92%→8%；`2` → y 轴 92%→8%（x 恒 50%）；`-2` → 8%→92%；转动角 90/−90/0/180；`durationMs=2500 / fadeMs=300 / moveMs=2200`；`0 / ±3 / 7` 返回 `null`；`CFG.animDirs` 只有 4 个键 | ✅ |
| **C15** | 同一次检测去重 10 条断言 | 6 连帧（0/200/…/1000 ms，同值 1）→ **1 次事件 + 5 帧残留**；间隔 301 ms → 2 次；值变化 → 2 次；`0` 打断后同值算新事件；`±3` 夹在中间不扰动（仍判残留帧）；`sameEventWindowMs=300` | ✅ |
| **C16** | 缓动与轨迹 10 条断言 | `ease(0)=0`、`ease(1)=1`、11 点采样单调、`ease(.5)∈(0.5,1)`；轨迹 `t=0 → 8%`、`t=moveMs → 92%`；`opacity` 位移段恒 1、`durationMs−150` 时 ≈0.5、`durationMs` 时 0；`ease = 0.4,0,0.2,1`、`marginPct=8` | ✅ |
| **C17** | prefers-reduced-motion 8 条断言 | `durationMs=800`、`fadeInMs=200`、`fadeMs=300`、`moveMs=500`；起终点都是 50%/50%；`opacity: 0ms=0 / 100ms=0.5 / 400ms=1 / 650ms≈0.5 / 800ms=0`；非 reduced 时仍是 2500 ms | ✅ |

```
自测结果：17/17 通过
```

### 2.1 几个数字为什么是这些值（免得下次误判）

| 数字 | 由来 |
|---|---|
| C8 的 `binFrames=1` 而不是 2 | 第 2 帧是**故意改坏校验和**的（`cs+1`），它走的是"丢 1 字节重同步"分支，计入 `resync` 而非 `binFrames`。这是 D3 决策：正常点云帧与坏帧头必须分开计数，否则"跳过帧数"失去诊断意义 |
| C8/C10 的坏行仍是 0 | 校验错帧丢弃 1 字节后，残留字节会粘在下一行前面（厂商实现里这行就废了）。本页在文本层补了一次「找 `GESTURE:`/`DBG:` 前缀再匹配」的重同步（设计文档 §6.5），所以行救回来了、只记 `trimmedLines` |
| C9 的末尾残行不记坏行 | 残行**没有换行**，解析器按厂商语义继续等下一批字节，不交付、不判坏。它只在"缓冲区超 16384 字节"时才被裁剪 |
| C11 的坏行 = 2 | `hello world` 与 `target=2` 都不匹配 6 条正则（`target` 只允许 `0|1`），各记 1 条 |
| C14 的 8% / 92% | `marginPct=8` → `百分比 = 50 + ratio×42`；`ratio` 取 `±1` 得 8%/92%。屏幕 `y` 向下，所以「屏幕下侧」= 92%、「屏幕上侧」= 8% |
| C14 的 `moveMs=2200` | `durationMs(2500) − fadeMs(300)`：位移在 2200 ms 走完，最后 300 ms 只做淡出 |
| C15 的"6 连帧仍算 1 次" | 窗口比的是**相邻两行的间隔**（200 ms ≤ 300 ms），不是整组跨度（1.2 s）。按整组跨度算会把第 2~6 行误判成新事件 |

---

## 3. 模拟演示管线（13 项，全通过）

做法：`buildSimChunk(tick)` 生成真实字节流（含 `0x4544` 帧），`feedInbound()` 喂给同一个解析器 ——
**不直接改 UI**，因此这一步同时验证解析器 + 渲染 + 统计三处。

| # | 断言 | 实测 |
|---|---|---|
| 1 | 60 行 → 60 有效行、0 坏行 | `valid=60 bad=0` |
| 2 | 跳过二进制帧 ≥ 6（每 9 拍一帧合法帧） | `binFrames=7` |
| 3 | 重同步 ≥ 2（每 23 拍一帧校验错帧） | `resync=3` |
| 4 | 被残留污染的行全部救回（坏行仍 0） | `trimmed=3 bad=0` |
| 5 | action 直方图总数 = 有效行数 | `sum=60 valid=60` |
| 6 | `±3` 只进"靠近/远离"计数，不进动作直方图 | `proxPos=2 proxNeg=2`，`actions['3']` 不存在 |
| 7 | 动作大字被真正驱动（≥3 种）※ | `["无挥手","左挥手","下挥手","右挥手"]`（4 种） |
| 8 | 触发卡片两种状态都出现（`z` 穿过 20 cm 阈值） | `触发=14 未触发=45` |
| 9 | 收字节数 > 0 | `rxBytes=3343` |
| 10 | 状态条数字与内部统计一致 | `st-valid=60 st-bin=7` |
| 11 | 两张位置图 canvas 真的在画 | `plotXZ` 2D 上下文累计 25,778 次调用（v1.1 起桩环境按 50 ms 推帧，绘制次数随虚拟时间增长，数值本身无意义） |
| 12 | 日志两类标注都在 | 含「跳过二进制帧」与「文本行」 |
| 13 | 直方图表格被填充 | `rows=7`（动作 5 行 + 靠近/远离 2 行） |

> ※ **第 7 项措辞已按 v1.1 行为改写**（原为"5 种全出现"）。原因：动画锁定期内动作大字被
> 冻结（用户明确要求"期间忽略任何状态更新"），60 拍 = 12 s 内只播约 4 次动画，
> 所以大字只可能看到 4 种而不是 5 种。这是**设计意图的可见结果**，不是退化；
> 5 种显示名与配色仍由页内 C3 全覆盖。
> 同时新增一条隐含证据：整段模拟里 `state.stats.skippedDuringAnim` 会在锁定期内增长，
> 而 `validLines` 始终等于 60 —— 说明"忽略展示"没有影响解析（见 §6 断言 ⑤ 的定量版本）。

探测点（便于对时序）：`tick=0 → 无挥手 / 未触发 / valid=1 / bin=0`；
`tick=12 → 左挥手（tick 2 触发的动画仍锁定中，大字定格） / 未触发 / valid=13 / bin=1`。

---

## 4. 阈值输入归一化（5 项，全通过）

| # | 输入 | 期望 | 实测 |
|---|---|---|---|
| 1 | `150` | 夹到 100 | `now=100` ✅ |
| 2 | `-5` | 夹到 0 | `now=0` ✅ |
| 3 | `''`（空） | 回退默认 20.0 | `now=20` ✅ |
| 4 | `abc` | 回退默认 20.0 | `now=20` ✅ |
| 5 | `35` | 保留 35，且三个输入框同步为 `35.0` | `inputs=35.0/35.0/35.0` ✅ |

口径来源：源码第 681-699 行（`_read_threshold` / `_normalize_threshold`）——
非数字回退 20.0、越界夹到 0~100、显示一位小数。

---

## 5. 静态不变量、布局与交互契约（38 项，全通过）

> 判定方式：先把 HTML 注释与 JS 注释剥掉，**只对代码**检查这些标识符
> （注释里写「本页不写 localStorage」这类免责声明属正常，不算违规；
> 自测断言里的字符串字面量 `'writer' in state` 也不算标识符）。
> 布局/交互部分：CSS 契约 22 项走源码正则（比 v1.0 多 8 项，全是动画层的）；
> 交互 4 项由桩环境真调用页面函数后读 DOM 状态。

### 5.1 不变量（16 项）

| # | 项 | 结果 |
|---|---|---|
| 1 | 代码里无 `localStorage` / `sessionStorage` / `indexedDB` | ✅（全文 4 次出现全在注释/说明文字） |
| 2 | 代码里无 `document.cookie` | ✅ |
| 3 | 代码里无 `port.writable` | ✅ |
| 4 | 代码里无 `getWriter` / `writer` 标识符 | ✅ |
| 5 | 无 `.write()` 调用 | ✅ |
| 6 | 无外链资源（`src=`/`href=` 指向 http(s)） | ✅ |
| 7 | 无 `<link rel=stylesheet>` / 外部字体 / `@import` | ✅ |
| 8 | 无 `fetch` / `XMLHttpRequest` / `WebSocket` | ✅ |
| 9 | 只用 `navigator.serial.requestPort` 取端口 | ✅ |
| 10 | 默认波特率 115200，选项含 230400/460800/921600 | ✅ |
| 11 | 正则含科学计数法分支 `[eE]` | ✅ |
| 12 | 二进制帧合法范围 11~4096 | ✅ |
| 13 | 帧头为 `[0x45,0x44]` | ✅ |
| 14 | action 5 条配色与厂商 `WAVE_ACTION_INFO` 一致 | ✅ |
| 15 | 单文件、只有一个 `<script>` 块 | ✅ |
| 16 | 动画层代码段不引入任何网络/存储/串口写入 API（`fetch`/`XMLHttpRequest`/`localStorage`/`writable` 零命中） | ✅ |

### 5.2 布局契约（22 项）

| # | 契约 | 规则 |
|---|---|---|
| 1 | 应用壳高度 | `.app { height:100vh; display:flex; flex-direction:column }` |
| 2 | 整页不出现纵向滚动 | `body { overflow:hidden }` |
| 3 | 主区两列 | `grid-template-columns: minmax(0,1fr) var(--right-col)` |
| 4 | 右列独立滚动 | `.col-right { overflow-y:auto }` |
| 5 | 顶栏吸顶层级 | `.topbar { z-index:5 }` |
| 6 | 日志面板定高 | `.logpanel { height:var(--log-h) }` |
| 7 | 日志可折叠 | `.logpanel.collapsed { height:42px }` |
| 8 | 两张位置图铺满各自 wrap | `.plotwrap canvas { position:absolute; inset:0 }` |
| 9 | 矮屏媒体查询存在 | `@media (max-height:820px)` |
| 10 | 窄屏媒体查询存在 | `@media (max-width:1100px)` |
| 11 | 三卡片区定高 | `.cards3 { height:var(--hero-h) }` |
| 12 | X-Z 俯视图 `x>0` 画在左 | `px = cx - xn * …` |
| 13 | X-Y 正视图 `y>0` 画在上 | `py = cy - yn * …` |
| 14 | 无目标提示（两图 + 位置卡） | 文案「目标位置未就绪」≥3 处 |
| 15 | 触发判定严格小于 | `telemetry.z < th` |
| **16** | 动画层覆盖整个视口 | `.anim-layer { position:fixed; inset:0 }` |
| **17** | 动画层不拦截交互 | `.anim-layer { pointer-events:none }` |
| **18** | 动画层高于顶栏 | `.anim-layer { z-index:9000 }`（`.topbar` 是 5） |
| **19** | reduced-motion 媒体查询存在且关掉拖尾 | `@media (prefers-reduced-motion: reduce) { … .anim-trail { display:none } }` |
| **20** | 方向表 4 条齐备且轴正确 | `'1'/'−1' → axis:'x'`、`'2'/'−2' → axis:'y'` |
| **21** | 时长/淡出/去重窗口写在 `CFG.anim` | `durationMs:2500`、`fadeMs:300`、`sameEventWindowMs:300` |
| **22** | 手掌是内联 SVG 路径，零素材依赖 | `HAND_PATH_D` 存在；无 `<image>`；JS 里无 emoji 码位 |

### 5.3 交互契约（4 项）

桩环境直接调用页面导出的 `toggleLogPanel()` / `applyShowLog()`，然后读 DOM 状态：

| # | 操作 | 期望 | 实测 |
|---|---|---|---|
| 1 | 调一次 `toggleLogPanel()` | `.logpanel` 带 `collapsed` 类；按钮文案变「展开日志」 | ✅（`cls=logpanel collapsed`） |
| 2 | 再调一次 | `collapsed` 类移除；按钮文案回「收起日志」 | ✅ |
| 3 | `applyShowLog(false)`（模拟取消勾选"显示原始日志"） | 面板 `hidden=true` 且折叠按钮 `disabled=true` | ✅ |
| 4 | `applyShowLog(true)` | 面板恢复显示、按钮可用 | ✅ |

（折叠会影响位置图高度：两个函数末尾都会调 `fitCanvas()` 重算画布尺寸。）

### 5.4 尺寸链（核对用）

```
--right-col:372   --hero-h:128   --plot-min:148   --log-h:176(--log-h-min:132)   --gap:12
矮屏(≤820px)：--hero-h:112 --plot-min:132 --log-h:148；动作字 40→33px，坐标 19→16px，触发 32→26px
窄屏(≤1100px)：单列堆叠，.cards3 一列，两图各 210px 高，页面允许自身滚动
```

**高度预算（1280×720）**：顶栏 ~150 → 主区可用 ~544 → 卡片 112 + 日志 148 + 间隙 24
→ **位置图 ≈ 260 px**（> `--plot-min` 132，不会被压扁）。
**1920×1080**：卡片 128 + 日志 176 + 间隙 24 → 位置图 ≈ 590 px。

---

## 6. 与 level.html / track.html 的一致性

| 项 | level.html | gesture.html |
|---|---|---|
| 应用壳 | `height:100vh` flex column | 同 |
| 顶栏 | 三行（品牌/状态条/控件）+ `z-index:5` | 同（状态条 10 个 chip） |
| 主区 | `grid` 两列 + `--right-col` | 同（372 px） |
| 左列 | 大字读数 / 图 / 定高日志 | 三卡片 / 两张图 / 定高日志 |
| 右列 | 独立滚动 5 面板 + 脚注 | 同（连接参数/阈值/显示选项/统计/自测） |
| 设计 token | `--fg/--sub/--line/--bg/--brand/--ok/--warn/--err`、`.chip`、`.tag`、`.switch`、圆角 12 | 逐字复用 |
| 自测 | 面板按钮 + 启动静默跑 | 同（**17 条**，比 level 多 4 条动画纯函数用例） |
| 动画层 | 无 | **独有**：全视口 `pointer-events:none` 浮层 + 内联 SVG 手掌 + 动画锁定期（level/track 都没有） |
| 只读声明 | 有（但 level 保留显式写入） | **更彻底：完全没有写入路径** |

差异（有意）：本页无"下发配置"面板（设备无命令接口）；触发卡片配色为绿（任务书 D4）。

---

## 7. 上板验收清单（需 EDK60_VA + EDV163 挥手固件 + Chrome/Edge）

### 7.1 上板前 30 秒（先确认环境，不要先怀疑解析）

| # | 检查 | 期望 | 不通过怎么办 |
|---|---|---|---|
| A1 | EDV163 模组插在 **H3**（`RES A R T G V P`），不是 H4 | 模组供电灯亮 | 插错 H4 会导致电源与数据互相干扰（见工程 AGENTS.md 第 1 节） |
| A2 | 固件是挥手版（`radar_bridge_edv163_gesture`），不是水位/体征版 | 串口助手看到 `GESTURE:` 文本行 | 烧错 demo 就换固件 |
| A3 | 浏览器 Chrome/Edge ≥89，`navigator.serial` 可用 | 点「连接串口」会弹串口选择框 | 换浏览器；或先用「模拟演示」确认页面本身正常 |
| A4 | 波特率选 **115200** | 「行率」≈ **5.0 行/秒** | 行率明显不对（0.5 或乱码坏行爆表）→ 先怀疑波特率，再试 230400/460800/921600 |

### 7.2 状态条（决定性证据）

| # | 项 | 期望 | 异常含义 |
|---|---|---|---|
| B1 | 连接状态 | 绿点 +「已连接」+ 串口 USB VID:PID | 灰点=没打开 |
| B2 | **行率** | 4.0~6.0 行/秒（5±20%） | 远低于此：固件节拍不同或丢行 |
| B3 | **坏行** | 恒为 0（偶发 ≤1%） | 持续增长 → 流里混了非协议文本（固件 printf？） |
| B4 | **跳过二进制帧** | ≥0；若这版固件发点云则应 >0 并稳定增长 | 恒 0 也可能是这版不发点云（属正常，记 U1） |
| B5 | **重同步** | 0 或很小；开启后随二进制帧增多略有增长但不刷屏 | 与 B4 同量级暴涨 → 帧格式/长度域理解有偏差 |
| B6 | **最后一行时间** | 约每 200 ms 跳一次 | 停住 → 断流（看是否触发"遥测超时"标签） |

### 7.3 挥手动作（U2 的关键验证）

| # | 动作 | 期望 | 记录 |
|---|---|---|---|
| C1 | 在雷达前 `z ≈ 0.2~0.8 m` 正对，**向左挥 10 次** | 每次都显示「左挥手」（绿 `#16a34a`） | 次数 = /10 |
| C2 | 向右挥 10 次 | 「右挥手」（蓝 `#2563eb`） | /10 |
| C3 | 向上挥 10 次 | 「上挥手」（橙 `#ea580c`） | /10 |
| C4 | 向下挥 10 次 | 「下挥手」（深蓝 `#0284c7`） | /10 |
| C5 | 停手 3 s | 回「无挥手」（灰） | — |
| C6 | 手掌沿 Z 轴靠近/远离（不是挥手） | **动作大字不变**（仍「无挥手」），"靠近/远离"小字计数增长 | ±3 是否出现（记 U3） |
| C7 | 右列直方图 | 五行计数与上面实际次数对得上；±3 两行不在挥手方向里 | — |
| C8 | 出现任何非四向的动作名（如「未知挥手」） | 记录当时的 `action=` 值 | 说明编码表要补（记 U2） |

> ⚠ 若四向**全部反了**（左显示右、上显示下），说明 `action` 符号约定与源码表格不一致——
> 这属于**固件/协议**问题，不要在网页端改映射，先回填 U2 由硬件/嵌入式确认。

### 7.4 目标位置与两张图（U1/U4）

| # | 项 | 期望 |
|---|---|---|
| D1 | 有人站进检测区 | X/Y/Z 三位小数开始跳动；tag 变 `target=1`/「位置有效」 |
| D2 | **X-Z 俯视图**：把目标往雷达**右侧**移动（站在雷达的右手边） | 红点向**图左**移动（`x>0` 显示在左），Z 越大红点越靠上 |
| D3 | Z 距离从 0.3 m 走到 0.8 m | 红点沿中轴向上移动，接近顶部（1.00 m 刻度） |
| D4 | **X-Y 正视图**：把手抬高 | 红点向**上**（`y>0`）；向左移 → 红点向**左** |
| D5 | 目标离开视野 | 两图各自显示「目标位置未就绪」，坐标变 `—`，tag 变 `target=0` |
| D6 | 记录 `target=1→0` 的耗时 | 与 U4 的"10 帧位置窗"说法对照（应为数百 ms 量级） |
| D7 | 坐标数量级是否合理 | `z` 应在 0.1~1.0 m 量级；若出现几十米/负数巨值 → 单位或字段错位，立即记录原行 |

### 7.5 距离触发（阈值 20.0 cm）

| # | 操作 | 期望颜色/文案 |
|---|---|---|
| E1 | 手停在 `z ≈ 0.30 m` | 「未触发」灰 |
| E2 | 手缓慢前移穿到 `z ≈ 0.15 m` | 变「触发」**绿** |
| E3 | 手退回 0.30 m | 回「未触发」灰 |
| E4 | 阈值改 50 → 手在 0.40 m | 变「触发」 |
| E5 | 阈值改 0 / 空 / `150` / `abc` | 分别：永不触发 / 回退 20.0 / 夹到 100 / 回退 20.0（三个输入框同步） |
| E6 | 手离开视野（`target=0`） | 即使阈值很大也「未触发」（`target=0` 不参与触发） |

### 7.6 日志与页面（约 3 分钟目检）

| # | 项 | 期望 |
|---|---|---|
| F1 | 日志面板 | 深底等宽；每行有时间戳；文本行青色、二进制帧/重同步/坏行红色、系统行绿色 |
| F2 | 二进制帧行 | 形如 `跳过二进制帧 #12  len=…  45 44 00 11 …` 并标 `[0x4544 · 校验和 OK]` |
| F3 | 「收起日志」 | 面板收到 42 px，两张位置图变高 |
| F4 | 右列「显示原始日志」取消勾选 | 日志块整块消失，位置图变高；再勾选恢复 |
| F5 | 右列「显示靠近/远离」取消勾选 | 动作卡片第三行小字消失（动作大字不受影响） |
| F6 | 右列可独立滚动 | 页面整体不滚动（`.app` 100vh），只有右列滚 |
| F7 | 1280×720 窗口 | 三卡片不换行、两张图仍 ≥130 px 高、日志不压掉位置图 |
| F8 | 断开 → 再连接 | 会话统计清零重新计；不残留旧的「遥测超时」标签 |
| F9 | 浏览器控制台 | 无红色报错（尤其 `CanvasRenderingContext2D` 相关） |
| F10 | 用 `file://` 打开 | 若 Web Serial 不可用，应看到黄条提示且「模拟演示」仍可跑 |

### 7.7 无硬件也能做的验收（模拟演示）

| # | 项 | 期望 |
|---|---|---|
| G1 | 勾「模拟演示（无需硬件）」 | 2 s 内动作大字在 左/右/上/下/无 之间跳，值颜色同步变化 |
| G2 | 「跳过二进制帧」 | 约每 1.8 s +1（9 拍 × 200 ms） |
| G3 | 「重同步」 | 约每 4.6 s +1（23 拍），日志出现红色「重同步」行，**坏行仍为 0** |
| G4 | 触发卡片 | 会周期性在「触发/未触发」之间切换（`z` 周期约 7 s） |
| G5 | 自我检查 | 点「运行帧解析自测」→ 结果框显示 `自测结果：17/17 通过` |
| G6 | 模拟与真串口互斥 | 模拟中「连接串口」置灰；连上串口后模拟开关自动弹回 |
| G7 | **挥手动画** | 动作大字一变，就有一个半透明手掌从对应方向的起点飞到终点，约 2.5 s；**动画期间点按钮、滚右列、拖窗口都正常**（浮层不拦鼠标） |
| G8 | **动画期忽略** | 动画播放中再换一个方向挥手（或等模拟流换向），右列「动画期忽略」+1，日志出现 `（动画播放中，忽略 action=X）`，动作大字**不跟着变** |
| G9 | **锁定期间解析不停** | 动画播放时看状态条：行率仍 ≈5 行/秒、有效行仍在涨、直方图仍在涨、坐标仍在跳 |
| G10 | **动画不叠加** | 连续挥手时屏幕上同时最多一个手掌；没有两个浮层叠在一起，也没有残留的浮层（2.5 s 后 DOM 里 `#gestureAnimLayer` 消失） |

### 7.8 上板时怎么快速验动画（30 秒）

| # | 操作 | 期望 |
|---|---|---|
| H1 | 向左挥 1 次 | 手掌**从屏幕左边飞到右边**，指尖朝右；约 2.5 s 后淡出消失 |
| H2 | 向右挥 1 次 | 手掌**从右飞到左**，指尖朝左 |
| H3 | 向上挥 1 次 | 手掌**从屏幕下侧升到上侧**，指尖朝上 |
| H4 | 向下挥 1 次 | 手掌**从屏幕上侧落到下侧**，指尖朝下 |
| H5 | 动画还没播完就反方向挥 | 屏幕上的手掌**不换向、不重启**；「动画期忽略」+1；日志有 `（动画播放中，忽略 action=X）` |
| H6 | 动画没播完时看状态条 | 行率、有效行、坏行、坐标、直方图**全部照常跳**（这是"锁只锁展示"的证据） |
| H7 | 动画播完后停手 3 s | 动作大字回「无挥手」；DOM 里不留 `#gestureAnimLayer` |
| H8 | 系统开启「减弱动态效果」后刷新页面再挥手 | 手掌**静止出现在屏幕正中**，约 0.8 s 淡入淡出，不位移 |

> 若 H1~H4 的**方向反了**（左挥手却从右往左），先确认 `action` 编码本身（U2，见 7.3 的表）——
> 方向表是用户拍板的约定，网页端按 `action` 值查表，不猜物理方向。

---

## 8. 复现命令（逐字可执行）

环境：Windows + PowerShell；Node 用的是项目里现成的临时 Node 20（不在 PATH 上）。
以下命令任一条都可直接粘贴执行。

### 8.1 语法校验（Node `--check`）

```powershell
$node = "C:\Users\fuhua\AppData\Local\Temp\docusaurus-node20\node-v20.19.5-win-x64\node.exe"
$src  = "C:\Users\fuhua\sync\easydetek\0.project\OK\20261014-ed_edk60_demos\ed_edk60_demos\release\gesture.html"

# 抽出页内唯一的 <script> 块 → 落成临时 .js → 语法检查
$t = [System.IO.File]::ReadAllText($src)
$i = $t.IndexOf("<script>"); $j = $t.LastIndexOf("</script>")
[System.IO.File]::WriteAllText("$env:TEMP\gesture_extract.js",
  $t.Substring($i + 8, $j - ($i + 8)), (New-Object System.Text.UTF8Encoding($false)))
& $node --check "$env:TEMP\gesture_extract.js"
Write-Output "exit=$LASTEXITCODE"      # 期望：exit=0
```

### 8.2 桩环境自测（页内 17 条 + 模拟管线 13 项 + 阈值 5 项 + UI 交互 4 项 + 动画/锁定期 21 项 + 静态 38 项 = 98 项）

harness 是**临时脚本**（不随交付物入库），正文见 **附录 A**。先落盘：

```powershell
$dir = "$env:TEMP\gesture_selftest"
New-Item -ItemType Directory -Force -Path $dir | Out-Null
# 把附录 A 的整段代码保存为： $dir\stub-harness.js
```

然后执行：

```powershell
$node = "C:\Users\fuhua\AppData\Local\Temp\docusaurus-node20\node-v20.19.5-win-x64\node.exe"
$src  = "C:\Users\fuhua\sync\easydetek\0.project\OK\20261014-ed_edk60_demos\ed_edk60_demos\release\gesture.html"

& $node "$env:TEMP\gesture_selftest\stub-harness.js" $src
Write-Output "exit=$LASTEXITCODE"      # 期望：exit=0，末行「结论：PASS」
```

harness 做的事（按顺序）：

1. 抽出页内 `<script>`，在 `vm` 沙箱里用 **DOM / Canvas / Web Serial / rAF 桩**执行（页面 `boot()` 自动跑一遍静默自测）；
2. 显式调用页面导出的 `runSelfTest(false)`，逐条打印 C1~C17；
3. 用页面导出的 `buildSimChunk()` + `feedInbound()` 直接驱动 60 拍模拟流，断言统计/直方图/触发/画布
   （每拍先用**可控时钟**把 rAF 队列推进 200 ms，动画才会自然收尾）；
4. 用 `applyThreshold()` 验 5 条归一化（越界夹取 / 回退 / 三框同步）；
5. 用 `toggleLogPanel()` / `applyShowLog()` 验 4 条 UI 交互（折叠类名、按钮文案、隐藏与置灰）；
6. 用 `window.__gestureAnim` + 手动推帧验 21 条动画/锁定期断言（方向、锁定忽略、解锁、6 连帧去重、
   锁定期解析不停、`pointer-events:none`、reduced-motion、调试钩子、0/±3 不触发）；
7. 对源码做 strip 注释后的静态不变量与布局契约检查（38 条）。

### 8.3 页面内自测（有浏览器时）

直接用 Chrome/Edge 打开 `release/gesture.html`（`file://` 一般亦可），
点右上「运行帧解析自测」或右列「运行帧解析自测」，结果框首行应为
`自测结果：17/17 通过`；日志里同步出现同样一行。
**启动时不会放动画**：C14~C17 全是纯函数（`animPlan/animClassify/animEase/animOpacity`），不建 DOM 不开定时器。

### 8.4 本次实际运行结果（原文摘录）

```
== 一、页内帧解析自测 ==
  [通过] C1 … [通过] C17
  自测结果：17/17 通过
== 二、模拟演示管线（60 拍 × 200 ms，经 buildSimChunk → feedInbound）==
  [通过] 模拟共 60 行、60 条有效行、0 条坏行  (valid=60 bad=0)
  [通过] 跳过二进制帧 ≥ 6（每 9 拍一帧合法 0x4544）  (binFrames=7)
  [通过] 重同步 ≥ 2（每 23 拍一帧校验错的 0x4544）  (resync=3)
  [通过] 被二进制残留污染的行全部靠文本层重同步救回（坏行仍为 0）  (trimmed=3 bad=0)
  [通过] action 直方图总数 = 有效行数  (sum=60 valid=60)
  [通过] ±3 只进"靠近/远离"计数，不进动作直方图  (proxPos=2 proxNeg=2)
  [通过] 动作大字出现过 ≥3 种（锁定期冻结显示，解锁后随最新遥测刷新）  (seen=["无挥手","左挥手","下挥手","右挥手"])
  [通过] 触发卡片两种状态都出现过（穿过 20 cm 阈值）  (触发=14 未触发=45)
  [通过] 收字节数 > 0  (rxBytes=3343)
  [通过] 状态条数字与内部统计一致  (st-valid=60 st-bin=7)
  [通过] 位置图 canvas 拿到 2D 上下文并发生绘制  (drawCalls=25778)
  [通过] 日志出现"跳过二进制帧"与"文本行"两类标注
  [通过] 直方图表格被填充（≥5 行）  (rows=7)
  探测点：[{"tick":0,"bin":0,"valid":1,"action":"无挥手","trig":"未触发"},{"tick":12,"bin":1,"valid":13,"action":"左挥手","trig":"未触发"}]
  [通过] 阈值 150 → 夹到 100  (now=100)
  …（阈值归一化共 5 条，全部通过）
  [通过] 「收起日志」→ .logpanel 带 collapsed 类，按钮文案变「展开日志」  (cls=collapsed btn=展开日志)
  …（UI 交互共 4 条，全部通过）
== 二之四、挥手动画层与动画锁定期 ==
  [通过] ① 触发 action=1：浮层出现且 isLocked() 为真  (layer=anim-layer locked=true)
  [通过] ① 方向为「屏幕左侧 → 屏幕右侧」（横向 x，8% → 92%，y 居中 50%）  (axis=x from=8%/50% to=92%/50%)
  [通过] ① 起点 transform 落在视口左侧 8%（1440×900 → x≈115.2px, y≈450px）  (translate3d(115.20px,450.00px,0) rotate(90.00deg) scale(1))
  [通过] ① 2200 ms（位移段结束）后到 92%（x≈1324.8px）  (xPct=92 tf=translate3d(1324.80px,450.00px,0) rotate(90.00deg) scale(1))
  [通过] ① 最后 300 ms 淡出：t=2350 时 opacity≈0.5，t=2500 后解锁且浮层被移除  (opacity@2350ms=0.5)
  [通过] ② 锁定期内 action=-1：不重启动画（runId 不变、仍在播左挥手）  (runId=6 action=1)
  [通过] ② skippedDuringAnim 由 0 变 1，日志记「（动画播放中，忽略 action=-1）」  (skipped=1)
  [通过] ② 动作大字没有被切换成「右挥手」（仍是触发时的「左挥手」）  (h-action=左挥手 tag=动画播放中)
  [通过] ③ 解锁后可触发下一次，且播的是新触发的 action=-1（右→左）  (action=-1 from=92% to=8%)
  [通过] ③ 锁定期内被忽略的 -1 没有"补播"痕迹：skipped 仍为 1，lastRun 是左挥手  (skipped=1 lastRun=1)
  [通过] ④ 同一 action=2 的 6 连帧（间隔 200 ms）只启动 1 次动画  (starts=1 skipped=0)
  [通过] ④ 6 帧里 5 帧被判为残留帧（residualFrames=5），且不计入 skipped  (residual=5)
  [通过] ⑤ 锁定期内有效行 +1、坏行 +1、行率 >0（解析与统计完全没停）  (valid=67→68 bad=0→1 rate=1.4 行/秒)
  [通过] ⑤ 锁定期内 action=1 仍照常计数（直方图数据 +1），但动作大字仍冻结在「上挥手」  (actions[1]=9→10 h-action=上挥手)
  [通过] ⑤ 「动画期忽略」计数在会话统计里可见（=1）  (ss-skip=1)
  [通过] ⑥ 浮层内联 pointer-events:none（覆盖整个视口 position:fixed;inset:0）  (inline=none position=fixed)
  [通过] ⑥ CSS 里 .anim-layer 同样是 pointer-events:none 且 z-index 高于顶栏（>5）  (zIndex=9000)
  [通过] ⑥ 手掌是内联 SVG（createElementNS 生成，无外链、无 emoji）  (d 子路径数=6)
  [通过] ⑦ prefers-reduced-motion：durationMs=800、自始至终居中不位移、结束后解锁  (reduced=true dur=800 xPct@400ms=50)
  [通过] ⑧ __gestureAnim.trigger() 锁定期内与遥测同语义（返回 false 且 skipped+1）；force 才重入  (first=false skipped=0→1 force=true)
  [通过] ⑨ action=0 与 ±3 不触发动画（runId 不变、浮层不存在、skipped 仍为 0）  (runId=11 skipped=0)
== 三、静态不变量与布局契约 ==
  …（共 38 条，全部通过）
== 汇总 ==
  页内帧解析用例：17/17
  模拟管线断言  ：13/13
  阈值归一化断言：5/5
  UI 交互断言   ：4/4
  动画/锁定期断言：21/21
  静态不变量/契约：38/38
  合计           ：98/98
  结论           ：PASS
```

---

## 9. 未覆盖项与说明

| # | 未覆盖 | 原因 | 谁补 |
|---|---|---|---|
| N1 | **真实串口 + 真实固件的端到端**（波特率、行率、`action` 编码方向、二进制帧占比、`target=0` 时延） | 无硬件在环；沙箱禁 Chromium | 上板（第 7 节清单）→ 回填设计文档 U1~U7 |
| N2 | 真浏览器**渲染观感**（字体、颜色、1280×720 是否挤、右列滚动条位置） | 同上 | 用户目检 F1~F10 + G1~G10 + H1~H8 |
| N3 | `navigator.serial` 的**权限拒绝 / 拔线 / 端口被占用**路径 | 桩环境无 Web Serial 对象，只走了"不支持"分支 | 上板时故意拔线一次，看日志是否给出「读取异常」并允许重连 |
| N4 | 极长会话（>1 h）内存与日志裁剪 | 桩跑 60 拍；日志 400 行上限与 `lineTimes` 600 上限是代码级保证 | 上板挂 1 h 看页签内存 |
| N5 | 窄屏（<1100px）双列堆叠的真实观感 | 同上 | 手机/半屏目检 |
| **N6** | **动画的真实观感**：手掌大小/位置/颜色是否好看、2.5 s 是否偏慢、拖尾是否明显、SVG 轮廓像不像手 | 沙箱禁 Chromium，**看不了渲染结果**；桩环境只能断言"位移到了哪个百分比、透明度是多少" | 项目经理截图（§11.2 的调用序列）→ 用户目检 H1~H8 → 需要时改 `CFG.anim.sizeRatio` / `trailCount` / `wobbleDeg` / `durationMs` |
| **N7** | 后台标签页 / 低帧率下的动画时序 | `requestAnimationFrame` 在后台标签页会被节流甚至暂停 → 动画"慢放"、锁定期跟着变长 | 属浏览器行为，不修。切走再切回时动画可能停在中途，下一次触发即恢复（H7 可覆盖） |
| **N8** | 多显示器 / 系统缩放（125%、150%）下的手掌尺寸与边缘留白 | 桩环境 `innerWidth/innerHeight` 是死值 | 目检：手掌不贴边、不越界（边距按百分比算，与缩放无关；尺寸按 `min(宽,高)` 算，会被 `maxSizePx` 夹住） |

**测试桩的已知边界**（v1.1 更新）：`requestAnimationFrame` 不再是空实现，而是**队列 + 手动推帧**
（`flushFrame(ts)` / `stepTo(target)`），时钟由 harness 自己推进（`fakeNow`）。
这么改有两个原因：①动画要靠 rAF 推进，空实现会让锁定期永远解不开；
②虚拟时间让"2.5 s 动画 / 300 ms 淡出 / 300 ms 去重窗口"这类时序断言**完全确定性**，
不受机器快慢影响。真实浏览器里由 `requestAnimationFrame(loop)` 驱动，逻辑是同一套函数。

---

## 10. 本轮发现并修复的缺陷（自测价值记录）

| # | 缺陷 | 触发方式 | 修法 |
|---|---|---|---|
| 1 | **自测夹具没把字符串转字节**：`build()` 型用例直接返回字符串，`feed()` 里 `str[i] & 0xFF` 全变 0 → C9 永远 0 行（假失败，掩盖真实行为） | 第一次跑 harness，C9 期望 1 行实际 0 行 | `runSelfTest` 里加 `asBytes()`，`chunks` 与 `build()` 两条路都归一化成 `Uint8Array` |
| 2 | **C13 只读不变量误报**：用子串匹配找 `set`，把 `parseTelemetryLine` 里的 "pars**ete**lemetry" 判成"含 set 的写接口" | 同上，C13 第 1 条断言失败 | 改成"方法名完全等于 / 以 write\|send\|transmit **结尾**"判定 |
| 3 | **阈值越界语义与源码不一致**：初版对 `150` 直接回退 20.0，源码是**夹到 100** | harness 阈值断言 1（`150 → 100`）失败 | 新增 `normalizeThreshold()`：非数字/空 → 回退 20.0；越界 → 夹到 0~100；并在提示里区分两种情形 |
| 4 | 静态检查误报（harness 自身缺陷）：注释里写"不写 localStorage"被判违规 | 第三区 2 条失败 | harness 先剥 HTML 注释与 JS 注释/字符串字面量，再断言；并把"全文出现次数 vs 代码出现次数"一并打印，避免以后被当成漏检 |

前 3 条是**页面/自测代码的真实缺陷**，第 4 条是**校验工具**的缺陷。
这也说明"页内自测 + 桩环境外测"两条腿都得有：页内自测抓不到"夹具写错"，外测抓不到"没上真硬件"。

### 10.1 v1.1（动画层）这一轮抓到的坑

| # | 现象 | 根因 | 处置 |
|---|---|---|---|
| 5 | 桩环境里一触发动画就**永久锁定**，之后所有动作都被忽略，模拟管线的"动作大字≥3 种"直接挂掉 | harness 的 `requestAnimationFrame` 是空实现（v1.0 为了让主循环不空转），动画拿不到帧 → `animFrame` 永不推进 → 到不了 `durationMs` | harness 改成**队列 + 手动推帧**（`flushFrame` / `stepTo`），并在模拟循环里按虚拟时间推进。顺带得到一个副作用：动画时序现在完全是确定性的，可逐帧断言 |
| 6 | 断言"锁定期内有效行 +1"第一次跑挂了，但其实页面是对的 | 行率是 5 s 滑窗，`state.lineTimes` 的**裁剪是懒执行**的（在 `lineRate()` 里才 `shift`），跨越大段虚拟时间后数组长度还是旧值 | 断言改成"先 `updateStatusBar()` 让窗口收敛，再取基线长度"，并直接断言 `ss-rate` 文本 |
| 7 | `layer.style.setProperty` 在桩环境报 `TypeError` | v1.0 的 style 桩是普通对象，没有 `setProperty`（浏览器里一定有） | 桩的 style 换成带 `setProperty/getPropertyValue` 的 Proxy；页面代码不用降级写法（浏览器原生支持） |

第 5 条是**测试工具**的缺陷，但它顺带证明了一件事：**动画永远不会"卡在锁定态"** ——
一旦没有帧可推，页面里的兜底分支（无 rAF 且无 `setTimeout`）会直接 `animFinish()` 收尾。
第 6、7 条也是工具缺陷；页面本轮**没有发现需要修的缺陷**。

---

## 11. 挥手动画层与动画锁定期（21 项，全通过）

> 这是 v1.1 新增的唯一功能。六个必验用例（①~⑥）来自需求，⑦~⑨是顺手补的边界。
> 全部在桩环境里用**可控时钟 + 手动推帧**跑（浏览器无需启动），每项开始前都重置
> 动画/去重状态（等价于"重新连接串口"）。

| # | 用例 | 断言 | 实测 |
|---|---|---|---|
| **①** | 触发 `action=1` → 浮层出现，方向 **左 → 右** | `trigger(1)` 返回 `true`；`#gestureAnimLayer` 在 DOM 里且 `class="anim-layer"`；`isLocked()=true`；`state().axis='x'`、`from=8%/50%`、`to=92%/50%` | ✅ `layer=anim-layer locked=true`；`from=8%/50% to=92%/50%` |
| **①-a** | 起止 `transform` 真的是视口坐标 | 起点 `translate3d(115.20px,450.00px,0)`（1440×900 的 8%/50%）；2200 ms 后 `translate3d(1324.80px,450.00px,0)`（92%） | ✅ 两个字符串逐字命中 |
| **①-b** | 末 300 ms 淡出后解锁并移除浮层 | `t=2350` 时 `opacity≈0.5`；`t=2500` 后 `isLocked()=false` 且 `#gestureAnimLayer` 不在 DOM | ✅ `opacity@2350ms=0.5` |
| **②** | 锁定期内再触发 `action=-1` → **不重启动画**、`skippedDuringAnim +1` | `runId` 不变、`state().action` 仍为 `1`、`skippedDuringAnim 0→1`、日志含 `（动画播放中，忽略 action=-1）`、`h-action` 仍是「左挥手」、来源 tag 变「动画播放中」 | ✅ `runId=6 action=1 skipped=1` |
| **③** | 动画结束后可正常触发下一次 | 解锁后 `trigger(-1)` 返回 `true`，新动画是 `action=-1`、`92% → 8%` | ✅ `action=-1 from=92% to=8%` |
| **③-b** | 锁定期内被忽略的方向**不补播** | 解锁后 `skipped` 仍为 1（没有补偿播放），`last()` 仍是"左挥手" | ✅ `skipped=1 lastRun=1` |
| **④** | 同一 action 的 **6 连帧只播 1 次** | 6 行 `action=2`（间隔 200 ms）→ `runId` 只 +1、`skipped=0`、`residualFrames=5` | ✅ `starts=1 residual=5` |
| **⑤** | 锁定期内 **行率/有效行仍在增长**（证明解析没被暂停） | 锁定期内喂一行 `GESTURE`（`action=1`）+ 一行 `hello world` → `validLines +1`、`badLines +1`、`lineTimes +1`、`ss-rate > 0`；`actions['1'] +1`（直方图数据照常）；而 `h-action` 仍冻结在「上挥手」 | ✅ `valid=67→68 bad=0→1 rate=1.4 行/秒`；`actions[1]=9→10`，`h-action=上挥手` |
| **⑤-b** | 「动画期忽略 N」在会话统计里可见 | `#ss-skip` 文本为 `1`（本次同向 action=1 被忽略） | ✅ `ss-skip=1` |
| **⑥** | `pointer-events:none` 断言 | 浮层内联 `style.pointerEvents === 'none'` 且 `position:fixed`；CSS 里 `.anim-layer` 同时有 `pointer-events:none`、`inset:0`、`z-index:9000` | ✅ `inline=none position=fixed zIndex=9000` |
| **⑥-b** | 手掌是**内联 SVG** | `#gestureAnimHand` 的 `svg > path` 的 `d` 含 6 个子路径（掌+4 指+拇）、无 `http`/`url(`、无 emoji | ✅ `d 子路径数=6` |
| **⑦** | `prefers-reduced-motion` → 静态 + 淡入淡出 800 ms（不位移） | `reduced=true`、`durationMs=800`、`from=to=50%/50%`、400 ms 时 `xPct` 仍为 50、800 ms 后解锁 | ✅ `dur=800 xPct@400ms=50` |
| **⑧** | `__gestureAnim.trigger()` 与遥测**同语义**；`force` 才重入 | 锁定期内 `trigger(-2)` 返回 `false` 且 `skipped+1`；`trigger(-2,{force:true})` 返回 `true` 且 `runId+1`、`action=-2` | ✅ `first=false skipped=0→1 force=true` |
| **⑨** | `action=0` 与 `±3` 不触发动画 | `runId` 不变、浮层不存在、`skipped` 仍为 0 | ✅ |

**这 21 项为什么能证明"解析没被暂停"**：断言 ⑤ 走的是**真实管线**
（`feedInbound(bytes)` → `GestureStreamParser.feed` → `applyLine`），不是直接改 UI；
它在锁定期内同时验证了 5 个互相独立的计数器（有效行、坏行、行率窗口、action 直方图数据、
`ss-skip`），并且验证了**唯一被冻结的那个**（动作大字）。

### 11.1 与需求原文的逐条对照

| 需求原文 | 实现 | 证据 |
|---|---|---|
| 左挥手 → 手掌从左到右，2~3 s | `CFG.animDirs['1'] = {axis:'x', from:[-1,0], to:[1,0], rotate:90}`，`durationMs=2500` | C14、①、①-a |
| 其他方向一样处理 | 方向表 4 条：`-1` 右→左、`2` 下→上、`-2` 上→下 | C14（10 条断言） |
| 期间忽略任何回收状态更新 | `state.animLock=true` 起，`updateHero()` 里动作大字整块 gate；新事件走 `animRequest()` → 只 +`skipped` + 一行日志 | ②、③-b、⑤ |
| 动画时间可调 | `CFG.anim = { durationMs: 2500, fadeMs: 300, marginPct: 8, … }` | C16 末两条断言 |
| 覆盖整个视口、不遮交互 | `.anim-layer{position:fixed;inset:0;pointer-events:none;z-index:9000}` | ⑥、⑥-b、静态 5.2 第 16~18 项 |
| 内联 SVG 手掌、透明 + 柔光、拖尾 | 一个 `<path>` 6 个子路径、`rgba` 填充/描边、`drop-shadow`、4 条拖尾 | ⑥-b |
| 缓动 `ease-in-out` / `cubic-bezier(.4,0,.2,1)` | 牛顿迭代实现 `cubic-bezier(.4,0,.2,1)` | C16（单调 + 端点 + 中点） |
| 起终点留边距 | `marginPct=8` → 8%/92% | C14、① |
| 结束时淡出后移除浮层 | `fadeMs=300`，到点 `animFinish()` → `removeChild` | ①-b |
| 解析与计数照常 | 无任何 gate | ⑤（5 个计数器同时增长） |
| 忽略的累加到 `skippedDuringAnim` + 日志 | `stats.skippedDuringAnim++`、`（动画播放中，忽略 action=X）` | ②、⑤-b |
| 锁定结束恢复；不补播 | `animFinish()` 回调 `updateHero()` 拉回最新遥测 | ③、③-b |
| 同一次检测 6 连帧只播 1 次 | `animClassify()` + `sameEventWindowMs=300` | C15、④ |
| reduced-motion → 静态 800 ms | `animPlan(code,{reduced:true})` | C17、⑦ |
| 调试钩子 `{trigger, isLocked}` | `window.__gestureAnim` | ⑧ |

### 11.2 给截屏脚本的推荐调用序列

```js
// 1) 单个方向的定格截图（不等 2.5 s，直接推到想要的时刻）
__gestureAnim.trigger(1);                 // 左挥手：屏幕左侧 → 右侧
__gestureAnim._frame(performance.now() + 1100);   // 手动推帧到 ~44%（手掌在中间偏右）

// 2) 四个方向各来一张：先收尾再触发下一个
['1', '-1', '2', '-2'].forEach(code => {
  __gestureAnim.finish();                 // 立刻解锁并移除浮层
  __gestureAnim.trigger(code, { force: true });
  __gestureAnim._frame(performance.now() + 1200);
  // … 此处截图 …
});

// 3) 锁定语义的对照截图（同一帧里应看到：浮层在播、动作大字定格、有效行/行率仍在跳）
__gestureAnim.trigger(1);
__gestureAnim.isLocked();                 // → true
__gestureAnim.state();                    // → { action:1, axis:'x', phase:'move', xPct:…, skipped:N }

// 4) 无障碍版本（静态居中 + 800 ms 淡入淡出）
__gestureAnim.trigger(2, { force: true, reduced: true });
__gestureAnim._frame(performance.now() + 400);    // 居中、opacity=1
```

> 真实浏览器里 `_frame()` **不需要调用**：`requestAnimationFrame` 自己在跑，`trigger()` 之后
> 等 1~1.2 s 截图即可（手掌大约在屏幕中部）。`_frame()` 只是给 Node 桩环境和"要精确某毫秒"的
> 截图脚本用的。

---

## 附录 A：桩环境 harness 全文

> 保存为 `%TEMP%\gesture_selftest\stub-harness.js` 后按 8.2 执行。
> 依赖：Node 18+（只用 `fs` / `vm` 两个内置模块，无需安装任何包）。
> v1.1 变更：`requestAnimationFrame` 改成**队列 + 手动推帧**（`flushFrame` / `stepTo`），
> 时钟由 harness 自身推进；新增「二之四」的 21 条动画/锁定期断言与 8 条动画层静态契约。

```javascript
/* eslint-disable */
/*
 * gesture.html 桩环境自测 harness（不启动浏览器）
 *
 * 用法：
 *   node stub-harness.js <gesture.html 的绝对路径>
 *
 * 做的事：
 *   1) 抽出页面内唯一的 <script> 块，在 vm 沙箱里用 DOM/Canvas/Web Serial/rAF 桩执行；
 *      页面 boot() 会自动跑一遍静默自测。
 *   2) 再显式调用页面的 runSelfTest(false)，逐条打印 C1~C17。
 *   3) 用 buildSimChunk()+feedInbound() 驱动"模拟演示"那条管线 60 拍，
 *      断言 有效行 / 跳过二进制帧 / 重同步 / 直方图 / 触发卡片 都能被驱动。
 *   4) 阈值归一化 5 条 + UI 交互 4 条。
 *   5) 挥手动画层与锁定期 6 项（方向 / 锁定忽略 / 解锁 / 去重 / 解析不停 / pointer-events）
 *      + prefers-reduced-motion + 调试钩子。
 *   6) 静态不变量与布局契约（含动画层新增契约）。
 */
'use strict';
const fs = require('fs');
const vm = require('vm');

const htmlPath = process.argv[2];
if (!htmlPath) { console.error('usage: node stub-harness.js <path-to-gesture.html>'); process.exit(2); }
const html = fs.readFileSync(htmlPath, 'utf8');

/* ---------------- DOM 桩 ---------------- */
const elements = new Map();
let reduceMotion = false;

function makeCtx2d() {
  const calls = { total: 0 };
  const handler = {
    get(t, p) {
      if (p in t) return t[p];
      if (p === 'measureText') return () => ({ width: 24 });
      if (p === 'canvas') return t.canvas;
      const fn = function () { calls.total++; };
      t[p] = fn;
      return fn;
    },
    set(t, p, v) { t[p] = v; return true; }
  };
  return new Proxy({ calls: calls }, handler);
}

function makeStyle() {
  const own = { _vars: {} };
  return new Proxy(own, {
    get(t, p) {
      if (p === 'setProperty') return (k, v) => { t._vars[k] = String(v); };
      if (p === 'getPropertyValue') return k => t._vars[k] || '';
      if (p === 'removeProperty') return k => { delete t._vars[k]; };
      if (p in t) return t[p];
      return undefined;
    },
    set(t, p, v) { t[p] = v; return true; }
  });
}

class El {
  constructor(tag, id) {
    this.tagName = (tag || 'div').toUpperCase();
    this.id = id || '';
    this._className = '';
    this.style = makeStyle();
    this.dataset = {};
    this._attrs = {};
    this._text = '';
    this._children = [];
    this.parentNode = null;
    this.hidden = false;
    this.disabled = false;
    this.checked = false;
    this.value = '';
    this.scrollTop = 0;
    this.scrollHeight = 0;
    this.width = 0;
    this.height = 0;
    this._ctx = null;
    this._listeners = {};
    const self = this;
    this.classList = {
      add(c) { const s = new Set(self._className.split(/\s+/).filter(Boolean)); s.add(c); self._className = [...s].join(' '); },
      remove(c) { const s = new Set(self._className.split(/\s+/).filter(Boolean)); s.delete(c); self._className = [...s].join(' '); },
      contains(c) { return self._className.split(/\s+/).includes(c); },
      toggle(c, on) { if (on === undefined) { this.contains(c) ? this.remove(c) : this.add(c); } else if (on) this.add(c); else this.remove(c); }
    };
  }
  get className() { return this._className; }
  set className(v) { this._className = String(v || ''); }
  get textContent() { return this._text; }
  set textContent(v) { this._text = String(v == null ? '' : v); this._children.length = 0; }
  get innerHTML() { return this._text; }
  set innerHTML(v) { this._text = String(v == null ? '' : v); this._children.length = 0; }
  get childNodes() { return this._children; }
  get firstChild() { return this._children[0] || null; }
  setAttribute(k, v) { this._attrs[k] = String(v); if (k === 'id') this.id = String(v); }
  getAttribute(k) { return (k in this._attrs) ? this._attrs[k] : null; }
  appendChild(node) {
    if (node && node._isFragment) { node._children.forEach(c => { c.parentNode = this; this._children.push(c); }); return node; }
    if (node) node.parentNode = this;
    this._children.push(node);
    this._text += (node && node.textContent) ? node.textContent : '';
    return node;
  }
  removeChild(node) {
    const i = this._children.indexOf(node);
    if (i >= 0) this._children.splice(i, 1);
    if (node) { node.parentNode = null; if (node.textContent) this._text = this._text.replace(node.textContent, ''); }
    return node;
  }
  addEventListener(type, fn) { (this._listeners[type] = this._listeners[type] || []).push(fn); }
  dispatch(type, ev) { (this._listeners[type] || []).forEach(fn => fn(ev || { target: this, key: '' })); }
  getBoundingClientRect() { return { x: 0, y: 0, top: 0, left: 0, width: 520, height: 220 }; }
  getContext() { if (!this._ctx) { this._ctx = makeCtx2d(); this._ctx.canvas = this; } return this._ctx; }
  getInfo() { return { usbVendorId: 0x303A, usbProductId: 0x1001 }; }
}

function el(id) {
  if (!elements.has(id)) {
    const e = new El('div', id);
    if (['plotXZ', 'plotXY'].indexOf(id) >= 0) e.tagName = 'CANVAS';
    elements.set(id, e);
  }
  return elements.get(id);
}

/** 递归在 DOM 树里按 id 找元素（动画浮层是 createElement 出来的，不在 elements 登记表里） */
function findById(id) {
  const root = el('body');
  const walk = (n) => {
    if (!n) return null;
    if (n.id === id) return n;
    const kids = n._children || [];
    for (let i = 0; i < kids.length; i++) {
      const hit = walk(kids[i]);
      if (hit) return hit;
    }
    return null;
  };
  return walk(root);
}

const bodyEl = el('body');
bodyEl.tagName = 'BODY';

const documentStub = {
  readyState: 'complete',
  getElementById: id => (id === 'body' ? bodyEl : el(id)),
  createElement: tag => new El(tag),
  createElementNS: (ns, tag) => new El(tag),
  createTextNode: text => ({ nodeType: 3, textContent: String(text) }),
  createDocumentFragment: () => { const f = new El('#fragment'); f._isFragment = true; return f; },
  addEventListener: () => {},
  documentElement: new El('html'),
  body: bodyEl
};

/* ---------------- 可控时钟 + 可手动推帧的 rAF ---------------- */
let fakeNow = 1000;
function nowMs() { return fakeNow; }
let rafQueue = [];
let rafSeq = 0;
function requestAnimationFrameStub(cb) { const id = ++rafSeq; rafQueue.push({ id, cb }); return id; }
function cancelAnimationFrameStub(id) { rafQueue = rafQueue.filter(x => x.id !== id); }
/** 推进一帧：把当前排队的回调各调一次（页面的 loop() 与动画的 animFrame 都在里面） */
function flushFrame(ts) {
  fakeNow = ts;
  const q = rafQueue;
  rafQueue = [];
  q.forEach(x => { try { x.cb(ts); } catch (e) { console.error('  [warn] rAF 回调异常：' + (e && e.message)); } });
}
/** 按 50 ms 一步推进到目标时刻（模拟 ~20 fps，足够覆盖 2.5 s 动画） */
function stepTo(target, stepMs) {
  const step = stepMs || 50;
  while (fakeNow < target) {
    const next = Math.min(target, fakeNow + step);
    flushFrame(next);
  }
}

const timers = [];
const context = {
  console: console,
  document: documentStub,
  navigator: {},                       // 无 Web Serial：与老浏览器路径一致
  performance: { now: () => fakeNow },
  requestAnimationFrame: requestAnimationFrameStub,
  cancelAnimationFrame: cancelAnimationFrameStub,
  matchMedia: (q) => ({ matches: !!reduceMotion && /prefers-reduced-motion/.test(q), media: q, addEventListener: () => {} }),
  setInterval: (fn, ms) => { timers.push({ fn, ms }); return timers.length; },
  clearInterval: id => { timers[id - 1] = null; },
  setTimeout: (fn, ms) => { timers.push({ fn, ms }); return timers.length; },
  clearTimeout: id => { timers[id - 1] = null; },
  TextDecoder: TextDecoder,
  ResizeObserver: undefined
};
context.window = context;
context.globalThis = context;
context.window.innerWidth = 1440;
context.window.innerHeight = 900;
context.window.devicePixelRatio = 1;
context.window.addEventListener = () => {};

/* ---------------- 抽取 <script> 并执行 ---------------- */
const open = html.indexOf('<script>');
const close = html.lastIndexOf('</script>');
if (open < 0 || close < 0) { console.error('未找到 <script> 块'); process.exit(2); }
const js = html.slice(open + 8, close);

vm.createContext(context);
try {
  vm.runInContext(js, context, { filename: 'gesture.inline.js' });
} catch (err) {
  console.error('脚本执行异常：', err && err.stack ? err.stack : err);
  process.exit(1);
}

const G = context.window.GESTURE;
const A = context.window.__gestureAnim;
if (!G) { console.error('页面没有导出 window.GESTURE'); process.exit(1); }
if (!A) { console.error('页面没有导出 window.__gestureAnim'); process.exit(1); }

/* ---------------- 一、页内用例 ---------------- */
const result = G.runSelfTest(false);
console.log('== 一、页内帧解析自测 ==');
result.details.forEach(d => {
  console.log('  ' + (d.ok ? '[通过] ' : '[失败] ') + d.id + ' ' + d.name);
  d.problems.forEach(p => console.log('         · ' + p));
});
console.log('  ' + result.text.split('\n')[0]);

/* ---------------- 二、模拟演示管线（60 拍） ---------------- */
console.log('\n== 二、模拟演示管线（60 拍 × 200 ms，经 buildSimChunk → feedInbound）==');
const probes = [];
let triggered = 0, notTriggered = 0;
const actionSeen = new Set();

G.startSimulation();             // 内部已 tick 一次（tick 0）
flushFrame(fakeNow + 16);
probes.push({
  tick: 0,
  bin: G.state.stats.binFrames, valid: G.state.stats.validLines,
  action: el('h-action').textContent, trig: el('h-trig').textContent
});
actionSeen.add(el('h-action').textContent);
for (let tick = 1; tick < 60; tick++) {
  stepTo(fakeNow + 200);                 // 先按虚拟时间推进帧（动画才会自然结束/解锁）
  const bytes = G.buildSimChunk(tick);
  G.feedInbound(bytes);
  G.updateHero(fakeNow);
  G.updateStatusBar(fakeNow);
  const trig = el('h-trig').textContent;
  if (trig === '触发') triggered++; else notTriggered++;
  actionSeen.add(el('h-action').textContent);
  if (tick === 12) {
    probes.push({ tick, bin: G.state.stats.binFrames, valid: G.state.stats.validLines,
                  action: el('h-action').textContent, trig: trig });
  }
}
const st = G.state.stats;
const sumActions = Object.keys(st.actions).reduce((n, k) => n + st.actions[k], 0);
const checks = [];
function chk(name, ok, detail) { checks.push({ name, ok: !!ok, detail: detail || '' }); }

chk('模拟共 60 行、60 条有效行、0 条坏行',
  st.validLines === 60 && st.badLines === 0,
  'valid=' + st.validLines + ' bad=' + st.badLines);
chk('跳过二进制帧 ≥ 6（每 9 拍一帧合法 0x4544）',
  st.binFrames >= 6, 'binFrames=' + st.binFrames);
chk('重同步 ≥ 2（每 23 拍一帧校验错的 0x4544）',
  st.resync >= 2, 'resync=' + st.resync);
chk('被二进制残留污染的行全部靠文本层重同步救回（坏行仍为 0）',
  st.trimmedLines >= 1 && st.badLines === 0,
  'trimmed=' + st.trimmedLines + ' bad=' + st.badLines);
chk('action 直方图总数 = 有效行数', sumActions === st.validLines,
  'sum=' + sumActions + ' valid=' + st.validLines);
chk('±3 只进"靠近/远离"计数，不进动作直方图',
  (st.proxPos + st.proxNeg) >= 2 && st.actions['3'] === undefined && st.actions['-3'] === undefined,
  'proxPos=' + st.proxPos + ' proxNeg=' + st.proxNeg);
chk('动作大字出现过 ≥3 种（锁定期冻结显示，解锁后随最新遥测刷新）',
  actionSeen.size >= 3, 'seen=' + JSON.stringify([...actionSeen]));
chk('触发卡片两种状态都出现过（穿过 20 cm 阈值）',
  triggered > 0 && notTriggered > 0, '触发=' + triggered + ' 未触发=' + notTriggered);
chk('收字节数 > 0', st.rxBytes > 0, 'rxBytes=' + st.rxBytes);
chk('状态条数字与内部统计一致',
  el('st-valid').textContent === String(st.validLines) &&
  el('st-bin').textContent === String(st.binFrames),
  'st-valid=' + el('st-valid').textContent + ' st-bin=' + el('st-bin').textContent);
chk('位置图 canvas 拿到 2D 上下文并发生绘制',
  el('plotXZ')._ctx && el('plotXZ')._ctx.calls.total > 10,
  'drawCalls=' + (el('plotXZ')._ctx ? el('plotXZ')._ctx.calls.total : 0));
chk('日志出现"跳过二进制帧"与"文本行"两类标注',
  el('logbox').textContent.indexOf('跳过二进制帧') >= 0 &&
  el('logbox').textContent.indexOf('文本行') >= 0, '');
chk('直方图表格被填充（≥5 行）',
  el('histBody').childNodes.length >= 5, 'rows=' + el('histBody').childNodes.length);

checks.forEach(c => console.log('  ' + (c.ok ? '[通过] ' : '[失败] ') + c.name +
  (c.detail ? '  (' + c.detail + ')' : '')));

/* ---------------- 二之二、阈值输入归一化 ---------------- */
const norm = [];
function normChk(name, ok, detail) { norm.push({ name, ok: !!ok, detail: detail || '' }); }
G.applyThreshold('150', 'user');
normChk('阈值 150 → 夹到 100', G.state.thresholdCm === 100, 'now=' + G.state.thresholdCm);
G.applyThreshold('-5', 'user');
normChk('阈值 −5 → 夹到 0', G.state.thresholdCm === 0, 'now=' + G.state.thresholdCm);
G.applyThreshold('', 'user');
normChk('阈值空 → 回退默认 20.0', G.state.thresholdCm === 20, 'now=' + G.state.thresholdCm);
G.applyThreshold('abc', 'user');
normChk('阈值非数字 → 回退默认 20.0', G.state.thresholdCm === 20, 'now=' + G.state.thresholdCm);
G.applyThreshold('35', 'user');
normChk('阈值 35 → 保留 35 且写回三个输入框',
  G.state.thresholdCm === 35 && el('inp-threshold').value === '35.0' &&
  el('inp-threshold-top').value === '35.0' && el('inp-threshold2').value === '35.0',
  'inputs=' + el('inp-threshold').value + '/' + el('inp-threshold-top').value + '/' + el('inp-threshold2').value);
G.applyThreshold('20.0', 'user');
norm.forEach(c => console.log('  ' + (c.ok ? '[通过] ' : '[失败] ') + c.name +
  (c.detail ? '  (' + c.detail + ')' : '')));
console.log('  探测点：' + JSON.stringify(probes));

/* ---------------- 二之三、UI 交互（日志折叠 / 隐藏） ---------------- */
const ui = [];
function uiChk(name, ok, detail) { ui.push({ name, ok: !!ok, detail: detail || '' }); }

G.toggleLogPanel();
uiChk('「收起日志」→ .logpanel 带 collapsed 类，按钮文案变「展开日志」',
  el('logPanel').classList.contains('collapsed') === true &&
  el('btn-logtoggle').textContent === '展开日志',
  'cls=' + el('logPanel').className + ' btn=' + el('btn-logtoggle').textContent);
G.toggleLogPanel();
uiChk('再点一次 → 恢复展开，按钮文案变「收起日志」',
  el('logPanel').classList.contains('collapsed') === false &&
  el('btn-logtoggle').textContent === '收起日志',
  'cls=' + el('logPanel').className + ' btn=' + el('btn-logtoggle').textContent);
G.applyShowLog(false);
uiChk('取消「显示原始日志」→ 面板 hidden 且折叠按钮置灰',
  el('logPanel').hidden === true && el('btn-logtoggle').disabled === true,
  'hidden=' + el('logPanel').hidden + ' disabled=' + el('btn-logtoggle').disabled);
G.applyShowLog(true);
uiChk('重新勾选 → 面板恢复显示、按钮可用',
  el('logPanel').hidden === false && el('btn-logtoggle').disabled === false,
  'hidden=' + el('logPanel').hidden + ' disabled=' + el('btn-logtoggle').disabled);
ui.forEach(c => console.log('  ' + (c.ok ? '[通过] ' : '[失败] ') + c.name +
  (c.detail ? '  (' + c.detail + ')' : '')));

/* ---------------- 二之四、挥手动画层与动画锁定期（6 项） ----------------
   每项开始前把动画与去重状态清干净（等价于"重新连接串口"时的 reset）。 */
console.log('\n== 二之四、挥手动画层与动画锁定期 ==');
const anim = [];
function aChk(name, ok, detail) { anim.push({ name, ok: !!ok, detail: detail || '' }); }

function resetAnim() {
  A.finish();
  G.state.animLock = false;
  G.state.anim.run = null;
  G.state.anim.lastRun = null;
  G.state.anim.lastSkipped = null;
  G.state.anim.lastAction = 0;
  G.state.anim.lastSeenAt = -1e9;
  G.state.anim.residualFrames = 0;
  G.state.anim.finishedCount = 0;
  G.state.stats.skippedDuringAnim = 0;
  G.state.anim.runId = G.state.anim.runId; // runId 单调递增，用作"启动次数"计数
  const layer = findById('gestureAnimLayer');
  if (layer && layer.parentNode) layer.parentNode.removeChild(layer);
  rafQueue = rafQueue.filter(x => true);
}
function line(action, extra) {
  return 'GESTURE: target=1 x=0.100 y=0.000 z=0.300 action=' + action + (extra || '');
}

/* ① 触发 action=1 → 浮层出现，方向 左 → 右 */
resetAnim();
fakeNow = 5000;
const started1 = A.trigger(1);
flushFrame(fakeNow);
const layer1 = findById('gestureAnimLayer');
const s1 = A.state();
aChk('① 触发 action=1：浮层出现且 isLocked() 为真',
  started1 === true && !!layer1 && A.isLocked() === true,
  'layer=' + (layer1 ? layer1.className : 'null') + ' locked=' + A.isLocked());
aChk('① 方向为「屏幕左侧 → 屏幕右侧」（横向 x，8% → 92%，y 居中 50%）',
  s1.axis === 'x' && s1.from.xPct === 8 && s1.to.xPct === 92 &&
  s1.from.yPct === 50 && s1.to.yPct === 50,
  'axis=' + s1.axis + ' from=' + (s1.from && s1.from.xPct) + '%/' + (s1.from && s1.from.yPct) +
  '% to=' + (s1.to && s1.to.xPct) + '%/' + (s1.to && s1.to.yPct) + '%');
aChk('① 起点 transform 落在视口左侧 8%（1440×900 → x≈115.2px, y≈450px）',
  s1.handTransform.indexOf('translate3d(115.20px,450.00px,0)') === 0,
  s1.handTransform);
stepTo(fakeNow + 2200);
const mid = A.state();
aChk('① 2200 ms（位移段结束）后到 92%（x≈1324.8px）',
  Math.abs(mid.xPct - 92) < 0.01 && mid.handTransform.indexOf('translate3d(1324.80px,450.00px,0)') === 0,
  'xPct=' + mid.xPct + ' tf=' + mid.handTransform);
aChk('① 最后 300 ms 淡出：t=2350 时 opacity≈0.5，t=2500 后解锁且浮层被移除',
  (function () {
    stepTo(fakeNow + 150);
    const f = A.state();
    const midOpacity = f.opacity;
    stepTo(fakeNow + 200);
    anim.lastOpacityAt2350 = midOpacity;
    return Math.abs(midOpacity - 0.5) < 0.06 && A.isLocked() === false && !findById('gestureAnimLayer');
  })(),
  'opacity@2350ms=' + anim.lastOpacityAt2350);

/* ② 锁定期内再来一个不同方向 → 不重启动画、skippedDuringAnim +1 */
resetAnim();
fakeNow = 10000;
A.trigger(1);
flushFrame(fakeNow);
const runIdBefore = G.state.anim.runId;
fakeNow += 200;
G.applyLine(line(-1));
flushFrame(fakeNow);
const s2 = A.state();
aChk('② 锁定期内 action=-1：不重启动画（runId 不变、仍在播左挥手）',
  G.state.anim.runId === runIdBefore && s2.action === 1 && A.isLocked() === true,
  'runId=' + G.state.anim.runId + ' action=' + s2.action);
aChk('② skippedDuringAnim 由 0 变 1，日志记「（动画播放中，忽略 action=-1）」',
  G.state.stats.skippedDuringAnim === 1 &&
  el('logbox').textContent.indexOf('（动画播放中，忽略 action=-1）') >= 0,
  'skipped=' + G.state.stats.skippedDuringAnim);
aChk('② 动作大字没有被切换成「右挥手」（仍是触发时的「左挥手」）',
  el('h-action').textContent === '左挥手' && el('h-source-tag').textContent === '动画播放中',
  'h-action=' + el('h-action').textContent + ' tag=' + el('h-source-tag').textContent);

/* ③ 动画结束后可正常触发下一次（含"不补播锁定期内被忽略的方向"） */
stepTo(fakeNow + 2600);
const unlocked = A.isLocked() === false;
const started3 = A.trigger(-1);
flushFrame(fakeNow);
const s3 = A.state();
aChk('③ 解锁后可触发下一次，且播的是新触发的 action=-1（右→左）',
  unlocked && started3 === true && s3.action === -1 && s3.from.xPct === 92 && s3.to.xPct === 8,
  'action=' + s3.action + ' from=' + (s3.from && s3.from.xPct) + '% to=' + (s3.to && s3.to.xPct) + '%');
aChk('③ 锁定期内被忽略的 -1 没有"补播"痕迹：skipped 仍为 1，lastRun 是左挥手',
  G.state.stats.skippedDuringAnim === 1 && A.last().action === 1,
  'skipped=' + G.state.stats.skippedDuringAnim + ' lastRun=' + (A.last() && A.last().action));
A.finish();

/* ④ 同一次检测 6 连帧只播 1 次 */
resetAnim();
fakeNow = 20000;
const runId0 = G.state.anim.runId;
for (let i = 0; i < 6; i++) {
  G.applyLine(line(2));                 // 每 200 ms 一行，与固件节奏一致
  if (i < 5) { fakeNow += 200; }
}
const runId4 = G.state.anim.runId;
aChk('④ 同一 action=2 的 6 连帧（间隔 200 ms）只启动 1 次动画',
  runId4 - runId0 === 1 && G.state.stats.skippedDuringAnim === 0,
  'starts=' + (runId4 - runId0) + ' skipped=' + G.state.stats.skippedDuringAnim);
aChk('④ 6 帧里 5 帧被判为残留帧（residualFrames=5），且不计入 skipped',
  G.state.anim.residualFrames === 5,
  'residual=' + G.state.anim.residualFrames);

/* ⑤ 锁定期内行率 / 有效行 / 坏行 / 直方图仍在增长（证明解析没被暂停） */
const v0 = G.state.stats.validLines;
const b0 = G.state.stats.badLines;
const a0 = G.state.stats.actions['1'] || 0;
G.updateStatusBar(fakeNow);              // 让 5 秒行率窗口先收敛，再取基线
const t0 = G.state.lineTimes.length;
fakeNow += 200;
G.feedInbound(new Uint8Array(Array.from(line(1) + '\r\n').map(c => c.charCodeAt(0))));
G.feedInbound(new Uint8Array(Array.from('hello world\r\n').map(c => c.charCodeAt(0))));
const lockedDuringParse = A.isLocked();
G.updateStatusBar(fakeNow);
  aChk('⑤ 锁定期内有效行 +1、坏行 +1、行率 >0（解析与统计完全没停）',
  lockedDuringParse === true && G.state.stats.validLines === v0 + 1 &&
  G.state.stats.badLines === b0 + 1 && G.state.lineTimes.length === t0 + 1 &&
  parseFloat(el('ss-rate').textContent) > 0,
  'valid=' + v0 + '→' + G.state.stats.validLines + ' bad=' + b0 + '→' + G.state.stats.badLines +
  ' rate=' + el('ss-rate').textContent);
aChk('⑤ 锁定期内 action=1 仍照常计数（直方图数据 +1），但动作大字仍冻结在「上挥手」',
  (G.state.stats.actions['1'] || 0) === a0 + 1 && el('h-action').textContent === '上挥手',
  'actions[1]=' + a0 + '→' + (G.state.stats.actions['1'] || 0) + ' h-action=' + el('h-action').textContent);
aChk('⑤ 「动画期忽略」计数在会话统计里可见（=1，本次 action=1 与触发动作同向被忽略）',
  el('ss-skip').textContent === '1',
  'ss-skip=' + el('ss-skip').textContent);

/* ⑥ pointer-events:none（CSS + 内联双保险），保证动画期间仍可交互 */
const layer6 = findById('gestureAnimLayer');
const cssText = (html.match(/<style>([\s\S]*?)<\/style>/) || [])[1] || '';
aChk('⑥ 浮层内联 pointer-events:none（覆盖整个视口 position:fixed;inset:0）',
  !!layer6 && layer6.style.pointerEvents === 'none',
  'inline=' + (layer6 && layer6.style.pointerEvents) +
  ' position=' + (layer6 && layer6.style.position));
aChk('⑥ CSS 里 .anim-layer 同样是 pointer-events:none 且 z-index 高于顶栏（>5）',
  /\.anim-layer\s*\{[^}]*pointer-events:none/.test(cssText) &&
  /\.anim-layer\s*\{[^}]*z-index:9000/.test(cssText) &&
  /\.anim-layer\s*\{[^}]*inset:0/.test(cssText),
  'zIndex=' + (layer6 && layer6.style.zIndex));
aChk('⑥ 手掌是内联 SVG（createElementNS 生成，无外链、无 emoji）',
  (function () {
    const hand = findById('gestureAnimHand');
    const svg = hand && hand._children[0];
    const path = svg && svg._children[0];
    if (!svg || !path) return false;
    const d = path.getAttribute('d') || '';
    return svg.tagName.toLowerCase() === 'svg' &&
           (d.match(/M/g) || []).length >= 5 &&          // 掌 + 4 指 + 拇 = 5 个子路径
           !/https?:|url\(/.test(d) && !/[\u{1F300}-\u{1FAFF}]/u.test(el('logbox').textContent);
  })(),
  'd 子路径数=' + (function () {
    const hand = findById('gestureAnimHand');
    const svg = hand && hand._children[0];
    const path = svg && svg._children[0];
    return ((path && path.getAttribute('d')) || '').split('M').length - 1;
  })());

/* ⑦ prefers-reduced-motion：静态居中、800 ms 淡入淡出、不位移 */
resetAnim();
reduceMotion = true;
fakeNow = 30000;
A.trigger(1);
flushFrame(fakeNow);
const s7 = A.state();
stepTo(fakeNow + 400);
const s7b = A.state();
stepTo(fakeNow + 500);
aChk('⑦ prefers-reduced-motion：durationMs=800、自始至终居中不位移、结束后解锁',
  s7.reduced === true && s7.durationMs === 800 && s7.from.xPct === 50 && s7.to.xPct === 50 &&
  Math.abs(s7b.xPct - 50) < 1e-9 && A.isLocked() === false,
  'reduced=' + s7.reduced + ' dur=' + s7.durationMs + ' xPct@400ms=' + s7b.xPct);
reduceMotion = false;

/* ⑧ 调试钩子语义：锁定期内 trigger() 同遥测（只计数），force 才重入 */
resetAnim();
fakeNow = 40000;
A.trigger(2);
flushFrame(fakeNow);
const skippedBefore = G.state.stats.skippedDuringAnim;
const forced1 = A.trigger(-2);
const skippedAfter = G.state.stats.skippedDuringAnim;
const runIdForced = G.state.anim.runId;
const forced2 = A.trigger(-2, { force: true });
flushFrame(fakeNow);
aChk('⑧ __gestureAnim.trigger() 锁定期内与遥测同语义（返回 false 且 skipped+1）；force 才重入',
  forced1 === false && skippedAfter === skippedBefore + 1 && forced2 === true &&
  G.state.anim.runId === runIdForced + 1 && A.state().action === -2,
  'first=' + forced1 + ' skipped=' + skippedBefore + '→' + skippedAfter + ' force=' + forced2);
A.finish();

/* ⑨ 0 / ±3 不触发动画，且不写 skipped */
resetAnim();
fakeNow = 50000;
const runId9 = G.state.anim.runId;
G.applyLine(line(0));
G.applyLine(line(3));
G.applyLine(line(-3));
aChk('⑨ action=0 与 ±3 不触发动画（runId 不变、浮层不存在、skipped 仍为 0）',
  G.state.anim.runId === runId9 && !findById('gestureAnimLayer') &&
  G.state.stats.skippedDuringAnim === 0,
  'runId=' + G.state.anim.runId + ' skipped=' + G.state.stats.skippedDuringAnim);

anim.forEach(c => console.log('  ' + (c.ok ? '[通过] ' : '[失败] ') + c.name +
  (c.detail ? '  (' + c.detail + ')' : '')));

/* ---------------- 三、静态不变量 / 布局契约 ---------------- */
console.log('\n== 三、静态不变量与布局契约 ==');
const inv = [];
function invChk(name, ok, detail) { inv.push({ name, ok: !!ok, detail: detail || '' }); }

function stripJsComments(src) {
  let out = '';
  let i = 0;
  let q = null;
  while (i < src.length) {
    const c = src[i], n = src[i + 1];
    if (q) {
      out += c;
      if (c === '\\') { out += (src[i + 1] || ''); i += 2; continue; }
      if (c === q) q = null;
      i++;
      continue;
    }
    if (c === '"' || c === "'" || c === '`') { q = c; out += c; i++; continue; }
    if (c === '/' && n === '/') { while (i < src.length && src[i] !== '\n') i++; continue; }
    if (c === '/' && n === '*') {
      i += 2;
      while (i < src.length && !(src[i] === '*' && src[i + 1] === '/')) i++;
      i += 2;
      continue;
    }
    out += c; i++;
  }
  return out;
}
const codeOnly = stripJsComments(js);

function stripStrings(src) {
  let out = '';
  let i = 0;
  while (i < src.length) {
    const c = src[i];
    if (c === '"' || c === "'" || c === '`') {
      const q = c;
      out += q; i++;
      while (i < src.length) {
        if (src[i] === '\\') { i += 2; continue; }
        if (src[i] === q) { out += q; i++; break; }
        i++;
      }
      continue;
    }
    out += c; i++;
  }
  return out;
}
const storageHits = (html.match(/localStorage|sessionStorage|indexedDB/gi) || []).length;

invChk('代码里不含 localStorage / sessionStorage / indexedDB',
  !/localStorage|sessionStorage|indexedDB/.test(codeOnly),
  '全文出现 ' + storageHits + ' 次，全部在注释/说明文字里');
invChk('代码里不含 document.cookie', !/document\.cookie/.test(codeOnly), '');
invChk('代码里不含 port.writable（没有任何写入通道）', !/\.writable/.test(codeOnly), '');
invChk('代码里不含 getWriter / writer 标识符（字符串与注释之外）',
  !/getWriter/.test(codeOnly) && !/\bwriter\b/.test(stripStrings(codeOnly)), '');
invChk('JS 里没有真正的串口写调用（无 .write()）', !/\.write\s*\(/.test(codeOnly), '');
invChk('无外链资源（无 src= / href= 指向 http(s)）',
  !/(?:src|href)\s*=\s*["']?(?:https?:)?\/\//i.test(html), '');
invChk('无 <link rel=stylesheet> / 外部字体',
  !/<link\b/i.test(html) && !/@import|url\(\s*['"]?https?:/i.test(html), '');
invChk('无 fetch / XMLHttpRequest / WebSocket', !/\bfetch\s*\(|XMLHttpRequest|WebSocket/.test(js), '');
invChk('端口打开只用 navigator.serial.requestPort', /navigator\.serial\.requestPort/.test(js), '');
invChk('默认波特率 115200 且选项含 230400/460800/921600',
  /baudRate:\s*115200/.test(js) && /230400/.test(html) && /460800/.test(html) && /921600/.test(html), '');
invChk('正则接受科学计数法（[eE] 分支存在）', /\[eE\]/.test(js), '');
invChk('二进制帧合法范围 11~4096', /minBinaryFrame:\s*11/.test(js) && /maxBinaryFrame:\s*4096/.test(js), '');
invChk('帧头为 0x45 0x44', /header:\s*\[0x45,\s*0x44\]/.test(js), '');
invChk('action 表逐条与厂商配色一致',
  /'#0284c7'/.test(js) && /'#2563eb'/.test(js) && /'#64748b'/.test(js) &&
  /'#16a34a'/.test(js) && /'#ea580c'/.test(js), '');
invChk('单文件且只有一个 <script> 块', (html.match(/<script/g) || []).length === 1, '');
invChk('动画层不引入任何网络/存储 API（无 fetch / 无 localStorage / 无 port 写入）',
  !/(fetch|XMLHttpRequest|localStorage|writable)/.test(
    stripJsComments(js).slice(stripJsComments(js).indexOf('十一、挥手动画层') >= 0
      ? stripJsComments(js).indexOf('function animPlan') : 0,
      stripJsComments(js).indexOf('function installAnimDebugApi'))), '');

const css = (html.match(/<style>([\s\S]*?)<\/style>/) || [])[1] || '';
invChk('应用壳：.app height:100vh + flex 列', /\.app\s*\{[^}]*height:100vh/.test(css), '');
invChk('整页不滚动：body overflow:hidden', /body\s*\{[^}]*overflow:hidden/.test(css), '');
invChk('主区两列：grid-template-columns 带 --right-col', /grid-template-columns:\s*minmax\(0,1fr\)\s*var\(--right-col\)/.test(css), '');
invChk('右列独立滚动：overflow-y:auto', /\.col-right\s*\{[^}]*overflow-y:auto/.test(css), '');
invChk('顶栏吸顶层级：.topbar z-index', /\.topbar\s*\{[^}]*z-index:\s*\d/.test(css), '');
invChk('日志面板定高：height:var(--log-h)', /\.logpanel\s*\{[^}]*height:var\(--log-h\)/.test(css), '');
invChk('日志面板可折叠（.collapsed 高度 42px）', /\.logpanel\.collapsed\s*\{[^}]*height:42px/.test(css), '');
invChk('两张位置图 canvas 绝对铺满各自 wrap', /\.plotwrap canvas\s*\{[^}]*position:absolute[^}]*inset:0/.test(css), '');
invChk('矮屏媒体查询（max-height:820px）存在', /@media\s*\(max-height:\s*820px\)/.test(css), '');
invChk('窄屏媒体查询（max-width:1100px）存在', /@media\s*\(max-width:\s*1100px\)/.test(css), '');
invChk('三张卡片区高度固定（--hero-h）', /\.cards3\s*\{[^}]*height:var\(--hero-h\)/.test(css), '');
invChk('X-Z 俯视图语义：x>0 画在左侧（cx - xn * ...）', /px = cx - xn \*/.test(js), '');
invChk('X-Y 正视图语义：y>0 画在上方（cy - yn * ...）', /py = cy - yn \*/.test(js), '');
invChk('无目标时两图都显示「目标位置未就绪」', (js.match(/目标位置未就绪/g) || []).length >= 3, '');
invChk('触发严格小于（z < th）', /telemetry\.z < th/.test(js), '');
invChk('动画层：position:fixed + inset:0 覆盖整个视口', /\.anim-layer\s*\{[^}]*position:fixed[^}]*inset:0/.test(css), '');
invChk('动画层：pointer-events:none 不拦截交互', /\.anim-layer\s*\{[^}]*pointer-events:none/.test(css), '');
invChk('动画层：z-index 9000 高于顶栏（.topbar z-index:5）', /\.anim-layer\s*\{[^}]*z-index:9000/.test(css), '');
invChk('动画层：prefers-reduced-motion 媒体查询存在（不位移/无拖尾）',
  /@media\s*\(prefers-reduced-motion:\s*reduce\)[\s\S]{0,300}\.anim-trail\s*\{\s*display:none/.test(css), '');
invChk('方向表 4 条齐备：1/−1/2/−2（0 与 ±3 不在表里）',
  /'1':\s*\{[^}]*axis:\s*'x'/.test(js) && /'-1':\s*\{[^}]*axis:\s*'x'/.test(js) &&
  /'2':\s*\{[^}]*axis:\s*'y'/.test(js) && /'-2':\s*\{[^}]*axis:\s*'y'/.test(js), '');
invChk('动画时长 2500 ms / 淡出 300 ms / 同事件窗口 300 ms 写在 CFG.anim',
  /durationMs:\s*2500/.test(js) && /fadeMs:\s*300/.test(js) && /sameEventWindowMs:\s*300/.test(js), '');
invChk('手掌图形是内联 SVG 路径（无 <image>、无 emoji 字体依赖）',
  /HAND_PATH_D/.test(js) && !/<image\b/.test(html) && !/[\u{1F300}-\u{1FAFF}]/u.test(js), '');

inv.forEach(c => console.log('  ' + (c.ok ? '[通过] ' : '[失败] ') + c.name +
  (c.detail ? '  (' + c.detail + ')' : '')));

/* ---------------- 汇总 ---------------- */
const pageOk = result.pass;
const pageTotal = result.total;
const pipeOk = checks.filter(c => c.ok).length;
const normOk = norm.filter(c => c.ok).length;
const uiOk = ui.filter(c => c.ok).length;
const animOk = anim.filter(c => c.ok).length;
const invOk = inv.filter(c => c.ok).length;
const allOk = (pageOk === pageTotal) && (pipeOk === checks.length) &&
              (normOk === norm.length) && (uiOk === ui.length) &&
              (animOk === anim.length) && (invOk === inv.length);

console.log('\n== 汇总 ==');
console.log('  页内帧解析用例：' + pageOk + '/' + pageTotal);
console.log('  模拟管线断言  ：' + pipeOk + '/' + checks.length);
console.log('  阈值归一化断言：' + normOk + '/' + norm.length);
console.log('  UI 交互断言   ：' + uiOk + '/' + ui.length);
console.log('  动画/锁定期断言：' + animOk + '/' + anim.length);
console.log('  静态不变量/契约：' + invOk + '/' + inv.length);
console.log('  合计           ：' + (pageOk + pipeOk + normOk + uiOk + animOk + invOk) + '/' +
            (pageTotal + checks.length + norm.length + ui.length + anim.length + inv.length));
console.log('  结论           ：' + (allOk ? 'PASS' : 'FAIL'));
process.exit(allOk ? 0 : 1);
```

---

## 附录 B：交付物清单

| 路径 | 字节 | 说明 |
|---|---|---|
| `release/gesture.html` | 122,643 | 单文件网页工具（2,912 行，零外链，只读；v1.1 新增挥手动画层 §十一） |
| `release/gesture-selftest.md` | 本文件 | 自测报告（98/98）+ 布局/交互/动画契约 + 上板清单（含 H1~H8 动画验收）+ harness 全文（附录 A，≈33 KB，可直接落盘复跑） |
| `docs/project/edv163-gesture/web-gesture-tool-design.md` | 42,845 | 设计说明（协议映射含源码行号、二进制帧跳过算法、**动画与锁定期（第 16 节）**、未确认项、决策记录 D1~D13） |

未改动：`release/level.html`、`release/track.html`（及二者对应的 selftest 文档）、
`release/edv163-gesture/gesture.html`（对外副本，由项目经理同步）。
未新增：固件、工具脚本、发布包（不在本次全栈交付范围）；harness 以附录 A 形式随文档入库，不进仓库脚本目录。
