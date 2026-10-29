# C++ 采集内核（UTF-8 中文）

## 2026-10-27 本地验收定位

当前 Windows x64 配套候选为 `V8Trace-HTTPRoot-Candidate-20261027-R7`；29 项 CTest、无 CDP 有限链、未决/未读、暂停/手停、预算/异常退出及原生/UI/MCP/TCP 矩阵通过。有限样本三条目标根闭合；UI 实际核验 871 条上游 JSCall、Native/Opcode/DOM/加密和独立 HTTP oracle 的完整 payload。消费续段消除双 Worker/307 样本的环，但一项常驻栅格工作池 Job 仍未结束，三根保持 `completeCausality=false`，该对照只证明真实边界未被抹掉。截断前根未落盘时仍声明无链证明；macOS 和任意外部网站不据此视为通过。入口、源码补丁、SHA-256、原始失败和清理残留见 [中文交接](../完整链路验收与交接-20261027.md)。

## MCP 搜索 / 分析

软件右侧新增“MCP 搜索 / 分析”：选日志根目录，启动独立 C++ 服务，复制 HTTP 或 stdio 客户端配置。支持 64 位稀疏行索引、后台进度、字面量 / RE2 正则搜索、有序分页、原始字节导出，以及 JSCall / Opcode / Native / HTTP 的结构化证据定位。默认不开端口，不自动改全局 MCP 配置，不启动浏览器。见 [MCP 搜索分析说明](MCP搜索分析说明.md) 和 [来源与适配边界](trace-search来源与适配.md)。

## 默认英语与独立启动

定制浏览器界面默认 `en-US`，首选语言顺序为 `en-US,en`，不自动加入中文语言项，仍保留中文输入。本轮原生构建显式关闭 Chromium 的整套开发测试实验配置（不是关闭沙箱、IME、JIT 或采集通道），避免开发实验影响启动与语言列表。修复在浏览器构建中生效，独立运行不依赖 UI 启动器追加参数；旧编译目录请先重新 Configure。配套版本以 `v8trace/startup-defaults/1`、GN 参数散列和真实启动/采集验收回执为准。

目录按便携软件根目录分层：浏览器放 `chromium/`，配置与日志使用其外侧的 `profiles/`、`captures/` 等目录。它们仍可是相对路径；不要把已运行的用户 profile 塞进浏览器运行文件目录。启动 ACL 自检会保留对保护文件的拒绝，不以放宽 profile 权限修复该布局。

原生暂停/继续的控制路径在浏览器端先按操作系统规范化分隔符，再与唯一的 `native-output` 同级目录比较。Windows 的 `/` 与 `\` 表示同一路径，不再被误判；绝对路径、输出目录和 URL 协议检查仍保留，不折叠 `..` 来放宽边界。该修复需配套重编定制 Chromium，只有宿主单元通过不代表原生采集已验收。

## 大日志索引与流式导出

关联和导出改用静态链接 SQLite 磁盘索引，资源按身份查询，同步包围使用区间候选与精确数值复核；JSON 数组、同 URL 冲突和环境依赖流式输出。实际缓存、查询和扫描步数记录在 `diskIndex`。配置 8 MiB 页缓存不等于进程内存上限。完整说明及规模测试见 [大日志索引说明](大日志索引说明.md)。

## 全量缺口流水

`gaps.jsonl` 按 `gapSeq` 保存全部缺口，包括正常事件 / 字节预算耗尽之后的诊断。该流水不占正常采集预算，但记录 `diagnosticBytes`，遵守同一有界背压队列；磁盘错误会传播而非隐藏。`gapJournal` 含协议 `v8trace/gaps/1`、数量、实际字节、SHA-256 和排空状态。`gaps.json` 与摘要最多保留 200 条 / 合计 4 MiB 样例，避免大量超长诊断挤爆摘要与内存；完整值始终在全量流水，`omittedGapCount` 仅表示摘要省略数。导出会验证完整流水 hash 并复制；保存后 TCP 也保留该文件。

## 随停随开采集（2026-10-23）

在基本配置中勾选“启用随停随开”，启动后使用底部“暂停采集 / 开启采集”按钮反复切换。“启动后先不采集”适合先手动登录、操作页面，再开启目标阶段的记录。浏览器进程、页面和 profile 不重启；“停止并保存”在暂停时仍有效，结束后关闭本次启动的浏览器。

按钮收到生产端确认才恢复可点击；暂停期间网页继续执行，但选中业务通道不追加记录，暂停及刷盘切换时间不计入采集时长。状态和计时使用单调时钟。仅控制回执、运行错误和浏览器进程诊断仍保留，不将这些诊断当作业务事件。

- CDP：实际启停 Tracing、CPU Profiler、Network、Debugger、Runtime/Page 事件域，Env 包装器走轻量旁路。恢复前清理尚未记录的 Console 缓存，防止把暂停期间的 Console / 异常历史重新灌入。Debugger 恢复时可能枚举已经存在的源码，这属于恢复时可见的静态元数据，不声称它在恢复后才加载。
- 非 CDP：通过当前用户采集目录内的身份校验控制文件，交给浏览器主进程使用原生 TracingController 和独立 NetLogExporter 启停；不创建调试端口，也不静默切换 CDP。浏览器身份描述须含 `recordingControlProtocol=v8trace/recording/1` 及对应补丁哈希。旧浏览器会明确报版本不匹配；取消该选项仍使用旧的持续采集路径。
- Env Hook：启动暂停时也预安装包装器，关闭时先判断闸门再处理参数和值，首次开启时补充安装/就绪元数据。包装器身份与解释执行模式保留，暂停不等于卸载插装或恢复原生 JIT 性能。
- 证据范围：只记录开启窗口，不补录暂停期间的调用、HTTP 正文、Console 和异常。跨暂停边界的调用、请求、异步回调可能不完整，窗口索引及原始文件用于识别边界；这不是连续全程日志，也不是网站业务成功证明。

`recording-control.jsonl` 保留确认记录，`recording-windows.json` 保留窗口索引，`recording-summary.json` 记录有效/暂停时长。`native-output/trace-N.json`（CDP 为 `cdp-trace-N.json`）和非 CDP 的 `netlog-N.json` 保留各段原始内容，最终流式归并供原解码器使用。归并后的原始事件带 `v8traceWindow`，CDP 业务上下文另带 `recordingWindow`；原生规范化记录仍沿用原解码字段，可结合原始窗口和时间索引定位。保留逐窗口刷盘、丢失统计与完整值，窗口合并不将缺口改成成功。原始字节总预算及宿主预算继续生效，超出时停止并保留证据，而非循环覆盖。

Worker 输入为一行一条 JSON，启动选项增加 `recordingControl: true`、可选 `startPaused: true`；启动确认后发送 `{"id":"pause-1","command":"pause-recording"}` / `{"id":"resume-1","command":"resume-recording"}`。`recording-pending` 是受理状态，`recording-state` 的 `pending=false` 才是生产端确认，`recording-control-error` 不结束当前采集任务。等待确认期间仍接受 `stop` / `shutdown`。

本轮 Windows CDP 实测、UI、状态机和原生控制夹具的证据位于 `artifacts/v8trace-recording-toggle-20261023`。原生核心头文件编译检查不替代完整 Chromium 验收；定制浏览器是否已编入该功能，以完整构建回执、浏览器身份文件及实际非 CDP 测试为准。

## 当前边界

本目录实现独立 C++20 内核：浏览器进程管理、原生/CDP 两条采集通道、流式 trace 解码、逐次调用/Opcode/WASM、浏览器通道归一化、内容寻址正文和源码、案例导出，以及 TCP 收发。运行期不加载 Node.js，不通过 Node 子进程调用旧采集器。Env Hook 的 JavaScript 是被观察浏览器内执行的页面脚本，不是 Node 依赖。

Windows 保留原有 WinForms 界面，采集工作改由 `runtime/v8trace-runtime.exe` 完成。macOS/Linux 的内核使用相同 CMake 工程；macOS 真实编译、运行及 ARM64 Chromium 插装覆盖尚需在对应机器验收，当前 Windows 测试结果不等于这些平台已通过。完整定制 Chromium 的平台迁移不是只编译此目录：其二进制、Framework 资源、插装与身份描述均需匹配。

旧 JS 采集源码目前仅作为迁移对照，待差分验收通过再删除。新 C++ 发行包不复制 Node 运行时、JS worker 或这些旧宿主模块，不做自动回退。关联证据明确保留未决边界；新旧关联索引的逐项等价验收尚未完成。

## 场景组合与案例提示词（2026-10-23）

UI 提供 13 个分阶段采集组合和 6 个案例提示词模板。新站点先用“网站初探”，补环境先用“环境取证”；只有缺少关键调用值时再开启目标调用详情、Native 参数或对象快照。目标是取得本地 V8 执行关键 JS 所需的证据，再按任务要求做纯协议交付；预设不执行案例或自动还原。

“JSCall 结构定位”明确不读取参数/this/返回/异常详情，不是 CPU 采样，也不当作完整值证据。定向组合要求具体脚本 URL/文件名 glob，UI 和 C++ 均校验；只有通配符的规则不作为定向范围。切换组合会重置所有通道、值模式、调用过滤、Native 子类、对象预算和宿主缓冲，保留用户网址、目录、无头选项与表达式。预览不应用配置。

脚本范围只约束 JSCall/Native，不约束 Env Hook、HTTP、源码、Ignition Opcode 或 WASM 内部。单独的 Opcode/WASM 组合默认不叠加其他密集详情；“全通道短时诊断”仍保留，但明确为高开销选项。未知来源、探针排除、值图和正文预算均须按实际范围核对，预算或采集缺口继续报告 partial。

提示词分别覆盖补环境、纯算、仅日志交接、JSVMP、白屏/卡顿定位和 WASM。全程使用 UTF-8 中文记录，浏览器只用于取证，最终方案禁止浏览器自动化及自动化兜底；只要求取证时不声称业务复现已通过。5s_js 样本的 UI 自动替换与探针 TCP 桥接仍未接入，不因新增模板而变成已支持。

长提示词复制会读回核对全文，保存显式使用 UTF-8。宿主缓冲是落盘队列，TCP 是保存后的完整包传输，不是 Chromium 实时推流。新分层 JSCall 模式依赖匹配版本的定制 Chromium；旧版不做静默回退。

## Windows 编译

在 Visual Studio x64 开发者 PowerShell 中，从项目根目录运行：

```powershell
pwsh -NoProfile -File .\v8-trace\build-cpp.ps1 -BuildRoot G:\v8-trace-cpp-build\build -OutputPath .\v8-trace\dist\V8Trace-Cpp
```

可选 `-NativeChromePath <完整定制浏览器的 chrome.exe>` 将已校验的 Chromium 整包复制到输出目录。本内核构建不重新编译 Chromium。

依赖使用固定版本、固定下载 SHA-256 的 nlohmann/json、IXWebSocket、zlib、Brotli 和 Zstd，许可证随包交付。首次配置联网，后续使用 CMake 缓存。内核只通过回环 WebSocket 使用 CDP，普通 HTTP/HTTPS 网站访问由 Chrome 自己执行。

## macOS 编译内核

在安装 Xcode Command Line Tools、CMake、Ninja 的 macOS 主机执行：

```sh
cd v8-trace/runtime-cpp
cmake --preset macos-arm64
cmake --build --preset macos-arm64 --parallel 4
ctest --preset macos-arm64
cmake --install out/macos-arm64 --prefix ../../dist/V8Trace-Cpp-macos-arm64 --component Runtime
```

Intel Mac 使用 `macos-x64`。内核支持 `.app/Contents/MacOS/...` 路径，并在 `Contents/Resources/v8trace-opcode.json` 查找定制身份；`binaryHashes` 的路径相对 `Contents`，同时覆盖启动文件和实际 Framework 引擎。签名与安装包公证需要在 macOS 平台另外执行。

## 命令行

```powershell
.\runtime\v8trace-runtime.exe capture --config .\capture-options.json
.\runtime\v8trace-runtime.exe inspect --capture .\logs\capture-001
.\runtime\v8trace-runtime.exe export --capture .\logs\capture-001 --output .\cases\case-001
```

开发目录运行时使用 `--assets .\v8-trace`；发行包自动相对自身定位 `app/v8-trace`。采集 JSON 例子：

```json
{
  "preset": "env-protocol",
  "chromePath": "./chromium/chrome.exe",
  "url": "https://example.test/",
  "output": "./logs/capture-001",
  "durationSeconds": 10,
  "detailScriptInclude": "target.js",
  "nativeKinds": "api-operation,api-getter,api-setter,runtime"
}
```

所有相对输入路径以进程当前工作目录解释；UI 将工作目录设为软件目录。源码同时保留 UTF-8 与无损 UTF-16LE 副本，孤立代理项、NUL、64 位整数及原始浮点类型值保持原始意义。日志缺口和预算超限继续标记 `partial`，不删除字段制造 `complete`。

## TCP 大日志传输

接收端（可运行在另一台 Windows/macOS/Linux 机器）：

```sh
runtime/v8trace-runtime receive --root ./received --bind 127.0.0.1 --port 17880
```

发送端：

```powershell
.\runtime\v8trace-runtime.exe send --capture .\logs\capture-001 --host 127.0.0.1 --port 17880
```

跨机器监听/发送必须两端指定相同的 `--token-file <令牌文件>`，内容至少 16 字节。端口默认 17880。令牌不放入日志或命令行值中。TCP 未启用 TLS，完整 Header、Cookie、正文和调用值均是原始内容，请在可信网络或加密隧道内使用。

- 协议 `v8trace/tcp/1`：8 字节帧头、最大 64 KiB JSON 元数据、4 MiB 原始数据块、Zstd 3 级无损压缩。
- 每块 SHA-256 校验，写入并同步文件后 ACK；每个文件及最终提交再次校验大小与 SHA-256。
- 默认最多 4 次连接尝试，接收端报告已落盘偏移，按同一清单身份续传，不从头重复写。
- 接收端先保存到 `.incoming/<传输标识>/data`，全部校验后提交为 `<接收根目录>/<传输标识>`。失败数据保留用于续传，不冒充完成。
- 默认接收配额 1 TiB，可通过 `--maxBytes` 调整；内存按单块及清单有界，不把完整大日志读入内存。
- 完整传输原始 trace、事件日志、正文与源码 blob、目录和摘要；不传浏览器 profile，不删除发送端原件。
- Windows UI 的“TCP 日志传输”页支持手动传输和采集保存后自动传输。
- 当前阶段是**已落盘采集包的传输**，不是 Chromium 生产端边生成边推送。TCP 解决跨机器搬运，Zstd 减少传输字节；它不消除采集时的对象快照开销，不保证解决复杂页面白屏，也尚未减少原生 trace 的本地落盘量。

## 验证

`ctest` 验证 Unicode/孤立代理项、整数、摘要、blob、过滤令牌、流式解析、生产者顺序与临时文件清理。`tests/integration_test.py` 用真实 C++ 进程执行原生导入、界面协议、TCP 完整性、分块和续传等回归。真实网站、macOS 与完整功能差分必须另外记录实测结果。

## 原生环境四件套

UI“原生环境取证”页提供缺失候选、接口查询、宿主改动及原始接口库存，默认关闭，有独立脚本范围和库存预算。启用会使用 Ignition 并关闭 JS 优化，保留方法身份，不执行额外 getter/Proxy。需要新编译的 `env/1` 后端；具体证据、unknown 状态与运行验收边界见 [原生环境取证说明](原生环境取证说明.md)。

## JSCall 分层采集说明

新增函数字典、函数级过滤缓存、类型化调用事件与最终计数验收。逐次调用与值采集分开，jsCallValues 默认保持 true；详情可单独按脚本或函数选择。结构模式和详情范围需要新编译的 v8trace/call/2 Chromium，旧浏览器会明确报版本不匹配，不自动降级。完整说明见源码目录 native/JSCall优化说明.md。

## JSVMP 读取定位与宿主缓冲

`vmp-discover --trace FILE --output NEW_DIR [--site CANDIDATE_SHA256]` 从原始 Ignition Opcode trace 提取索引读取候选与完整值，保留入口 / 出口证据及缺口。它是采集后的分析，不自动识别未知自定义指令语义或重写网站 JS。

宿主日志新增 8 MiB 有界背压队列和最高 1 MiB 连续批量写入；UI 高级选项可调整。满时等待、超大单条完整输出、停止排空，写入错误传播。这里不是浏览器 Perfetto 缓冲或实时 TCP 推流。完整说明见 `JSVMP与缓冲说明.md`。

## 直接落盘，不打印采集日志

插桩数据直写 trace / JSONL；浏览器 stdout/stderr 直接保存到采集目录 `native-output`，网页原有 Console/异常按勾选通道记录，不转打印到终端。

CLI `capture`、`import` 的运行信息和错误写 UTF-8 `<output>.runtime.jsonl`，stdout/stderr 为空；配置尚未读出时写当前目录 `v8trace-runtime.jsonl`。日志文件写失败返回 `74`，不向终端打印兜底。UI worker / MCP stdio 的内部 JSON 通信以及 `plan`、`query` 等机器查询输出保持兼容。

逐 HTTP 请求查看同步/异步函数、DOM、加密参数和正文引用，见 [HTTP首栈与参数DOM插桩说明](HTTP首栈与参数DOM插桩说明.md)；新版原生浏览器仍以实际编译和运行验收为准。
