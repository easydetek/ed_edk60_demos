# 发布包说明 — EDK60 × EDX106 门锁感应雷达（v1.0.0）

> 2026-10-27 ｜ 全栈工程师（opcs-fullstack-engineer）｜ 状态：**网页与发布包已就绪，等用户批准后 push**
> 本文档是本次发布的唯一清单：文件、体积、SHA256、线上 URL、构建结果、复验清单。
> **未执行任何 git 操作**（不 commit / 不 push / 不 tag）。

---

## 一、交付物清单（工程侧 `release/edx106/`）

| # | 文件 | 体积 (B) | SHA256 |
|---|---|---|---|
| 1 | `release/edx106/test.html` | 137,772 | `56BBF96FE7F0B48FAB2339CB94716C5B3B820CF688364C767559BA3B1F07C726` |
| 2 | `release/edx106/flash.html` | 16,515 | `3EF3D1F953D863636B116AFC9EDFF7686DCBA7CE45DB26AA3299EC768C966A41` |
| 3 | `release/edx106/manifest.json` | 283 | `8F47674D0F27F3790CD96FAC28CF30E79C6C34D44158264BDF7C7D9449629031` |
| 4 | `release/edx106/edk60_edx106_door_lock.bin` | 254,736 | `1B13C347C9EB50DC7A792B697B02CD16831713039D38D546D8493E165306CE78` |

### 1.1 固件来源与核对

- 来源：`build-door_lock_edx106/merged-binary.bin`（`idf.py -B build-door_lock_edx106 merge-bin` 产物），**直接复制**，未做任何二次处理。
- 复制后逐字节/哈希核对：**源 254,736 B / `1B13C347…306CE78` = 目标 254,736 B / `1B13C347…306CE78`**（一致）。
- 烧录偏移 `0x0`（合并固件一次成型，含 bootloader + 分区表 + app），目标 `ESP32-C6`。

### 1.2 文档站静态资源（`static/edk60/edx106/`）

4 个文件同名复制，**逐文件 SHA256 与工程侧一致**（已核对，全部 True）：

| 文件 | 体积 (B) | SHA256 前 16 位 | 与源一致 |
|---|---|---|---|
| `test.html` | 137,772 | `56BBF96FE7F0B48F` | ✅ |
| `flash.html` | 16,515 | `3EF3D1F953D86363` | ✅ |
| `manifest.json` | 283 | `8F47674D0F27F379` | ✅ |
| `edk60_edx106_door_lock.bin` | 254,736 | `1B13C347C9EB50DC` | ✅ |

> **未改动**既有的 `static/edk60/edv11p/`、`static/edk60/edv163/`、`static/edk60/edv163-gesture/`
> 以及 EDV151 的 `flash.html / monitor.html / manifest.json / edk60_edv151_vital.bin / radar_*.py`。

---

## 二、版本 1.0.0 的依据

| 依据 | 证据 |
|---|---|
| **已上板验证** | 2026-10-27 在 EDK60_VA V1.0 + EDX106 实物上跑通：I2C 内部上拉 100 kHz 一次通过（`i2c ok freq=100000 … pullup=internal`）、13 个寄存器回读一致（`cfg ok` / `cfg state verified=1`）、**1.0 m 处稳定触发**、橙灯随触发点亮、闭锁期内连挥不重复触发 |
| 关键控制位 | `0x6c=0x53(mode_reg=1)`、`0x6e=0xe8(thr_reg=1)`、`0x45=0x01(delay_reg=1)` 三者齐备（缺任一则 SET thr / SET delay 不生效） |
| 定稿参数 | 常规 normal · thr=70 · delay=2000 ms · lockout=2000 ms · LED 跟随 · I2C 100 kHz 内部上拉 |
| 构建 | demo 5.5 `door_lock_edx106`，合并固件 254,736 B；工程 20 个 demo 口径 |
| 版本语义 | 与前三套网页（track / level / gesture）一致：**首个对外可用的实机验证版本 = 1.0.0** |

> ⚠ 未做的事（不要对外声称）：未做长时间连续运行统计、未做电池供电/功耗实测、未做深睡唤醒。
> 探测距离只讲实测到的 **≥1.0 m**，不要外推。

---

## 三、预期线上 URL（push 后自动部署，3~5 分钟生效）

| 资源 | URL |
|---|---|
| 网页测试页 | `https://www.easydetek.com/edk60/edx106/test.html` |
| 网页烧录页 | `https://www.easydetek.com/edk60/edx106/flash.html` |
| 烧录 manifest | `https://www.easydetek.com/edk60/edx106/manifest.json` |
| 固件 bin | `https://www.easydetek.com/edk60/edx106/edk60_edx106_door_lock.bin` |
| 首页入口（当前版） | `/opensource/开源硬件/EDK60/` 的「应用案例 / 快速上手 / 在线工具」四入口 |
| 首页入口（版本快照） | version-1.0.0 镜像同四处 |
| 实施指南 | `/opensource/开源硬件/EDK60/door-lock-guide`（技术文档工程师提供，本次未创建） |
| 应用案例 | `/blog/edk60-edx106-door-lock`（技术文档工程师提供，本次未创建） |

### 3.1 首页改动（两份 index.md 镜像同改，共 4 处入口 + 1 处计数）

| 位置 | 改动 |
|---|---|
| 示例工程 | demo 数 **19 → 20**，雷达桥描述补「EDX106 门锁感应 I2C 主控」 |
| 应用案例 | 新增 EDK60 + EDX106 门锁感应方案（指向 `/blog/edk60-edx106-door-lock`） |
| 快速上手 | 新增 门锁感应实施指南（EDX106）（指向 `./door-lock-guide`） |
| 在线工具 | 新增「门锁感应固件（EDK60 + EDX106）」小节：烧录页 + 测试页两条 |

改动文件：
- `opensource_docs/开源硬件/EDK60/index.md`
- `opensource_versioned_docs/version-1.0.0/开源硬件/EDK60/index.md`

> 两份文件改动后内容**完全一致**（SHA256 相同）；均保持无 BOM、LF 换行（与既有文件一致）。

---

## 四、构建结果

```powershell
$nodeDir = "C:\Users\fuhua\AppData\Local\Temp\docusaurus-node20\node-v20.19.5-win-x64"
cd C:\Users\fuhua\sync\easydetek\0.project\20260810-docs.easydetek.com
& "$nodeDir\node.exe" node_modules\@docusaurus\core\bin\docusaurus.mjs build `
    --locale zh-Hans --out-dir "$env:TEMP\dsbuild-edx106"
```

- 结果：**exit code 0 · `[ERROR]` 0 条 · `[WARNING]` 0 条 · `[SUCCESS] Generated static files`**
  （日志全文：`%TEMP%\edx106-build.log`）
- `onBrokenLinks: 'throw'` 下未报 broken link，说明 `/blog/edk60-edx106-door-lock` 与
  `./door-lock-guide` 两个并行交付物**已落库**，两个入口**无需临时注释**。
- 产物目录：`%TEMP%\dsbuild-edx106`（避开 OneDrive 锁 `build/img` 的 EPERM 问题）。

### 4.1 构建产物抽查（构建后逐项 grep 确认，全部 True）

| 检查 | 结果 |
|---|---|
| `opensource/next/开源硬件/EDK60/index.html` 含 `edx106/flash.html` | ✅ |
| 同上含 `edx106/test.html` | ✅ |
| 同上含 `door-lock-guide` | ✅ |
| 同上含 `edk60-edx106-door-lock` | ✅ |
| 同上含「20 个」 | ✅ |
| `edk60/edx106/` 下 4 个静态文件（254,736 / 16,515 / 283 / 137,772 B） | ✅ |
| `blog/edk60-edx106-door-lock/index.html` 已生成 | ✅ |

---

## 五、网页测试页（test.html）实现要点

### 5.1 结构（满视口应用壳，照 `release/gesture.html`）

- 顶栏吸顶：品牌行 + 状态条（连接状态 / 行率 / 有效行 / 坏行 / 未识别 / 已触发 / 当前电平 / 最后一行 / 已下发条数 / 自检窗口）+ 控制行
- 左列：大字状态「有人 / 无人」+ 高电平倒计时 → **触发时间线**（横向条带）→ 统计卡 → 原始事件流（定高、内部滚动、可折叠）
- 右列（独立滚动）：连接参数 / 参数配置 / LED 测试 / 读回配置 / 重新初始化 / 写入记录 / 页内自测

### 5.2 读 + 写双向的纪律（本 demo 与前三套的根本差异）

1. **默认只读**：不点任何写按钮时，页面不发送任何 `SET / LED / RESET`（只读命令 `CFG? / TRIG? / SCAN / LED?` 也只在点按钮时发一次）。
2. **写前必确认**：任何写命令先弹出清单，逐条列出**将要发送的原始命令**与**期望回执**；点「取消」= 一个字节都不发。
3. **逐条等回执**：`SET` 等 `EDX106: set <key>=<val> ok rd=…`；单条超时 **800 ms** → **立即中止**，把失败项与未发送项列在右侧「写入记录」里。
4. **不重试、不猜测、不连发**：一次写请求失败后必须人工重新发起。

### 5.3 三个必须体现在界面里的已知事实

| 事实 | 界面表现 |
|---|---|
| 开机自检期 OUT 恒高 ≈3000 ms，下降沿以 `trig=0 width=1xx` 出现 | 判定 `width < 500 ms && t ≤ 自检窗口结束 + 250 ms` → 时间线画**灰段**并标「自检」，不计入触发次数与宽度统计；顶栏与大字旁常驻「自检期，非触发」标签 |
| 高电平宽度**大于** delay 是正常（期间再次触发顺延） | 不设任何"异常"标记；倒计时文案直接写「宽于设定是正常的：期间再次触发会顺延」；统计里只给「相对 delay 平均偏差」 |
| `SET` 不落 NVS，复位/上电回默认 | 右列参数面板常驻橙色提示；确认弹窗里再提示一次；`RESET` 按钮单独一个面板并说明会重跑启动序列 |

### 5.4 页内自测：15 条 + 13 项端到端扩展验收（**全部通过**）

Node 桩环境（无浏览器、无串口）跑页面真实脚本，结果：

```
PASS 15/15 条用例通过（页内自测）
✔ C1  启动横幅解析（i2c ok / id / ctrl / cfg ok / run / ready）
✔ C2  trig=1 / trig=0 配对算宽度（t=6149 → t=8514 ⇒ 2364 ms）
✔ C3  自检期识别：width=141 且 t≤自检窗口 ⇒ 自检（非触发）＋两个反例
✔ C4  stat 行解析（interval/trig/width_min/width_max/i2c_err）
✔ C5  set … ok rd= 回执匹配（六类命令 + err 回执 + 交叉误配）
✔ C6  led? 字段解析 + LED on/off 回执配对
✔ C7  scan found=0x28（含 scan done 计数与 err 干扰）
✔ C8  坏行 / 半行 / 噪声后重同步
✔ C9  闭锁违例计算（间隔 < lockout 记违例；自检段不参与；边界等于不算）
✔ C10 写命令必须经确认：未确认时下发计数 = 0（含"无通道时不得报成功"）
✔ C11 trig? 行解析（level/active/last_trig_ms/count/selfcheck_dropped）
✔ C12 宽度 > delay 属正常（不判异常，只在偏差里体现）
✔ C13 CFG? 寄存器回读与期望值比对（13 行；thr 改动 / 0x6e bit6=0 均能报不一致）
✔ C14 命令清单格式与回执一一对应（含越界夹取、四舍五入、未知模式回落）
✔ C15 不变量：默认只读 / 无直发字节入口 / 定稿默认参数 / 命令不含换行
```

扩展验收（13/13 通过，覆盖页内自测够不到的端到端行为）：

| # | 验收项 | 结果 |
|---|---|---|
| A1 | 确认后逐条下发并逐条收到回执 | sent 6/6 |
| A2 | 下发计数递增（0 → 6） | ✅ |
| A3 | 回执把观测值写回会话（thr/delay/lockout/mode/gain/win） | ✅ |
| B1 | 无回执时 **800 ms 超时**并中止（第 2 条不再发） | elapsed 811 ms，实际写入 1 条 |
| B2 | 实际写出的字节 = `命令 + \n`，无多余字节 | `"SET thr=70\n"` |
| C1 | 模拟脚本：自检段 1 段 + 真触发 4 次 | ✅ |
| C2 | 模拟脚本宽度 min/max 与 stat 行一致（1030 / 2950） | ✅ |
| C3 | 模拟脚本闭锁违例被识别（间隔 1600 < 2000） | 1 次 |
| C4 | 模拟脚本自检段 width=141 被标灰（非触发） | ✅ |
| C5 | 状态条/大字/统计/时间线/寄存器表渲染冒烟 | 无异常 |
| D1 | 只读命令（`CFG?` + `TRIG?`）无需确认即可执行 | ✅ |
| D2 | `CFG?` 回读 13 个寄存器且与定稿期望一致 | 13 行 0 不一致 |

静态检查（harness 自动跑）：

| 检查 | 结果 |
|---|---|
| 单文件 / 无 `<script src>` / 无 `<link>` / 无 `@import` / 无绝对 URL | ✅ 通过 |
| JS 里无 `localStorage / sessionStorage / indexedDB / document.cookie` | ✅ 通过 |
| 未处理的 Promise 拒绝 | 0 |

> 自测器脚本（`run.js`）**不属于发布物**，存放在系统临时目录，未提交仓库。

### 5.5 给截图脚本的推荐调用序列

页面在 `window.EDX106` 暴露了只读调试入口（写操作一律走 `requestWrite`，没有"裸发字节"的口子）。
截图脚本建议按下列顺序驱动，保证每张图都是「有数据、有意义」的状态：

```js
// ① 满视口空态（顶栏 + 左列空时间线 + 右列配置，证明"默认只读、已下发 0 条"）
await page.goto('file:///…/release/edx106/test.html');
await page.waitForTimeout(400);
await page.screenshot({ path: 'edx106-01-idle-1920x1080.png' });

// ② 启动自检完成后的行率/状态条（自检期标签可见时抓）
await page.evaluate(() => window.EDX106.startSimulation());
await page.waitForTimeout(1200);          // 横幅 + 自检虚警（width=141，灰段）
await page.screenshot({ path: 'edx106-02-selfcheck-1920x1080.png' });

// ③ 多次触发 + 时间线全貌（等模拟脚本跑到 4 次真触发之后）
await page.waitForTimeout(6000);
await page.screenshot({ path: 'edx106-03-timeline-1920x1080.png' });

// ④ 「有人」大字 + 倒计时进行中（在触发上升沿后 500 ms 内抓）
await page.evaluate(() => {
  const S = window.EDX106.state;
  window.__shot = setInterval(() => {
    if (S.level === 1) { clearInterval(window.__shot); }
  }, 50);
});
await page.waitForFunction(() => window.EDX106.state.level === 1, { timeout: 15000 });
await page.screenshot({ path: 'edx106-04-person-high-1920x1080.png' });

// ⑤ 确认弹窗（写前必须确认的证据图）：点「下发配置…」后立刻抓
await page.click('#btn-apply');
await page.waitForSelector('#confirmDlg:not([hidden])');
await page.screenshot({ path: 'edx106-05-confirm-dialog-1920x1080.png' });
await page.click('#cfOk');                 // 模拟模式下会逐条回执
await page.waitForTimeout(1200);
await page.screenshot({ path: 'edx106-06-write-done-1920x1080.png' });

// ⑥ 寄存器表（读回配置，13 行全 ✓）
await page.click('#btn-readcfg');
await page.waitForTimeout(1000);
await page.screenshot({ path: 'edx106-07-regs-1920x1080.png' });

// ⑦ 页内自测结果面板（15/15）
await page.click('#btn-selftest2');
await page.waitForTimeout(800);
await page.screenshot({ path: 'edx106-08-selftest-1920x1080.png' });

// ⑧ 1280×720 矮屏回归（时间线仍可用、日志可折叠）
await page.setViewportSize({ width: 1280, height: 720 });
await page.waitForTimeout(400);
await page.screenshot({ path: 'edx106-09-timeline-1280x720.png' });
```

注意事项：① 截图前先 `window.EDX106.stopSimulation()` 再重新 `startSimulation()` 可复位模拟脚本；
② ④ 依赖异步等待，不要用固定 sleep 猜时间；③ 以上均为模拟模式，**不需要真实硬件**。

---

## 六、烧录页（flash.html）要点

照 `release/edv163-gesture/flash.html` 同构改写（同一套内联 CSS + 同一行
`unpkg esp-web-tools@10`），内容替换为 EDX106 事实：

1. 页首绿色成功块：`✅ 已在 EDK60 + EDX106 实物上跑通（2026-10-27）：1.0 m 处稳定触发、橙灯随触发点亮、19 项上板判据通过`
2. 开始前三件事：模组插 **H3** 5 根线（V/G/SCL→T/SDA→A/OUT→A2）、**不需要任何外部电阻**（内部上拉 + 100 kHz）、USB-C + Chrome/Edge
3. 烧录三步 + 烧完 6 步（拔插 → RADAR 红灯亮 → 打开 `./test.html` → 选 115200 → 看三行 `i2c ok` / `ctrl … thr_reg=1 … delay_reg=1` / `cfg ok` → 1 m 处挥手看「有人」+ 灯亮）
4. 固件说明表：纯 I2C 主控（非透传）、每次上电自动重配 + 回读校验、定稿参数（常规/thr70/2000/2000）、demo 5.5 / 共 20 个、探测距离 ≥1.0 m
5. FAQ **6 条**：①上电前 3 秒「有人」是自检 ②`SCAN` 扫不到 0x28 四步排查 ③只能测 30 cm = ULP 模式 ④宽度大于设定延时是二次触发顺延 ⑤`SET` 参数掉电丢失（设计如此） ⑥不能与 EDV11P/EDV163 同时插

---

## 七、上板复验清单（8 条，**push 之后必须由人复跑一遍**）

| # | 复验项 | 通过判据 |
|---|---|---|
| 1 | 网页烧录 | 用 `flash.html` 从零烧录成功，出现「完成」；拔插后串口重新枚举 |
| 2 | 启动横幅 | 连上 `test.html` 后依次看到 `i2c ok freq=100000 … pullup=internal`、`id reg0x0d=0x0a expect=0x0a`、`cfg ok thr=70 delay=2000 lockout=2000 mode=normal light=off` |
| 3 | 三个控制位 | `ctrl` 行里 `mode_reg=1`、`thr_reg=1`、`delay_reg=1` 全为 1 |
| 4 | 自检期标注 | 上电后 3 秒内的时间线出现**灰段**并标「自检」，`已触发` 计数保持 0 |
| 5 | 触发与亮灯（1 m） | 正对 1.0 m 挥手：大字变「有人」、倒计时开始、**橙灯亮**；松手后 `trig=0 width≈2000~2400`、灯灭 |
| 6 | 宽度 > delay | 连续挥动时出现 `width≈2900`，页面**不标异常**，统计里体现为正的平均偏差 |
| 7 | 写通道（改 thr） | 点「下发配置」→ 确认弹窗逐条列出 6 条 `SET` → 确认后 6 条全部 `ok rd=`；随后 `CFG?` 里 `reg 0x0f` 与页面期望一致 |
| 8 | 掉电回默认 | 拔插 USB 后 `run` 行回到 `thr=70 delay=2000 lockout=2000 mode=normal`，页面「写入记录」提示参数已回默认 |

复验记录建议贴在 `docs/project/edx106/release-package-notes.md` 末尾（追加一节「复验记录」）。

---

## 八、发布动作（**待用户批准，未执行**）

| 步骤 | 命令 | 状态 |
|---|---|---|
| 1 | `git -C <文档站> status --short` 复核改动文件（应只有 2 个 index.md + 新增 `static/edk60/edx106/`） | ⬜ 待批准 |
| 2 | `git add static/edk60/edx106 opensource_docs/.../index.md opensource_versioned_docs/.../index.md`（含并行的 door-lock-guide.md 与博客页） | ⬜ 待批准 |
| 3 | `git commit -m "feat(edk60): EDX106 门锁感应固件 1.0.0（网页测试页 + 烧录页 + 静态资源）"` | ⬜ 待批准 |
| 4 | `git push`（部署由 `.github/workflows/deploy.yml` 触发，3~5 分钟生效） | ⬜ 待批准 |
| 5 | 上线后按第七节 8 条复跑一次复验清单 | ⬜ 待批准 |

> 本轮**没有执行任何 git 命令**（不 add / commit / push / tag）。

---

## 九、已知限制与后续建议

1. **test.html 的写入能力未在真实硬件上跑过写流程**：页内自测与 Node 桩环境覆盖了确认门禁、回执匹配、800 ms 超时中止；
   真实 `SET` 回执的时序（USB CDC 缓冲 + 固件 40~200 ms 出字）需第七节第 7 条现场确认。
2. **自检窗口长度按 `boot … selfcheck=3000ms` 行取**；若以后改 Kconfig 里的自检时间，页面会自动跟随（不需要改代码）。
3. **`LED blink` 回执约 1.2 s 后到**，页面给这个命令单独放宽到 3000 ms 超时；`CFG?` 收集窗口 3000 ms、`SCAN` 8000 ms、`RESET` 15000 ms。
4. 页面不复刻固件内部的 `poll_resync` 纠偏逻辑，只做展示（`TRIG?` 里的 `poll_resync` 原值透出）。
5. 未做英文站（`i18n/en/`）翻译，访问 `/en/...` 会回落中文 —— 与前三个网页一致。
