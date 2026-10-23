# EDV163 手扫（挥手识别）网页版工具 — 自测报告（gesture.html）

> 对象：`release/gesture.html`（90,641 字节 / 2,208 行，单文件零外链）
> 设计说明：`docs/project/edv163-gesture/web-gesture-tool-design.md`
> 日期：2026-10-23 ｜ 全栈工程师 ｜ **浏览器未启动**（沙箱禁 Chromium），全部结论来自 Node 桩环境
> **状态：未上板验证** —— 见第 7 节「上板验收清单」与第 9 节「未覆盖项」

---

## 1. 结论摘要

| 分类 | 通过 | 说明 |
|---|---|---|
| 页内帧解析用例（页面按钮 / 启动静默跑同一套函数） | **13 / 13** | 覆盖 ①正常行 ②`target=0` ③action 各值 ④±3 ⑤科学计数法 ⑥旧格式 ⑦`DBG:` ⑧二进制帧混流 ⑨半行截断 ⑩噪声重同步 + 坏行 + 触发 ⑪只读不变量 |
| 模拟演示管线断言 | **13 / 13** | 60 拍 × 200 ms，字节流回灌同一解析器：60 有效行、7 帧二进制跳过、3 次重同步、0 坏行 |
| 阈值输入归一化断言 | **5 / 5** | 越界夹取、空/非数字回退、三输入框同步 |
| UI 交互断言（日志折叠 / 隐藏） | **4 / 4** | 折叠类名与按钮文案、隐藏时按钮置灰、恢复可用 |
| 静态不变量与布局契约 | **30 / 30** | 无外链 / 无写入路径 / 无本地存储 / 满视口尺寸链 |
| **合计** | **65 / 65 PASS** | `node --check` 语法校验 0 错 |

命令与结果一一对应，可逐字复现（第 8 节）。

---

## 2. 页内帧解析用例（13 条，全通过）

页面右上「运行帧解析自测」/ 右列「运行帧解析自测」触发；`boot()` 时也会静默跑一遍。
每条用例**独立新建** `GestureStreamParser`，互不影响，也不碰会话状态。

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

```
自测结果：13/13 通过
```

### 2.1 几个数字为什么是这些值（免得下次误判）

| 数字 | 由来 |
|---|---|
| C8 的 `binFrames=1` 而不是 2 | 第 2 帧是**故意改坏校验和**的（`cs+1`），它走的是"丢 1 字节重同步"分支，计入 `resync` 而非 `binFrames`。这是 D3 决策：正常点云帧与坏帧头必须分开计数，否则"跳过帧数"失去诊断意义 |
| C8/C10 的坏行仍是 0 | 校验错帧丢弃 1 字节后，残留字节会粘在下一行前面（厂商实现里这行就废了）。本页在文本层补了一次「找 `GESTURE:`/`DBG:` 前缀再匹配」的重同步（设计文档 §6.5），所以行救回来了、只记 `trimmedLines` |
| C9 的末尾残行不记坏行 | 残行**没有换行**，解析器按厂商语义继续等下一批字节，不交付、不判坏。它只在"缓冲区超 16384 字节"时才被裁剪 |
| C11 的坏行 = 2 | `hello world` 与 `target=2` 都不匹配 6 条正则（`target` 只允许 `0|1`），各记 1 条 |

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
| 7 | 动作大字被真正驱动（≥3 种） | `["无挥手","左挥手","右挥手","上挥手","下挥手"]`（5 种全出现） |
| 8 | 触发卡片两种状态都出现（`z` 穿过 20 cm 阈值） | `触发=14 未触发=45` |
| 9 | 收字节数 > 0 | `rxBytes=3343` |
| 10 | 状态条数字与内部统计一致 | `st-valid=60 st-bin=7` |
| 11 | 两张位置图 canvas 真的在画 | `plotXZ` 2D 上下文累计 7,726 次调用 |
| 12 | 日志两类标注都在 | 含「跳过二进制帧」与「文本行」 |
| 13 | 直方图表格被填充 | `rows=7`（动作 5 行 + 靠近/远离 2 行） |

探测点（便于对时序）：`tick=0 → 无挥手 / 未触发 / valid=1 / bin=0`；
`tick=12 → 上挥手 / 未触发 / valid=13 / bin=1`。

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

## 5. 静态不变量、布局与交互契约（34 项，全通过）

> 判定方式：先把 HTML 注释与 JS 注释剥掉，**只对代码**检查这些标识符
> （注释里写「本页不写 localStorage」这类免责声明属正常，不算违规；
> 自测断言里的字符串字面量 `'writer' in state` 也不算标识符）。
> 布局/交互部分：CSS 契约 14 项走源码正则；交互 4 项由桩环境真调用页面函数后读 DOM 状态。

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
| 16 | 触发判定是 `z < th`（严格小于） | ✅ |

### 5.2 布局契约（14 项）

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
| 自测 | 面板按钮 + 启动静默跑 | 同（13 条） |
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
| G5 | 自我检查 | 点「运行帧解析自测」→ 结果框显示 `自测结果：13/13 通过` |
| G6 | 模拟与真串口互斥 | 模拟中「连接串口」置灰；连上串口后模拟开关自动弹回 |

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

### 8.2 桩环境自测（页内 13 条 + 模拟管线 13 项 + 阈值 5 项 + UI 交互 4 项 + 静态 30 项 = 65 项）

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

1. 抽出页内 `<script>`，在 `vm` 沙箱里用 **DOM / Canvas / Web Serial 桩**执行（页面 `boot()` 自动跑一遍静默自测）；
2. 显式调用页面导出的 `runSelfTest(false)`，逐条打印 C1~C13；
3. 用页面导出的 `buildSimChunk()` + `feedInbound()` 直接驱动 60 拍模拟流，断言统计/直方图/触发/画布；
4. 用 `applyThreshold()` 验 5 条归一化（越界夹取 / 回退 / 三框同步）；
5. 用 `toggleLogPanel()` / `applyShowLog()` 验 4 条 UI 交互（折叠类名、按钮文案、隐藏与置灰）；
6. 对源码做 strip 注释后的静态不变量与布局契约检查（30 条）。

### 8.3 页面内自测（有浏览器时）

直接用 Chrome/Edge 打开 `release/gesture.html`（`file://` 一般亦可），
点右上「运行帧解析自测」或右列「运行帧解析自测」，结果框首行应为
`自测结果：13/13 通过`；日志里同步出现同样一行。

### 8.4 本次实际运行结果（原文摘录）

```
== 一、页内帧解析自测 ==
  [通过] C1 … [通过] C13
  自测结果：13/13 通过
== 二、模拟演示管线（60 拍 × 200 ms，经 buildSimChunk → feedInbound）==
  [通过] 模拟共 60 行、60 条有效行、0 条坏行  (valid=60 bad=0)
  [通过] 跳过二进制帧 ≥ 6（每 9 拍一帧合法 0x4544）  (binFrames=7)
  [通过] 重同步 ≥ 2（每 23 拍一帧校验错的 0x4544）  (resync=3)
  [通过] 被二进制残留污染的行全部靠文本层重同步救回（坏行仍为 0）  (trimmed=3 bad=0)
  [通过] ±3 只进"靠近/远离"计数，不进动作直方图  (proxPos=2 proxNeg=2)
  [通过] 触发卡片两种状态都出现过（穿过 20 cm 阈值）  (触发=14 未触发=45)
  [通过] 位置图 canvas 拿到 2D 上下文并发生绘制  (drawCalls=7726)
  …（共 13 条，全部通过）
  [通过] 阈值 150 → 夹到 100  (now=100)
  …（阈值归一化共 5 条，全部通过）
  [通过] 取消「显示原始日志」→ 面板 hidden 且折叠按钮置灰  (hidden=true disabled=true)
  …（UI 交互共 4 条，全部通过）
== 三、静态不变量与布局契约 ==
  …（共 30 条，全部通过）
== 汇总 ==
  页内帧解析用例：13/13
  模拟管线断言  ：13/13
  阈值归一化断言：5/5
  UI 交互断言   ：4/4
  静态不变量/契约：30/30
  合计           ：65/65
  结论           ：PASS
```

---

## 9. 未覆盖项与说明

| # | 未覆盖 | 原因 | 谁补 |
|---|---|---|---|
| N1 | **真实串口 + 真实固件的端到端**（波特率、行率、`action` 编码方向、二进制帧占比、`target=0` 时延） | 无硬件在环；沙箱禁 Chromium | 上板（第 7 节清单）→ 回填设计文档 U1~U7 |
| N2 | 真浏览器**渲染观感**（字体、颜色、1280×720 是否挤、右列滚动条位置） | 同上 | 用户目检 F1~F10 + G1~G6 |
| N3 | `navigator.serial` 的**权限拒绝 / 拔线 / 端口被占用**路径 | 桩环境无 Web Serial 对象，只走了"不支持"分支 | 上板时故意拔线一次，看日志是否给出「读取异常」并允许重连 |
| N4 | 极长会话（>1 h）内存与日志裁剪 | 桩跑 60 拍；日志 400 行上限与 `lineTimes` 600 上限是代码级保证 | 上板挂 1 h 看页签内存 |
| N5 | 窄屏（<1100px）双列堆叠的真实观感 | 同上 | 手机/半屏目检 |

**测试桩的已知边界**：`requestAnimationFrame` 被替换为空实现（不真跑 30 fps 主循环），
所以"主循环连续重绘"只由 harness 手动调用 `updateHero/updateStatusBar/drawPlots` 覆盖；
真实浏览器里由 `requestAnimationFrame(loop)` 驱动，逻辑同一套函数。

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

---

## 附录 A：桩环境 harness 全文

> 保存为 `%TEMP%\gesture_selftest\stub-harness.js` 后按 8.2 执行。
> 依赖：Node 18+（只用 `fs`/`vm`/`path` 三个内置模块，无需安装任何包）。

```javascript
/* eslint-disable */
/*
 * gesture.html 桩环境自测 harness（不启动浏览器）
 *
 * 用法：
 *   node stub-harness.js <gesture.html 的绝对路径>
 *
 * 做的事：
 *   1) 抽出页面内唯一的 <script> 块，在 vm 沙箱里用 DOM/Canvas/Web Serial 桩执行；
 *      页面 boot() 会自动跑一遍静默自测。
 *   2) 再显式调用页面的 runSelfTest(false)，逐条打印用例结果。
 *   3) 用 buildSimChunk()+feedInbound() 直接驱动"模拟演示"那条管线 60 拍，
 *      断言 有效行 / 跳过二进制帧 / 重同步 / 直方图 / 触发卡片 都能被驱动。
 *   4) 对源码做静态不变量检查（无外链 / 无写入路径 / 无本地存储）与布局契约检查。
 */
'use strict';
const fs = require('fs');
const vm = require('vm');

const htmlPath = process.argv[2];
if (!htmlPath) { console.error('usage: node stub-harness.js <path-to-gesture.html>'); process.exit(2); }
const html = fs.readFileSync(htmlPath, 'utf8');

/* ---------------- DOM 桩 ---------------- */
const elements = new Map();

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

class El {
  constructor(tag, id) {
    this.tagName = (tag || 'div').toUpperCase();
    this.id = id || '';
    this._className = '';
    this.style = {};
    this._text = '';
    this._children = [];
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
  appendChild(node) {
    if (node && node._isFragment) { node._children.forEach(c => this._children.push(c)); return node; }
    this._children.push(node);
    this._text += (node && node.textContent) ? node.textContent : '';
    return node;
  }
  removeChild(node) {
    const i = this._children.indexOf(node);
    if (i >= 0) this._children.splice(i, 1);
    if (node && node.textContent) this._text = this._text.replace(node.textContent, '');
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
    const canvasIds = ['plotXZ', 'plotXY'];
    if (canvasIds.indexOf(id) >= 0) { e.tagName = 'CANVAS'; }
    elements.set(id, e);
  }
  return elements.get(id);
}

const documentStub = {
  readyState: 'complete',
  getElementById: id => el(id),
  createElement: tag => new El(tag),
  createTextNode: text => ({ nodeType: 3, textContent: String(text) }),
  createDocumentFragment: () => { const f = new El('#fragment'); f._isFragment = true; return f; },
  addEventListener: () => {},
  documentElement: new El('html')
};

/* 记录所有定时器，但一律不触发（避免后台无限 tick） */
const timers = [];
const context = {
  console: console,
  document: documentStub,
  navigator: {},                       // 无 Web Serial：与老浏览器路径一致
  performance: { now: () => nowMs() },
  requestAnimationFrame: () => 1,      // 不真正回调 loop，避免 30fps 空转
  cancelAnimationFrame: () => {},
  setInterval: (fn, ms) => { timers.push({ fn, ms }); return timers.length; },
  clearInterval: id => { timers[id - 1] = null; },
  setTimeout: (fn, ms) => { timers.push({ fn, ms }); return timers.length; },
  clearTimeout: () => {},
  TextDecoder: TextDecoder,
  Uint8Array: Uint8Array,
  ResizeObserver: undefined,
  Math: Math,
  JSON: JSON,
  Date: Date,
  Number: Number,
  String: String,
  Object: Object,
  Array: Array,
  parseInt: parseInt,
  parseFloat: parseFloat,
  isFinite: isFinite
};
context.window = context;
context.globalThis = context;
context.window.devicePixelRatio = 1;
context.window.addEventListener = () => {};

let fakeNow = 1000;
function nowMs() { return fakeNow; }

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
if (!G) { console.error('页面没有导出 window.GESTURE'); process.exit(1); }

/* ---------------- 一、页内用例 ---------------- */
const result = G.runSelfTest(false);
console.log('== 一、页内帧解析自测 ==');
if (process.env.GESTURE_PROBE) {
  console.log('  [probe] window.GESTURE keys = ' + JSON.stringify(Object.keys(G)));
}
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
probes.push({
  tick: 0,
  bin: G.state.stats.binFrames, valid: G.state.stats.validLines,
  action: el('h-action').textContent, trig: el('h-trig').textContent
});
actionSeen.add(el('h-action').textContent);
for (let tick = 1; tick < 60; tick++) {
  fakeNow += 200;
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
chk('动作大字出现过 ≥3 种（左/右/上/下 至少三种被驱动）',
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

/* ---------------- 二之二、阈值输入归一化（照源码夹取 / 回退语义） ---------------- */
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

/* ---------------- 三、静态不变量 / 布局契约 ---------------- */
console.log('\n== 三、静态不变量与布局契约 ==');
const inv = [];
function invChk(name, ok, detail) { inv.push({ name, ok: !!ok, detail: detail || '' }); }

/* 只检查"代码"里有没有这些标识符：注释与页面说明文字里出现「不写 localStorage」
   这类免责声明是正常的，不能算违规。所以先把注释剥掉再判定。 */
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

/* 再把字符串字面量的内容清空：像 'writer' in state 这种自测断言里的字面量不算标识符 */
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

inv.forEach(c => console.log('  ' + (c.ok ? '[通过] ' : '[失败] ') + c.name +
  (c.detail ? '  (' + c.detail + ')' : '')));

/* ---------------- 汇总 ---------------- */
const pageOk = result.pass;
const pageTotal = result.total;
const pipeOk = checks.filter(c => c.ok).length;
const normOk = norm.filter(c => c.ok).length;
const uiOk = ui.filter(c => c.ok).length;
const invOk = inv.filter(c => c.ok).length;
const allOk = (pageOk === pageTotal) && (pipeOk === checks.length) &&
              (normOk === norm.length) && (uiOk === ui.length) && (invOk === inv.length);

console.log('\n== 汇总 ==');
console.log('  页内帧解析用例：' + pageOk + '/' + pageTotal);
console.log('  模拟管线断言  ：' + pipeOk + '/' + checks.length);
console.log('  阈值归一化断言：' + normOk + '/' + norm.length);
console.log('  UI 交互断言   ：' + uiOk + '/' + ui.length);
console.log('  静态不变量/契约：' + invOk + '/' + inv.length);
console.log('  合计           ：' + (pageOk + pipeOk + normOk + uiOk + invOk) + '/' +
            (pageTotal + checks.length + norm.length + ui.length + inv.length));
console.log('  结论           ：' + (allOk ? 'PASS' : 'FAIL'));
process.exit(allOk ? 0 : 1);
```

---

## 附录 B：交付物清单

| 路径 | 字节 | 说明 |
|---|---|---|
| `release/gesture.html` | 90,641 | 单文件网页工具（2,208 行，零外链，只读） |
| `release/gesture-selftest.md` | 本文件 | 自测报告（65/65）+ 布局与交互契约 + 上板清单 + harness 全文（附录 A，19.5 KB，可直接落盘复跑） |
| `docs/project/edv163-gesture/web-gesture-tool-design.md` | 28,916 | 设计说明（协议映射含源码行号、二进制帧跳过算法、未确认项、决策记录） |

未改动：`release/level.html`、`release/track.html`（及二者对应的 selftest 文档）。
未新增：固件、工具脚本、发布包（不在本次全栈交付范围）。
