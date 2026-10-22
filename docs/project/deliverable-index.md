# 交付物索引 — EDK60 多工程 Demo

| 项 | 内容 |
|---|---|
| 项目 | EDK60 多工程 Demo |
| 维护人 | 项目经理 |
| 建立日期 | 2026-10-14 |
| 最近更新 | 2026-10-14 |
| 关联范围基线 | `SC-EDK60-DEMOS-v1`（2026-10-14 冻结） |

---

## 一、编号规则

```
<类别>-<三位序号>

类别：
  SW    软件交付物（源码、构建脚本、工程配置）
  DOC   文档交付物
  CFG   环境/IDE 配置
  REC   评审与记录（Gate 记录、验证记录）

命名规范：
  * 文件名用小写 + 短横线（kebab-case）：demo_get_started_<demo>.c、<主题>.md
  * 源文件前缀固定：demo_get_started_ 表示「参与 menuconfig 选择的 demo」
  * 文档统一放 docs/；项目管理类统一放 docs/project/
版本规则：
  * 首次交付 v1.0；内容修改升 v1.x；结构或范围变更升 v2.0（须走变更登记）
```

---

## 二、交付物索引表

| 编号 | 交付物 | 类型 | 责任角色 | 所属阶段 | 版本 | 状态 | 存放路径 | 关联 Gate |
|---|---|---|---|---|---|---|---|---|
| SW-001 | 顶层工程 CMakeLists（含目标芯片校验） | 源码 | 嵌入式工程师 | 骨架 | v1.0 | ✅ 已交付 | [`CMakeLists.txt`](../../CMakeLists.txt) | Gate-1 |
| SW-002 | 编译默认配置 | 配置 | 嵌入式工程师 | 骨架 | v1.0 | ✅ 已交付 | [`sdkconfig.defaults`](../../sdkconfig.defaults) | Gate-1 |
| SW-003 | demo 选择配置（脚本维护） | 配置 | 项目经理 | 骨架 | v1.0 | ✅ 已交付 | [`sdkconfig.defaults.esp32c6`](../../sdkconfig.defaults.esp32c6) | Gate-1 |
| SW-004 | menuconfig 菜单定义（15 个 demo + 参数） | 源码 | 嵌入式工程师 | 骨架 | v1.0 | ✅ 已交付 | [`main/Kconfig.projbuild`](../../main/Kconfig.projbuild) | Gate-2 |
| SW-005 | 应用入口 | 源码 | 嵌入式工程师 | 骨架 | v1.0 | ✅ 已交付 | [`main/src/app_main.c`](../../main/src/app_main.c) | Gate-1 |
| SW-006 | 多 demo 分派器 | 源码 | 嵌入式工程师 | 骨架 | v1.0 | ✅ 已交付 | [`components/edk60_app/src/demo_dispatch.c`](../../components/edk60_app/src/demo_dispatch.c) | Gate-1 |
| SW-007 | demo 统一接口声明 | 源码 | 嵌入式工程师 | 骨架 | v1.0 | ✅ 已交付 | [`components/edk60_app/include/edk60_demo.h`](../../components/edk60_app/include/edk60_demo.h) | Gate-1 |
| SW-008 | 板级支持包 BSP 实现 | 源码 | 硬件 + 嵌入式工程师 | 骨架 | v1.0 | ✅ 已交付 | [`components/edk60_board/`](../../components/edk60_board/) | Gate-1 |
| SW-009 | 引脚宏头文件（含编译期校验） | 源码 | 硬件工程师 | 骨架 | v1.0 | ✅ 已交付 | [`components/edk60_board/include/edk60_board.h`](../../components/edk60_board/include/edk60_board.h) | Gate-1 |
| SW-010 | Demo 1.1 hello_board | 源码 | 嵌入式工程师 | 实现 | v1.0 | ✅ 已交付 | [`demo_get_started_hello_board.c`](../../components/edk60_app/src/demo_get_started_hello_board.c) | Gate-2 |
| SW-011 | Demo 1.2 blink | 源码 | 嵌入式工程师 | 实现 | v1.0 | ✅ 已交付 | [`demo_get_started_blink.c`](../../components/edk60_app/src/demo_get_started_blink.c) | Gate-2 |
| SW-012 | Demo 1.3 button | 源码 | 嵌入式工程师 | 实现 | v1.0 | ✅ 已交付 | [`demo_get_started_button.c`](../../components/edk60_app/src/demo_get_started_button.c) | Gate-2 |
| SW-013 | Demo 1.4 uart_echo | 源码 | 嵌入式工程师 | 实现 | v1.0 | ✅ 已交付 | [`demo_get_started_uart_echo.c`](../../components/edk60_app/src/demo_get_started_uart_echo.c) | Gate-2 |
| SW-014 | Demo 1.5 adc_read | 源码 | 嵌入式工程师 | 实现 | v1.0 | ✅ 已交付 | [`demo_get_started_adc_read.c`](../../components/edk60_app/src/demo_get_started_adc_read.c) | Gate-2 |
| SW-015 | Demo 2.1 wifi_sta | 源码 | 嵌入式工程师 | 实现 | v1.0 | ✅ 已交付 | [`demo_get_started_wifi_sta.c`](../../components/edk60_app/src/demo_get_started_wifi_sta.c) | Gate-2 |
| SW-016 | Demo 2.2 wifi_softap | 源码 | 嵌入式工程师 | 实现 | v1.0 | ✅ 已交付 | [`demo_get_started_wifi_softap.c`](../../components/edk60_app/src/demo_get_started_wifi_softap.c) | Gate-2 |
| SW-017 | Demo 2.3 ble_uart | 源码 | 嵌入式工程师 | 实现 | v1.0 | ✅ 已交付 | [`demo_get_started_ble_uart.c`](../../components/edk60_app/src/demo_get_started_ble_uart.c) | Gate-2 |
| SW-018 | Demo 2.4 ble_scan | 源码 | 嵌入式工程师 | 实现 | v1.0 | ✅ 已交付 | [`demo_get_started_ble_scan.c`](../../components/edk60_app/src/demo_get_started_ble_scan.c) | Gate-2 |
| SW-019 | Demo 3.1 radar_power | 源码 | 嵌入式 + 硬件工程师 | 实现 | v1.0 | ✅ 已交付 | [`demo_get_started_radar_power.c`](../../components/edk60_app/src/demo_get_started_radar_power.c) | Gate-2 |
| SW-020 | Demo 3.2 led_sequence | 源码 | 嵌入式工程师 | 实现 | v1.0 | ✅ 已交付 | [`demo_get_started_led_sequence.c`](../../components/edk60_app/src/demo_get_started_led_sequence.c) | Gate-2 |
| SW-021 | Demo 3.3 i2c_scan | 源码 | 嵌入式工程师 | 实现 | v1.0 | ✅ 已交付 | [`demo_get_started_i2c_scan.c`](../../components/edk60_app/src/demo_get_started_i2c_scan.c) | Gate-2 |
| SW-022 | Demo 4.1 deep_sleep | 源码 | 嵌入式工程师 | 实现 | v1.0 | ✅ 已交付 | [`demo_get_started_deep_sleep.c`](../../components/edk60_app/src/demo_get_started_deep_sleep.c) | Gate-2 |
| SW-023 | Demo 4.2 nvs_rw | 源码 | 嵌入式工程师 | 实现 | v1.0 | ✅ 已交付 | [`demo_get_started_nvs_rw.c`](../../components/edk60_app/src/demo_get_started_nvs_rw.c) | Gate-2 |
| SW-024 | Demo 4.3 usb_serial_bridge（USB 转串口） | 源码 | 嵌入式 + 硬件工程师 | 实现 | v1.0 | ✅ 已交付 | [`demo_get_started_usb_serial_bridge.c`](../../components/edk60_app/src/demo_get_started_usb_serial_bridge.c) | Gate-2 |
| SW-025 | 批量构建脚本 | 脚本 | 项目经理 | 骨架 | v1.1 | ✅ 已交付 | [`build_all.ps1`](../../build_all.ps1) | Gate-2 |
| SW-026 | 仓库忽略规则 | 配置 | 项目经理 | 骨架 | v1.0 | ✅ 已交付 | [`.gitignore`](../../.gitignore) | Gate-1 |
| SW-027 | Demo 5.1 radar_bridge（雷达原始字节透传 + 接收脚/波特率自动识别） | 源码 | 项目经理 | 实现 | v1.0 | ✅ 已交付 | [`demo_get_started_radar_bridge.c`](../../components/edk60_app/src/demo_get_started_radar_bridge.c) | Gate-2 |
| CFG-001 | VS Code ESP-IDF 插件配置 | 配置 | 项目经理 | 交付 | v1.0 | ✅ 已交付 | [`.vscode/settings.json`](../../.vscode/settings.json) | Gate-3 |
| CFG-002 | VS Code 调试配置 | 配置 | 项目经理 | 交付 | v1.0 | ✅ 已交付 | [`.vscode/launch.json`](../../.vscode/launch.json) | Gate-3 |
| DOC-001 | 工程说明与快速开始 | 文档 | 项目经理 | 交付 | v1.0 | ✅ 已交付 | [`README.md`](../../README.md) | Gate-3 |
| DOC-002 | 引脚定义表与硬件红线 | 文档 | 硬件工程师 | 交付 | v1.0 | ✅ 已交付 | [`docs/EDK60-pinmap.md`](../EDK60-pinmap.md) | Gate-1 / Gate-3 |
| DOC-003 | VS Code 使用说明 | 文档 | 项目经理 | 交付 | v1.0 | ✅ 已交付 | [`docs/vscode-setup.md`](../vscode-setup.md) | Gate-3 |
| DOC-004 | 项目章程 | 文档 | 项目经理 | 全程 | v1.0 | ✅ 已交付 | [`docs/project/project-charter.md`](project-charter.md) | Gate-1 |
| DOC-005 | 交付物索引（本文件） | 文档 | 项目经理 | 全程 | v1.0 | ✅ 已交付 | [`docs/project/deliverable-index.md`](deliverable-index.md) | 全程 |
| DOC-006 | 风险登记册 | 文档 | 项目经理 | 全程 | v1.0 | ✅ 已交付 | [`docs/project/risk-register.md`](risk-register.md) | 全程 |
| DOC-007 | 任务分派记录 | 文档 | 项目经理 | 实现 | v1.0 | ✅ 已交付 | [`docs/project/dispatch-log.md`](dispatch-log.md) | Gate-2 |
| REC-001 | Gate-1 骨架评审记录 | 记录 | 评审官 | 骨架 | v1.0 | ✅ 已交付 | [`gate-records/gate-1-baseline.md`](gate-records/gate-1-baseline.md) | Gate-1 |
| REC-002 | Gate-2 实现评审记录 | 记录 | 评审官 | 实现 | v1.0 | ✅ 已交付 | [`gate-records/gate-2-implementation.md`](gate-records/gate-2-implementation.md) | Gate-2 |
| REC-003 | Gate-3 交付评审记录 | 记录 | 评审官 | 交付 | v1.0 | ✅ 已交付 | [`gate-records/gate-3-delivery.md`](gate-records/gate-3-delivery.md) | Gate-3 |
| REC-004 | 编译验证报告（15/15 通过） | 记录 | 项目经理 | 实现 | v1.0 | ✅ 已交付 | [`gate-records/build-verification.md`](gate-records/build-verification.md) | Gate-2 |
| REC-005 | 上板交接包（操作清单 + H-01 判定表） | 记录 | 项目经理 | 交付 | v1.0 | ✅ 已交付 | [`gate-records/handover.md`](gate-records/handover.md) | Gate-3 / Gate-4 |
| REC-006 | 原理图原件（基线输入，非本项目产出） | 输入 | 用户 | — | V1.0 | 📌 基线输入 | [`docs/SCH_EDK60_VA.pdf`](../SCH_EDK60_VA.pdf) | — |

> **清理记录（2026-10-14）**：经用户确认后已删除以下临时产物（均为可重建）：
> ① 子代理误留的渲染脚本 `docs/_render_sch.py`；
> ② 19 个生成的 `sdkconfig.<demo>` 副本（保留基线 `sdkconfig.defaults` 与 `sdkconfig.defaults.esp32c6`）；
> ③ 19 个 `build-*` 构建目录。
> 合计释放约 4.5 GB。下次运行 `build_all.ps1` 会自动重建。

> 状态取值：✅ 已交付 / 🔄 进行中 / ⏸ 待前置 / ❌ 阻塞 / 📌 基线输入 / 🗑 已废止（superseded）
>
> **验证状态说明**：SW-010 ~ SW-024、SW-027 共 16 个 demo 均已完成**编译/链接级验证**（16/16 通过，0 error / 0 warning，见 REC-004）。
> **尚未完成上板运行验证** —— 11 项待实测项见 [risk-register.md](risk-register.md) 第三节与 [handover.md](gate-records/handover.md)。

---

## 三、目录约定

```
docs/
├── SCH_EDK60_VA.pdf              原理图原件（只读基线输入，不要修改）
├── EDK60-pinmap.md               引脚定义表（人可读的权威来源）
├── vscode-setup.md               IDE 使用说明
└── project/
    ├── project-charter.md        项目章程（范围基线）
    ├── deliverable-index.md      本文件
    ├── risk-register.md          风险登记册 + 变更记录
    ├── dispatch-log.md           任务分派记录
    └── gate-records/
        ├── gate-1-baseline.md
        ├── gate-2-implementation.md
        ├── gate-3-delivery.md
        └── build-verification.md
```

**约定**：交付物只放在上表列出的位置；新增目录或改变约定需先更新本文件。

---

## 四、可追溯性检查

| 检查项 | 方法 | 结果 |
|---|---|---|
| 每个 demo 都能追溯到原理图依据 | 各源文件头注释是否写明「硬件事实（原理图…）」 | ✅ 15/15 均有 |
| 每个引脚结论都能追溯到原理图 | `edk60_board.h` 是否逐个标注原理图位置 | ✅ 每行宏带注释 |
| 每个 Gate 结论都有记录文件 | `gate-records/` 是否齐全 | ✅ REC-001~004 |
| 每项风险都有 owner 与状态 | `risk-register.md` 表格列完整性 | ✅ 13 项均有 |
| 每次范围变更都有登记 | `risk-register.md` 第四节 | ✅ CR-01~CR-05 |
| 文档之间不出现相互矛盾的引脚号 | 交叉比对 README / pinmap / 头文件 | ✅ 已比对一致 |
