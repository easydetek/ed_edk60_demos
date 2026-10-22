# release —— 面向最终用户的烧录资源

> 这一目录里的东西是**给零基础用户**用的，不含源码、不需要编译。
> 用户只需要：打开一个网页 → 点按钮 → 看数据。**全程不装任何软件。**

## 目录内容

| 文件 | 作用 | 谁用 |
|---|---|---|
| `edk60_edv151_vital.bin` | **合并固件**（引导程序 + 分区表 + 应用，烧到 `0x0` 一个地址即可） | 烧录工具 |
| `manifest.json` | 网页烧录器（ESP Web Tools）的清单 | 浏览器读取 |
| `flash.html` | **一键烧录页面** | 最终用户 |
| `monitor.html` | **实时数据监视页面**（Web Serial，免装串口软件） | 最终用户 |

配套文档：[docs/flash-guide-for-beginner.md](../docs/flash-guide-for-beginner.md)

## 部署步骤（一次性）

把这 4 个文件放到你的文档站同一目录下，例如：

```
https://docs.easydetek.com/edk60/
├── flash.html                   ← 用户访问这个
├── monitor.html                 ← 用户访问这个
├── manifest.json
└── edk60_edv151_vital.bin
```

然后把链接告诉用户：

```
烧录：  https://docs.easydetek.com/edk60/flash.html
看数据：https://docs.easydetek.com/edk60/monitor.html
```

## 技术要求（重要）

| 要求 | 说明 |
|---|---|
| **必须 HTTPS** | 浏览器只允许 https 或 localhost 使用串口 API。http 会提示「not allowed」 |
| **必须 Chrome / Edge** | Firefox、Safari 不支持 Web Serial |
| **文件可公开访问** | `manifest.json` 与 `.bin` 需要能被浏览器直接下载，不要加登录鉴权 |
| **不要改文件名** | `manifest.json` 里写死了 `edk60_edv151_vital.bin`，改名要同步改 json |
| **CORS 无限制** | 同目录下相对路径引用，无需额外配置；若放到 CDN，需允许跨域读取 bin |

## 更新固件时

重新生成合并固件并替换 bin，同时把 `manifest.json` 里的 `version` 改掉：

```bash
# 在仓库根目录，选好 demo 后：
idf.py -B build-<demo> merge-bin
cp build-<demo>/merged-binary.bin release/edk60_edv151_vital.bin
```

> 用 `merge-bin` 的好处：产出的是**单文件**，用户侧只需要「一个文件 + 地址 0x0」，
> 不必理解 bootloader / 分区表 / 应用三个地址，出错概率大幅降低。

## 已做的校验

| 项 | 结果 |
|---|---|
| 合并固件大小 | 275.3 KB |
| `0x0000` 首字节 | `0xE9` ✓ 合法 ESP 引导程序 |
| `0x8000` 首字节 | `0xAA` ✓ 分区表魔数 |
| `0x10000` 首字节 | `0xE9` ✓ 应用固件 |
| **合并固件实机烧录** | ✅ **通过**（`Wrote 281920 bytes at 0x0` + `Hash of data verified`） |
| **烧后能否正常启动** | ✅ **通过**（日志正常，未卡死） |
| **6 条开机握手** | ✅ **全部 `ok`**（turn_on / set_time / 周期 / 范围 / gain / boost） |
| **能否检测到人** | ✅ **通过**（上位机读到 距离 58~80cm、体动有值） |
| `manifest.json` | chipFamily = `ESP32-C6`，bin 路径存在，offset = 0 |
| `flash.html` | 引用 esp-web-tools CDN、manifest、activate 插槽均存在 |
| `monitor.html` | 括号/引号配平、关键 Web Serial API 与解析函数齐全 |

> 合并固件已按「单文件 + 地址 0x0」这条**用户实际会走的路径**烧录验证过，
> 行为与分三个地址烧录完全一致（这正是 `merge-bin` 的用途）。

## 待实机验证

| 项 | 状态 |
|---|---|
| `flash.html` 网页烧录器在真机 + Chrome 上跑通 | ⬜ **待验证**（本机未装 Chrome 串口环境，需人工点一次） |
| `monitor.html` 在真机浏览器里解析体征帧 | ⬜ **待验证**（解析逻辑与已验证的 `tools/radar_monitor.py` 完全一致） |
| Windows 10/11 免驱识别 | ✅ 已验证（VID 303A 原生 USB，系统自带驱动） |

> **上线前建议**：找一台「干净的、没装过 ESP-IDF 的电脑」，用 Chrome 走一遍
> `flash.html → monitor.html` 全流程，确认零基础用户能独立完成，再发给客户。
> 网页方案若在他机器上不可用，直接引导到文档里的「备用方案：Flash Download Tool」。

