# EDV11P 侧装人数统计 / 轨迹 demo —— 文档站发布记录（本轮：页面落库 + 本地构建验证）

| 项 | 值 |
|---|---|
| 日期 | 2026-10-22 |
| 执行角色 | 技术文档工程师 + 前端（opcs-tech-writer） |
| 本轮范围 | 文档站页面落库（recipe 页 / 博客案例 / 首页入口镜像 / 配图）+ 本地 `docusaurus build` 验证 |
| 本轮明确**未做** | 未执行任何 git 命令（add / commit / push 全未执行）、未部署、未删除任何文件 |
| 源材料仓库 | `ed_edk60_demos`（本仓库） |
| 目标仓库 | `20260810-docs.easydetek.com`（Docusaurus 3.10.2，locale `zh-Hans`） |

---

## 1. 本轮新增 / 修改的文件（全部在文档站仓库）

> 路径相对文档站根目录 `C:\Users\fuhua\sync\easydetek\0.project\20260810-docs.easydetek.com\`。

| # | 路径 | 动作 | 体积 | 说明 |
|---|---|---|---|---|
| 1 | `opensource_docs\开源硬件\EDK60\track-recipe.md` | **新增** | 57,505 B（947 行） | Recipe 页（current 源），源文件 `docs\recipe-edv11p-person-tracking.md` 改写而来 |
| 2 | `opensource_versioned_docs\version-1.0.0\开源硬件\EDK60\track-recipe.md` | **新增** | 57,505 B（947 行） | 版本镜像，与 #1 字节级完全一致（SHA256 `302B1C87…8B9F`） |
| 3 | `blog\2026-10-22-edk60-edv11p-person-tracking.md` | **新增** | 14,262 B（193 行） | 应用案例博客，源文件 `docs\case-edv11p-person-tracking.md` + 补 front-matter |
| 4 | `static\img\edk60\edv11p-track-map.svg` | **新增** | 199,270 B | 俯视轨迹图（源自 `docs\project\edv11p\figures\track-map.svg`，仅改文件名） |
| 5 | `static\img\edk60\edv11p-people-timeline.svg` | **新增** | 197,247 B | 人数时间序列（源自 `figures\people-timeline.svg`，仅改文件名） |
| 6 | `opensource_versioned_docs\version-1.0.0\开源硬件\EDK60\index.md` | **修改** | 2,093 B | 最小同步：补「应用案例」EDV11P 条目、「快速上手」recipe 条目、「在线工具」整段（共 3 条入口）。未改动其他内容 |
| 7 | `blog\tags.yml` | **修改** | 989 B | 新增 `tracking` 标签（`/case/tracking`），否则博客 front-matter 的 `tags: [radar, tracking]` 会报未定义标签 WARNING |

**未改动（按约束）**：`opensource_docs\开源硬件\EDK60\index.md`（项目经理已加好三条入口，本轮只核对不重加）、`static\edk60\` 下任何既有 EDV151 相关文件（`flash.html`、`monitor.html`、`edk60_edv151_vital.bin`、`radar_*.py` 等）。

---

## 2. 内容改写要点（工程内路径 → 读者可用表述）

Recipe 页所有工程内路径均已改写，实测数字**一个未改**（逐项复核见第 5 节）：

| 源材料里的写法 | 落库后的写法 |
|---|---|
| `release/track.html` | 站内静态页 `[📈 网页实时轨迹演示](pathname:///edk60/edv11p/track.html)` |
| 「线上烧录页地址（待补）」 | `[⚡ 网页烧录页 · 人数统计固件](pathname:///edk60/edv11p/flash.html)`；附录 C 待补 ② 标记为**已闭环** |
| `build-ev11p-final/merged-binary.bin` | `[⬇ 下载合并固件 edk60_edv11p_track.bin](pathname:///edk60/edv11p/edk60_edv11p_track.bin)`，正文给出体积 244,192 B 与 SHA256 `C847423F…B40E`；自行构建的路径改为 `idf.py -B <你的构建目录> merge-bin` |
| 桌面端脚本（厂商参考上位机） | 保留脚本名，改为「PySide6 桌面端上位机，由厂商/社区渠道提供，本页不提供下载；拿不到就用网页版」——**无任何本机绝对路径** |
| `tools/edv11p_frame_check.py`、`tools/capture_gate2.ps1`、`tools/plot_gate2_capture.py` | 统一表述为「随仓库提供的工具」，用途集中写进 **附录 C.1 随仓库提供的工具**（仓库相对路径，无绝对路径） |

其他处理：
- 加 front-matter：`title: "人数统计雷达 Recipe（EDV11P 侧装）"`、`sidebar_position: 4`（排在 `flash-guide` 的 3 之后）、`description`；移除源文件的 `author` / `license` / `tags`（非 Docusaurus 文档字段，避免未知字段干扰）。
- 保留源文档 **10 章 + 3 附录**结构（构建产物中 `<h2>` 14 个 = 10 章 + 3 附录 + 「许可与署名」，`<h3>` 30 个）。
- 博客补 front-matter（slug / title / authors / tags / description / image），保留 `{/* truncate */}` 标记，图片引用改为 `/img/edk60/edv11p-*.svg`。

### 2.1 与任务书不一致的两处（已按「文件真实存在」原则处理，请确认）

| 项 | 任务书写法 | 实际落库 | 原因 |
|---|---|---|---|
| 博客 `image:` | `/img/edk60/ev11p-track-map.svg` | `/img/edk60/edv11p-track-map.svg` | 任务书同段已注明正确前缀是 `edv11p`；`ev11p-*` 文件不存在，按原文会让构建/OG 图 404 |
| 配图文件名 | `edk11p-track-map.svg`（笔误） | `edv11p-track-map.svg` | 已确认命名的正确前缀为 `edv11p` |

### 2.2 构建期必须修的 MDX 转义（已修，属源文档的 MDX 兼容问题）

MDX 会把 `数字<数字` 当成 JSX 标签起始符，导致编译失败（`Unexpected character 1`）。已在落库副本里加空格规避，**数值含义不变**：

| 位置 | 原文 | 改为 |
|---|---|---|
| 9.3 坑 7 | `峰值 <1.2 A` | `峰值 < 1.2 A` |
| 附录 C ③ | `判据 <30 cm` | `判据 < 30 cm` |
| 附录 C ⑥ | `差异 <0.5%` | `差异 < 0.5%` |

---

## 3. 构建验证（本轮验收条件）

命令（与任务书一致，输出目录指到临时目录，规避 sync 盘 EPERM）：

```powershell
$nodeDir = "C:\Users\fuhua\AppData\Local\Temp\docusaurus-node20\node-v20.19.5-win-x64"
cd C:\Users\fuhua\sync\easydetek\0.project\20260810-docs.easydetek.com
& "$nodeDir\node.exe" node_modules\@docusaurus\core\bin\docusaurus.mjs build --locale zh-Hans --out-dir "$env:TEMP\dsbuild-edv11p"
```

结果：**成功，0 error、0 warning**（`onBrokenLinks: 'throw'` 全程未触发）。

构建日志全文（仅 3 行）：

```text
[INFO] [zh-Hans] Creating an optimized production build...
[SUCCESS] Generated static files in "..\..\..\..\AppData\Local\Temp\dsbuild-edv11p".
[INFO] Use `npm run serve` command to test your build locally.
```

`EXITCODE=0`。日志文件：`%TEMP%\dsbuild-edv11p.log`；产物目录：`%TEMP%\dsbuild-edv11p`。

> 说明：首轮构建有 1 个 `tracking` 标签未定义的 WARNING + 2 处 MDX 编译 ERROR（见 2.2），均已修复后重跑；
> 上面记录的是**修复后**的最终构建。

---

## 4. 产物校验（5 项要求 + 3 项附加，全部通过）

| # | 校验项 | 结果 | 证据 |
|---|---|---|---|
| 1 | `track-recipe` 页面 | ✅ | `%TEMP%\dsbuild-edv11p\opensource\开源硬件\EDK60\track-recipe\index.html`（130,051 B） |
| 2 | 博客文章 | ✅ | `…\blog\edk60-edv11p-person-tracking\index.html`（38,891 B）；`blog\index.html` 列表已收录 |
| 3 | `img\edk60\edv11p-*.svg` | ✅ | `…\img\edk60\edv11p-track-map.svg`（199,270 B）、`…\img\edk60\edv11p-people-timeline.svg`（197,247 B）；正文渲染引用 `/assets/images/edv11p-*.svg`（webpack 处理后） |
| 4 | `edk60\edv11p\flash.html` | ✅ | `…\edk60\edv11p\flash.html`（10,019 B） |
| 5 | `edk60\edv11p\track.html` | ✅ | `…\edk60\edv11p\track.html`（91,706 B） |
| 6 | 合并固件 | ✅ | `…\edk60\edv11p\edk60_edv11p_track.bin`（244,192 B，与正文体积一致） |
| 7 | 发布清单 | ✅ | `…\edk60\edv11p\manifest.json`（286 B，引用 `edk60_edv11p_track.bin` @ `0x0`） |
| 8 | EDK60 首页三条入口 | ✅ | `index.html` 内含 `/blog/edk60-edv11p-person-tracking`、`/opensource/开源硬件/EDK60/track-recipe`、`/edk60/edv11p/flash.html`、`/edk60/edv11p/track.html` |

交叉引用验证：博客页内「人数统计雷达 Recipe」链接渲染为 `/opensource/开源硬件/EDK60/track-recipe`（可点击、构建期已通过链接校验）；Recipe 页内三个 `pathname:///` 链接分别渲染为 `/edk60/edv11p/{flash.html,track.html,edk60_edv11p_track.bin}`。

> 版本路由提示：`opensource_versions.json` 只有 `1.0.0`，正式路由由 `opensource_versioned_docs\version-1.0.0\` 提供 —— 所以**新页面必须同时存在于 versioned 目录**才会上线，本轮已镜像（字节一致）。current 目录的副本仅作后续编辑源。

---

## 5. 实测数字完整性抽查（逐项比对源文件，均未改动）

| 指标 | 源文件出现次数 | 落库后出现次数 |
|---|---|---|
| `15,483`（有效帧） | 4 | 4 |
| `15,454`（0x70 帧） | 6 | 6 |
| `530,972`（字节数） | 2 | 2 |
| `100.0000%`（帧完整率） | 2 | 2 |
| `99.9`（判据） | 3 | 3 |
| `178,656`（app 体积） | 2 | 2 |
| `8.6 帧/s` | 2 | 2 |
| `244,192`（合并固件体积） | 3 | 4（4.1 节新增一处指引性引用，数值未变） |
| SHA256 `C847423F…B40E` | 2 | 2 |

---

## 6. ⏸ 待用户批准后才执行的步骤（本轮未执行任何一条）

> 文档站 `push` 到 `main` 会自动触发两个 GitHub Actions：`Deploy to GitHub Pages`（构建 + 发布）
> 与 `Sync to Gitee`（把产物推到 gitee `gh-pages`、并镜像源码到 gitee `main`）。
> **即点即上线（约 3~5 分钟），所以必须用户明确批准后再跑。**

```powershell
cd C:\Users\fuhua\sync\easydetek\0.project\20260810-docs.easydetek.com

# ① 只暂存本轮这 7 个文件（工作区另有约 33 项历史未提交改动，不要一起提交）
git add "opensource_docs/开源硬件/EDK60/track-recipe.md" `
        "opensource_versioned_docs/version-1.0.0/开源硬件/EDK60/track-recipe.md" `
        "opensource_versioned_docs/version-1.0.0/开源硬件/EDK60/index.md" `
        "blog/2026-10-22-edk60-edv11p-person-tracking.md" `
        "blog/tags.yml" `
        "static/img/edk60/edv11p-track-map.svg" `
        "static/img/edk60/edv11p-people-timeline.svg"

# ② 提交
git commit -m "feat(docs): EDK60 新增 EDV11P 人数统计/轨迹 Recipe 页与应用案例"

# ③ 推送（origin = GitHub，触发 Pages 部署 + Gitee 同步；gitee 直推会触发 gitee pages 部署）
git push origin main
# 如需同时直推 gitee：git push gitee main
```

批准后的验收动作：
1. 看 GitHub Actions 两个 workflow 是否都绿；
2. 线上打开 `https://docs.easydetek.com/opensource/开源硬件/EDK60/track-recipe`、`/blog/edk60-edv11p-person-tracking`、`/opensource/开源硬件/EDK60/`，确认三条入口可点、两张图可见；
3. 点一次「网页烧录」与「网页实时轨迹」两个在线工具，确认静态页正常加载。

---

## 7. 回滚方式（如需）

| 场景 | 操作 |
|---|---|
| 已 push、线上有问题 | `git revert <commit>` 后 `git push origin main`，CI 会重新构建回滚版本（保留历史，推荐） |
| 只想撤本地改动（未 commit） | `git restore --staged <上述 7 个路径>`，再对 3 个修改文件 `git restore <路径>`；**新增文件删除需另行确认**（删除操作按团队规矩须用户明确同意） |
| 只想撤首页入口（不影响正文） | 单独 revert `opensource_versioned_docs/version-1.0.0/开源硬件/EDK60/index.md` 的改动 |
| 本地构建产物 | `%TEMP%\dsbuild-edv11p` 与 `%TEMP%\dsbuild-edv11p.log` 属临时文件，可直接丢弃，不影响仓库 |

---

## 8. 遗留 / 建议后续

| # | 事项 | 优先级 |
|---|---|---|
| 1 | 英文站缺这三个页面的翻译（`i18n/en/` 下无 `opensource` EDK60 目录），`/en/...` 会回落中文 | 中 |
| 2 | Recipe 附录 C 待补项仍有：原始 `0x70` 捕获字节、几何标定偏差值、帧率 8.6 vs 10 帧/s 归因、零人基线、A/B 对照、24h 老化、配图 3 张、桌面端脚本下载入口 | 中 |
| 3 | 首页三条入口已核对可用（`./track-recipe`、两个 `pathname:///`）；若后续调整 sidebar 顺序，注意 `sidebar_position: 4` 与 `flash-guide: 3` 的相对关系 | 低 |
| 4 | 博客「已知边界」里「网页版轨迹工具真机联调未完成」一句可等真机联调后再更新（本轮未改结论） | 低 |

---

## 9. 上线执行结果（2026-10-22，项目经理执行）

### 9.1 提交与推送

| 项 | 值 |
|---|---|
| 提交 | `70eb6e5 feat(docs): EDK60 新增 EDV11P 人数统计/轨迹 Recipe 页与应用案例` |
| 暂存范围 | 仅本次 12 个文件（9 个路径，其中 `static/edk60/edv11p/` 展开为 4 个文件）；工作区其余 27 项历史改动**未提交** |
| `origin`(GitHub) | ✅ `699e778..70eb6e5 main -> main`，`Deploy to GitHub Pages` **success** |
| `gitee` | ✅ 本地直接推送 `699e778..70eb6e5 main -> main`（补偿 CI 失败） |

### 9.2 线上验证

| URL | 结果 |
|---|---|
| `https://easydetek.github.io/edk60/edv11p/flash.html` | ✅ 200 / 10,019 B |
| `https://easydetek.github.io/edk60/edv11p/track.html` | ✅ 200 / **92,027 B（含「连接串口」按钮修复）** |
| `https://easydetek.github.io/blog/edk60-edv11p-person-tracking` | ✅ 200 |
| `https://easydetek.github.io/opensource/开源硬件/EDK60/track-recipe` | ✅ 200 |
| `https://docs.easydetek.com/edk60/edv11p/*` | ❌ **404**（见 9.3） |

### 9.3 主域名 `docs.easydetek.com` 未更新的原因（与本次内容无关）

1. 该域名由**自有服务器**（Docker: Caddy + nginx）托管，不读 GitHub Pages；DEPLOY.md 规定的更新方式是服务器上 `git pull && docker compose up -d --build`。
2. CI 的 **`Sync to Gitee` 工作流自 2026-10-18 起一直失败**（失败步骤固定为 `Deploy build to Gitee Pages (gh-pages)`；同一天的 run `39137808835` 也是 failure），属**既有缺陷**，不是本次引入。已用本地 `git push gitee main` 绕过，让服务器有内容可拉。
3. **待用户执行**：服务器上 `cd <站点仓库> && git pull && docker compose up -d --build`，随后确认两个 URL 返回 200。

### 9.4 本次随发布一并修复的缺陷

`track.html` 的「连接串口」按钮永远置灰（`state.serialSupported` 只赋 false、从未赋 true）。修复后 3 份副本同步更新（`release/track.html`、`release/edv11p/track.html`、`static/ekd60/edv11p/track.html`），文档站产物中为 92,027 B 版本。详见 `test-report-onboard.md` 第七节第 4 条。

### 9.5 后续建议

| # | 事项 | 优先级 |
|---|---|---|
| 1 | 修 CI 的 `Sync to Gitee`（gh-pages 推送失败），否则每次上线都要手工 `git push gitee main` + 服务器重建 | 高 |
| 2 | 主域名更新是否需要自动化（服务器 cron / webhook），避免每次人工 `git pull` | 中 |
| 3 | 英文站 `i18n/en` 补这三页 | 低 |

---

## 10. 追加变更（2026-10-22 11:40，按用户要求去除“recipe”字样）

| 项 | 内容 |
|---|---|
| 起因 | 用户要求：教程文档中不要出现「（开发者 recipe）」这类的自我描述 |
| 正文改动 | 3 份教程副本（工程内 1 + 文档站 current/versioned 各 1）共 5 类措辞：「（开发者 recipe）」删除、「本 recipe」→「本文」、页面标题「人数统计雷达 Recipe（EDV11P 侧装）」→「人数统计雷达实施指南（EDV11P 侧装）」；复核后正文 **0 处** recipe |
| URL 改动 | opensource_docs/开源硬件/EDK60/track-recipe.md → **	rack-guide.md**（version-1.0.0 镜像同步 git mv，保留 rename 历史 R099）；首页两处入口、案例文章链接同步改为 ./track-guide |
| 提交 | c07a02d docs(edk60): 教程去除「recipe」字样并改址为 track-guide |
| 构建 | docusaurus build **0 error**；产物 	rack-guide/index.html 130,130 B；旧 	rack-recipe 页面不再生成 |
| 线上（GitHub Pages） | .../EDK60/track-guide **200**（129,612 B）；.../EDK60/track-recipe **404**（符合预期，尚未对外宣传过该地址） |
| 待办 | 主域名服务器更新时注意：本次是**改址**，服务器 git pull 后旧 URL 会消失；对外只需发布 /opensource/开源硬件/EDK60/track-guide |

> 说明：工程内源文件名仍为 docs/recipe-edv11p-person-tracking.md（内部文档，不对外），如需一并改名请另行确认。
---

## 11. 主域名上线核验（2026-10-22，用户执行服务器更新后）

用户已在服务器执行 git pull && docker compose up -d --build。PM 逐条核验：

| URL | 结果 |
|---|---|
| https://docs.easydetek.com/edk60/edv11p/flash.html | ✅ 200 / 10,019 B |
| https://docs.easydetek.com/edk60/edv11p/track.html | ✅ 200 / **96,936 B（满视口新布局）** |
| https://docs.easydetek.com/edk60/edv11p/manifest.json | ✅ 200 / 286 B |
| https://docs.easydetek.com/edk60/edv11p/edk60_edv11p_track.bin | ✅ 200 / 244,192 B |
| https://docs.easydetek.com/blog/edk60-edv11p-person-tracking | ✅ 200 / 39,547 B |
| https://docs.easydetek.com/opensource/开源硬件/EDK60/track-guide | ✅ 200 / 131,381 B |
| .../EDK60/track-recipe（旧址） | ✅ 404（预期，已改址） |
| .../opensource/开源硬件/EDK60/ | ✅ 200 / 23,408 B |

**线上固件一致性**：下载线上 bin 重算 SHA256 = C847423F1FFDF7482934355432907AE1A788533AF121DE997C96658549A7B40E，与本地 elease/edv11p/ 逐字节一致；首 8 字节 E9 03 02 10 1A B9 86 40（ESP 镜像魔数正常）。→ **线上烧录通道可信**。

至此 Gate-2 遗留「主域名 404」关闭；仅剩「斜装几何标定」未做。