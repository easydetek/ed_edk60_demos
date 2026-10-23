# EDV163 水位监测（EDK60）发布包说明 — v0.9.0（未实机验证）

| 项 | 内容 |
|---|---|
| 文档编号 | EDV163-REL-001 |
| 版本 | 1.0（2026-10-23） |
| 对应固件版本 | **0.9.0**（未在 EDV163 实物上实测，故不发布 1.0.0） |
| 责任角色 | 全栈工程师（opcs-fullstack-engineer，加载 `fs-frontend`） |
| 工程根 | `C:\Users\fuhua\sync\easydetek\0.project\OK\20261014-ed_edk60_demos\ed_edk60_demos` |
| 文档站 | `C:\Users\fuhua\sync\easydetek\0.project\20260810-docs.easydetek.com` |
| 状态 | 本地已完成、**构建通过、未 push**（等用户批准） |

---

## 1. 版本号约定

| 版本 | 含义 | 何时可用 |
|---|---|---|
| **0.9.0** | 桥接固件 + 网页工具已完成、**无硬件自测通过**，但**尚未在 EDV163 实物上验证** | **当前发布的就是这一档** |
| 1.0.0 | 已在 EDV163 实物上跑通：能收到合法 `0xA3/0x81` 帧、水位曲线与实测水位一致、连续运行无坏帧 | 实机验证后由用户确认再升 |

**文案口径（所有材料必须一致）**：
> 本固件与页面**尚未在 EDV163 实物上实测**（当前验证的是桥接实现与解析工具的自测），实测数据待补。

任何页面/文档**不得**出现「实测通过」「已验证」等表述。当前可引用的验证只有两类：

| 已有验证 | 证据 | 边界 |
|---|---|---|
| 解析工具自测 | `tools/edv163_frame_check.py --selftest` 通过 | 纯软件、合成帧 |
| 网页自测 | `release/level-selftest.md`：帧解析 **10/10**、整页桩环境 **44/44**、布局与不变量契约 **32/32** | 桩环境（假串口/假时钟/DOM+Canvas 桩），**未用真实浏览器与真实硬件** |

---

## 2. 发布包文件清单（`release/edv163/`）

| 文件 | 体积 | SHA256 | 说明 |
|---|---|---|---|
| `edk60_edv163_level.bin` | 244,192 B | `72E600D09B756883F02B14828B92CEA3528A31A3416F613437DF54B66307381B` | 合并固件，烧 **0x0**；由 `build-radar_bridge_edv163\merged-binary.bin` **直接复制**（未重新生成），复制后 SHA256 与源**逐字节一致** |
| `flash.html` | 12,435 B | `06372101B32E7FD1F9333E82581AA0BFFD9C0B14B1D4B94ED27A0E187873840D` | 网页烧录页（水位场景改写），页首含醒目警示块 |
| `level.html` | 118,537 B | `EF2D790046E9300FAA28B40E18CC544FFA1E047D48B804466F12251A90731693` | 网页水位演示，由 `release/level.html` **原样复制**（未改一字节） |
| `manifest.json` | 279 B | `0EAC583D948DD2E2F54650944C584E6619B478F18762CE413149D3D81620B558` | esp-web-tools 清单 |

### 2.1 manifest.json 关键字段

```json
{
  "name": "EDK60 水位监测雷达固件",
  "version": "0.9.0",
  "new_install_prompt_erase": true,
  "builds": [{ "chipFamily": "ESP32-C6",
               "parts": [{ "path": "edk60_edv163_level.bin", "offset": 0 }] }]
}
```

`chipFamily` / `offset` / `new_install_prompt_erase` 与既有 `release/edv11p/manifest.json` 完全同构，仅名称、版本、文件名不同。

### 2.2 flash.html 与 EDV11P 版的同构与差异

| 项 | 保持同构 | 差异（换成水位场景） |
|---|---|---|
| 内联 CSS（CSS 变量 / `.wrap` / `.box` / `table` / `.foot` 等） | ✅ 逐条沿用 | 新增 `.box.danger`（页首警示块）与 `.no`（红色「未实测」） |
| esp-web-tools 引用 | ✅ 同一行 `https://unpkg.com/esp-web-tools@10/dist/web/install-button.js?module` | — |
| 段落骨架 | ✅ 开始前三件事 → 烧录三步 → 本次写入内容 → 烧完之后 → 这套固件做什么 → 常见问题 → foot | 文案全部改为 EDV163/水位 |
| 串口参数 | — | **256000 8N1**（EDV11P 是 921600），文档内明确标注「不是 921600」 |
| 演示页链接 | — | `./track.html` → `./level.html` |
| 验证状态行 | — | EDV11P 写「30 分钟实测 100%」；EDV163 写 **❌ 尚未在实物上验证** + 已有自测清单 |
| FAQ 条数 | 5 条 | ①串口助手乱码正常 ②插 H4 不行 ③网页连不上（浏览器/非 https）④必须 256000 ⑤静水面被判「无人」正常、不影响水位 |

页首警示块原文（截图/校对基准）：

> ⚠️ 本固件与页面**尚未在 EDV163 实物上实测**（当前验证的是桥接实现与解析工具的自测），实测数据待补；如遇问题请反馈 support@easydetek.com

---

## 3. 文档站静态资源（`static/edk60/edv163/`）

4 个文件**原样复制**，逐个核对 SHA256：

| 文件 | 文档站体积 | SHA256 与工程内一致 |
|---|---|---|
| `edk60_edv163_level.bin` | 244,192 B | ✅ |
| `flash.html` | 12,435 B | ✅ |
| `level.html` | 118,537 B | ✅ |
| `manifest.json` | 279 B | ✅ |

既有资源**未改动**（只新增了 `edv163/` 子目录）：
`static/edk60/{flash.html, monitor.html, manifest.json, edk60_edv151_vital.bin, radar_*.py}` 与
`static/edk60/edv11p/{flash.html, track.html, manifest.json, edk60_edv11p_track.bin}` 全部保持原状。

### 3.1 预期线上 URL

| 用途 | URL |
|---|---|
| 网页烧录（水位固件） | `https://www.easydetek.com/edk60/edv163/flash.html` |
| 网页水位演示 | `https://www.easydetek.com/edk60/edv163/level.html` |
| esp-web-tools 清单 | `https://www.easydetek.com/edk60/edv163/manifest.json` |
| 合并固件（烧 0x0） | `https://www.easydetek.com/edk60/edv163/edk60_edv163_level.bin` |

`flash.html` 内以相对路径引用 `manifest.json` 与 `./level.html`，故三者必须同目录，已满足。

---

## 4. 首页入口（各 1 处，共 2 个文件）

改的是「开放硬件 / EDK60 / index.md」，**当前版与 version-1.0.0 镜像各一份，内容完全一致**：

1. `opensource_docs\开源硬件\EDK60\index.md`
2. `opensource_versioned_docs\version-1.0.0\开源硬件\EDK60\index.md`

每个文件补 **3 条**入口（应用案例 1 + 快速上手 1 + 在线工具 2 个链接），共 **4 行**：

| 段落 | 新增内容 |
|---|---|
| 应用案例 | `[EDK60 + EDV163 水位监测方案](/blog/edk60-edv163-water-level)` —— 吊装向下测距，水位 = 挂高 − 距离；含协议要点与转义处理（**待实机验证**） |
| 快速上手 | `[水位监测实施指南（EDV163）](./level-guide)` —— 接线 → 烧录 → 读水位曲线的完整配方（待实机验证） |
| 在线工具 | 新增小标题 `**水位监测固件（EDK60 + EDV163）**` + `[网页烧录 · 水位监测固件](pathname:///edk60/edv163/flash.html)` + `[网页水位演示](pathname:///edk60/edv163/level.html)` |

- 静态资源链接用 `pathname:///`（Docusaurus 对 `static/` 下文件的要求），与 EDV11P 写法一致。
- `level-guide.md` 与博客页由技术文档工程师并行创建，本角色**未创建、未修改**这两个文件。
- 两个 md 均**无 UTF-8 BOM**（改写前后保持一致，避免无意义 diff）。

### 4.1 一处已发现但未擅改的不一致（待用户决定）

首页「示例工程」段仍写「**17 个 ESP-IDF demo**」，而工程实为 **18 个**（新增 `radar_bridge_edv163`，编号 5.3）。
该数字不在本次职责范围内、且属于另一条既有内容，故**未改动**，留待用户批准后一并修正（两份 index.md 同改）。

---

## 5. 构建与产物校验

命令（Node 不在 PATH，用临时 Node 20；`--out-dir` 必须指到临时目录，仓库在同步盘上会 EPERM）：

```powershell
$nodeDir = "C:\Users\fuhua\AppData\Local\Temp\docusaurus-node20\node-v20.19.5-win-x64"
cd C:\Users\fuhua\sync\easydetek\0.project\20260810-docs.easydetek.com
& "$nodeDir\node.exe" node_modules\@docusaurus\core\bin\docusaurus.mjs build `
    --locale zh-Hans --out-dir "$env:TEMP\dsbuild-edv163"
```

| 轮次 | 结果 | 说明 |
|---|---|---|
| 第 1 次 | ❌ 失败（2 条 broken link） | 全部来自并行任务尚未交付的文件：`/blog/edk60-edv163-water-level`（博客页未创建）与 `./level-guide`（实施指南未创建），当前版与 versioned 各报 1 条，共 4 行 |
| 第 2 次（轮询等并行文件落地后重试，约 5 分钟） | ✅ **0 error（exit code 0）** | 技术文档工程师已交付 `opensource_docs/开源硬件/EDK60/level-guide.md`、`opensource_versioned_docs/version-1.0.0/开源硬件/EDK60/level-guide.md` 与博客页 `blog/2026-10-22-edk60-edv163-water-level.md`（`slug: edk60-edv163-water-level`），4 条 broken link 全部消失；输出 `[SUCCESS] Generated static files in "...\dsbuild-edv163"`，**无 error、无 warning** |

### 5.1 产物核对（第 2 次构建，全部通过）

| 产物（`$env:TEMP\dsbuild-edv163\edk60\edv163\`） | 存在 | 体积 | SHA256 与源一致 |
|---|---|---|---|
| `flash.html` | ✅ | 12,435 B | ✅ |
| `level.html` | ✅ | 118,537 B | ✅ |
| `manifest.json` | ✅ | 279 B | ✅ |
| `edk60_edv163_level.bin` | ✅ | 244,192 B | ✅ |

### 5.2 入口渲染核对（读构建产物 HTML 确认）

| 构建页 | edv163/flash.html | edv163/level.html | level-guide | 博客页 | 「待实机验证」 |
|---|---|---|---|---|---|
| `opensource/开源硬件/EDK60/index.html`（当前版） | ✅ | ✅ | ✅ | ✅ | ✅ |
| `opensource/next/开源硬件/EDK60/index.html`（镜像版） | ✅ | ✅ | ✅ | ✅ | ✅ |
| `opensource/开源硬件/EDK60/level-guide/index.html` | ✅ | ✅ | — | — | — |
| `blog/edk60-edv163-water-level/index.html` | 已生成 | | | | |

即：三条入口（应用案例 / 快速上手 / 在线工具）在当前版与镜像版**均已实际渲染出可点击链接**，无 404。

---

## 6. 待用户批准后才 push（本角色未执行任何 git 命令）

严格按要求：**本角色未运行任何 `git` 命令**（未 add / 未 commit / 未 push）。以下为待用户批准后的动作清单：

| # | 仓库 | 变更 | 状态 |
|---|---|---|---|
| 1 | 工程 `ed_edk60_demos` | 新增 `release/edv163/`（4 文件）+ `docs/project/edv163/release-package-notes.md` | 本地就绪 |
| 2 | 文档站 `20260810-docs.easydetek.com` | 新增 `static/edk60/edv163/`（4 文件）；改 2 个 `index.md`；技术文档工程师并行新增 `level-guide.md`（2 份）与博客页 | 本地就绪 |

push 到文档站即触发 `.github/workflows/deploy.yml`（或 gitee 远端流程）自动部署，3~5 分钟生效；
**因此 push 前必须确认**：
1. 构建 0 error（或 broken link 已在并行文件落地后消失）；
2. 文案里「未实机验证」的表述未被改动掉；
3. 既有 EDV151/EDV11P 资源与页面无回归（本次仅新增 + 追加行，无删除）。

---

## 7. 实机验证清单（拿到已换固件的 EDV163 后照此跑一遍）

| # | 步骤 | 通过判据 |
|---|---|---|
| 1 | 网页烧录 `edk60_edv163_level.bin` 到 EDK60 | 进度条完成，拔插后 RADAR 红灯常亮 |
| 2 | 打开 `level.html` → 连接串口（256000） | 状态条显示「已连接」、波特率 **256000**、收字节数增长 |
| 3 | 观察是否收到帧 | 帧率 > 0；坏帧 / 转义错误**不持续增长** |
| 4 | 10 秒无数据则点「下发配置…（需确认）」 | 6 条 SET 全部收到 `0xA2` 应答；之后开始出帧 |
| 5 | 核对挂高 | 顶栏「使用挂高」与设备 `0x25` 回读一致（不一致时页面会标红） |
| 6 | 量实测水位（卷尺）对比曲线 | 误差在 ±8 cm（模组距离分辨率）内视为通过 |
| 7 | 静水面场景 | `presence=0` 时曲线仍继续（判「无人」属正常） |
| 8 | 连续跑 30 分钟 | 记录帧数 / 坏帧数 / 帧完整率 —— **这组数据是升到 1.0.0 的依据** |

验证结论回写本文件 §1 与 `release/edv163/flash.html`、`manifest.json`（版本升 1.0.0）、
以及首页入口里「（待实机验证）」的字样。
