# EDV11P 线上烧录 + 线上演示 发布包说明

- 版本：**1.0.0**（与既有 EDV151 demo 的 `release/` 包同结构、同版本号起点）
- 生成时间：2026-10-22
- 负责人：全栈工程师（opcs-fullstack-engineer）
- 交付形态：2 份完全相同的静态文件组（工程内 `release/edv11p/` + 文档站 `static/edk60/edv11p/`）

---

## 1. 文件清单与校验

### 1.1 工程内发布包：`ed_edk60_demos\release\edv11p\`

| 文件 | 体积 (B) | SHA256 | 来源 |
|---|---:|---|---|
| `edk60_edv11p_track.bin` | 244,192 | `C847423F1FFDF7482934355432907AE1A788533AF121DE997C96658549A7B40E` | 复制自 `build-ev11p-final\merged-binary.bin`（字节级一致） |
| `manifest.json` | 286 | `82134B127D3862D93F9E8873E680C9E5D618D751C9D377E62BF6AF1C5425812A` | 新写（以既有 manifest 为模板） |
| `flash.html` | 10,019 | `8671034DE213740B2A97F9BB92BD58D624C304972693F06C0C79EEC7ADC4A2E3` | 以既有 `release\flash.html` 为模板改写（同排版/同内联 CSS/同 esp-web-tools 用法） |
| `track.html` | 91,706 | `96ADAE21A25B77CA8C3D5CC081F16624B332BB5638E1845CB3EFC9F47F982539` | **原样复制** `release\track.html`（已验收版本，未改一字节） |

### 1.2 文档站静态资源：`20260810-docs.easydetek.com\static\edk60\edv11p\`

同上 4 个文件，逐文件 SHA256 与 1.1 **完全一致**（已用 `Get-FileHash` 双向比对，见 §4.2）。

> `static\edk60\` 下既有的 `flash.html` / `monitor.html` / `manifest.json` / `edk60_edv151_vital.bin` / 3 个 `.py`
> **未做任何改动**（修改时间仍为 2026-10-17/18，体积未变），本次只**新增** `edv11p\` 子目录。

---

## 2. 版本号与命名约定

| 项 | 约定 | 本次取值 |
|---|---|---|
| 固件发布版本 `version` | 与 `manifest.json` 的 `version` 严格一致；随透传桥固件行为变化（引脚/波特率/握手/透传语义）升版 | `1.0.0` |
| 模块型号 | 由模组回读决定，**不写死**在固件版本里 | `EDV11P_VA`，回读 `V1.3.3, 0x95F0` |
| 文件命名 | `<板卡紧凑名>_<模组型号>_<用途>.bin`，全小写+下划线 | `edk60_edv11p_track.bin` |
| 用途后缀词表 | `track`＝人数统计/轨迹（本包）；后续如出跌倒版可加 `fall`，**不与既有 `vital`（EDV151）混名** | `track` |
| 烧录偏移 | 全项目统一使用 `idf.py merge-bin` 的一次性合并固件，**offset 固定 `0x0`** | `0x0` |
| 目录隔离 | 一个 demo 一个子目录（`release\edv11p\` ↔ `static\edk60\edv11p\`），页内相对链接只在目录内生效 | 已遵守 |
| 页内相对引用 | `manifest="manifest.json"`、`href="./track.html"`（目录内相对，工程内与线上都成立） | 已遵守 |

**升级路径**：新固件（如 `1.0.1`）→ 重新 `merge-bin` → 覆盖 `edk60_edv11p_track.bin` + 改 `manifest.json` 的 `version` → 同步 `static\edk60\edv11p\` → 提交文档站仓库（push 即自动部署）。

---

## 3. 线上 URL 预期

站点根 `https://docs.easydetek.com`（GitHub/Gitee Pages，Docusaurus static 直出）：

| 用途 | 线上 URL |
|---|---|
| 烧录页 | `https://docs.easydetek.com/edk60/edv11p/flash.html` |
| 演示页（轨迹/人数统计） | `https://docs.easydetek.com/edk60/edv11p/track.html` |
| manifest | `https://docs.easydetek.com/edk60/edv11p/manifest.json` |
| 固件 | `https://docs.easydetek.com/edk60/edv11p/edk60_edv11p_track.bin` |

**在 Markdown 文档里引用必须用绝对静态路径**（文档站规则，普通 markdown 链接不生效）：

```markdown
[网页烧录](pathname:///edk60/edv11p/flash.html)
[网页轨迹演示](pathname:///edk60/edv11p/track.html)
```

**Web Serial 前提**：页面必须以 `https://`（线上即为 https）或 `http://localhost` 打开；
`file://` 直接双击打开时烧录/串口按钮会不可用 —— `flash.html` 的「常见问题 3」已写明并给出本地 `py -3 -m http.server` 方案。

---

## 4. 自检结果

### 4.1 本地静态服务（`py -3 -m http.server 8771 --bind 127.0.0.1 --directory release\edv11p`）

| 请求 | HTTP | 返回字节 | 与磁盘一致 |
|---|---:|---:|:--:|
| `/flash.html` | 200 | 10,019 | ✅ |
| `/track.html` | 200 | 91,706 | ✅ |
| `/manifest.json` | 200 | 286 | ✅ |
| `/edk60_edv11p_track.bin` | 200 | 244,192 | ✅ |

- 服务端下载的 bin 重新计算 SHA256 = `C847423F…B40E`，与源文件一致 ✅
- bin 首 8 字节 = `e9 03 02 10 1a b9 86 40`（ESP 镜像魔数 `0xE9` 起始），说明 `offset 0x0` 放的是合法合并固件 ✅

### 4.2 复制一致性（`Get-FileHash` 源 ↔ 文档站）

| 文件 | 源 SHA256 前 16 位 | 文档站 SHA256 前 16 位 | 结论 |
|---|---|---|---|
| `edk60_edv11p_track.bin` | `C847423F1FFDF748` | `C847423F1FFDF748` | MATCH ✅ |
| `manifest.json` | `82134B127D3862D9` | `82134B127D3862D9` | MATCH ✅ |
| `flash.html` | `8671034DE213740B` | `8671034DE213740B` | MATCH ✅ |
| `track.html` | `96ADAE21A25B77CA` | `96ADAE21A25B77CA` | MATCH ✅ |

### 4.3 manifest 合法性

- `ConvertFrom-Json` 解析通过；另用 Python `json.loads` 复核通过 ✅
- **UTF-8 无 BOM**（`raw[:3] != EF BB BF`），中文 `name` 码点正确 ✅
- `name` = `EDK60 人数统计/轨迹雷达固件`、`version` = `1.0.0`、`builds[0].chipFamily` = `ESP32-C6`、
  `parts[0].path` = `edk60_edv11p_track.bin`（**文件存在**）、`parts[0].offset` = `0` ✅

### 4.4 旧文案残留检查（`EDV151|心率|呼吸|体征`）

| 文件 | 命中数 | 说明 |
|---|---:|---|
| `flash.html` | **0** | ✅ 无任何 EDV151 / 体征类旧文案 |
| `manifest.json` | 0 | ✅ |
| `edk60_edv11p_track.bin` | — | 二进制，不适用 |
| `track.html` | 3 | ⚠️ **非残留，属协议枚举正文**，见下 |

`track.html` 的 3 处命中（该文件按要求**原样复制、未改一字节**）：

```
191: <option value="2">2 体征检测</option>
204: 工作模式枚举含义（0 调试 / 1 追踪·人数统计 / 2 体征 / 3 跌倒）来自协议文档，
315: WORK_MODE_NAMES: ['调试模式', '追踪/人数统计', '体征检测', '跌倒检测'],
```

这 3 处是 EDV11P 协议里**工作模式枚举值 2 的合法名称**（0 调试 / 1 追踪·人数统计 / 2 体征 / 3 跌倒），
属于雷达自身的功能枚举，不是 EDV151 演示文案；页面默认工作模式是 `1 追踪/人数统计`，与本次 demo 一致。
若要彻底不出现「体征」二字，需要改 `release\track.html` 源文件再重新复制 —— **本次按约束未改**，如需统一口径请另行确认。

---

## 5. 页面内容要点（flash.html 与事实的对应）

| 页面写到的事实 | 来源 |
|---|---|
| 合并固件 244,192 B，偏移 `0x0`，芯片 ESP32-C6 | `build-ev11p-final\merged-binary.bin` 实测体积 + manifest |
| 模组必须插 **H3**，H4 顺序相反会电源/数据互扰 | `AGENTS.md` §1 实测结论 |
| 雷达模组回读 `EDV11P_VA` / `V1.3.3` / `0x95F0` | 模组回读 |
| 串口 921600 8N1 无流控，页面已设好无需手填 | `track.html` `requestPort` 参数（`baudRate: 921600`） |
| 纯字节透传桥，解析全在上位机 | 固件设计 `radar_bridge_edv11p` |
| 30 分钟 15,483 帧 / 坏帧 0 / 重同步 0 / 完整率 100% | 上板实测 |
| 同时跟踪 1~6 人（中位 3 人） | 上板实测 |
| 演示页默认只读，写入需显式确认框确认 | `track.html` P1-06 约定（「下发配置…（需确认）」+ 二次确认） |
| 供电建议 DC-005 + 5V/≥2A（模组启动峰值 <1.2A） | 硬件实测/说明 |
| 支持邮箱 support@easydetek.com，MIT | 项目约定 |

风格一致性：与既有 `flash.html` **同一份内联 CSS**（相同 CSS 变量、`.wrap/.alert/.box/.btn-area/.foot/kbd` 等），
未引任何外部样式表；`esp-web-tools` 仍用同一条 unpkg 引用
`https://unpkg.com/esp-web-tools@10/dist/web/install-button.js?module`。

---

## 6. 待办 / 提示

| # | 事项 | 说明 |
|---|---|---|
| 1 | 文档站需要页面入口 | 本次只交付静态资源；若要让人从文档里点到，需在相关 md 里用 `pathname:///edk60/edv11p/flash.html` 加链接（属文档站改动，未在本任务范围） |
| 2 | 线上实测 | 本地 8771 服务自检已过；`https` 线上环境下的 Web Serial 烧录/串口连接建议用一台干净电脑各走一次 |
| 3 | track.html 的「体征」枚举文案 | 见 §4.4，如需统一口径要改源文件（本次按「原样复制」约束未动） |
| 4 | 清理 `edk60_edv11p_track.bin` 与 `edk60_edv151_vital.bin` 的并存 | 两个 bin 都在 `static\edk60\` 体系下但分属不同子目录，不冲突；无需处理 |
