# V8 Trace 开关速查表

作者：Alcohol · 文档基准：2026-10-23 · C++ 内核 2.0.0 · 场景目录 2026-10-23.2

参考 RuyiTrace `README-cheatsheet.md` 的“按目标选开关 → 分类查参数 → 读输出 → 验收”结构，内容按本项目当前源码重新核对。这里不是 RuyiTrace 开关的兼容层：`MOZ_DOM_*` 环境变量不适用于 V8 Trace。

从头操作见 [操作手册](操作手册.md)；技术细节见 [C++ 内核说明](runtime-cpp/README.md)、[JSCall 分层说明](native/JSCall优化说明.md)、[JSVMP 与缓冲说明](runtime-cpp/JSVMP与缓冲说明.md)。

> 本文描述当前源码的选项。某台机器能用哪些原生通道，由配套 Chromium 的身份文件、二进制哈希及实际验收决定。软件更新不等于浏览器同步升级。本轮新增原生环境、定向闭包、状态、模块/确认 VM 探针、持续封块及配置/索引源码；完整新 Chromium 构建和实际矩阵仍由最终发行门槛控制，不据此宣称旧包已经具备这些能力。

## 0. 三步开始

### 新版专项 / 配置速查

当前共 20 个预设，原 13 个 ID 保留。新增 `env-native-probes`（原生环境四项）、`closure-target`（具体调用范围的 Context 词法槽）、`session-state`、`websocket-state`、`indexeddb-state`、`wasm-modules` 与 `large-log-spool`。已确认 VM 描述在专页载入，组合不会猜测或自动确认。

- `wasmModules` 是完整模块输入/字节，不是 `wasm` DrumBrake 指令。
- `chunkSpool` 持续保存无损 zstd 块；`streamTCP` 传输宿主已产生的块，不是 V8 线程直连。分块 ACK 与独立最终提交分开核对。
- 「配置 / 启动预检」导入替换/递归合并、逐项差异、确认/取消、精确值与未知字段保留；相对路径基于软件目录，不改成配置文件父目录。
- 「证据索引 / 全文」按脚本/函数/request/window 和真实身份范围精确查询；显示筛选不约束生产端。原始行/blob 按 64 KiB 页读取，完整另存返回 hash。
- `gaps.jsonl` 是全量缺口；`gaps.json` / summary 是有界兼容摘要，只看摘要行数不足以判断完整性。

完整新工作流和边界见 [配置与证据索引说明](runtime-cpp/配置与证据索引说明.md)、[网络状态专项](runtime-cpp/网络状态专项说明.md)、[持续分块](runtime-cpp/持续分块说明.md)、[VM / WASM 模块](runtime-cpp/VM探针与WASM模块说明.md)。

1. 启动发行目录的 `V8Trace.exe`，填写网址、Chrome 路径、**新的**日志目录。UI 相对路径基于 `V8Trace.exe` 所在目录：浏览器默认 `./chromium/chrome.exe`，日志默认 `./logs/capture-时间戳`。
2. 选择“① 网站初探 / 低开销基线”，点击 **一键应用**。先确认浏览器能正常打开目标；需要本地 V8 补环境时，下一轮再应用“② 补环境 + 纯协议（环境取证）”。这两项都默认走配套定制 Chromium 的非 CDP 通道。
3. 点击 **开始采集**，手动完成目标动作；点击 **停止并保存**，等保存、解码结束。先检查“采集摘要 / 缺口”，再导出案例或传输日志。

只有安装版 Chrome 时，先用“⑫ CDP 性能 / 生命周期（采样）”，或手动配置 CDP 基础通道。逐次 JSCall、Native 参数、Ignition Opcode 和 WASM 内部轨迹属于定制 Chromium 插装能力。

### 先记住这八条

- **选择组合只预览，点击“一键应用”才改变配置**；应用不启动浏览器。
- **参数与选项在开始采集前确定**。运行中只支持暂停、恢复、停止；不支持临时改通道或过滤规则。
- **CPU 调用采样 ≠ 逐次 JSCall；性能事件 ≠ Native 参数；Ignition Opcode ≠ 网站 JSVMP 字节码。**
- 调用过滤只约束 **JSCall / Native**，不约束 Env Hook、HTTP、源码、Opcode、WASM。
- 脚本过滤是**区分大小写的整串 glob**，每行一条，支持 `*`、`?`；不是 RuyiTrace 的逗号/分号子串列表，也没有 `web`、`iframe` 语义 token。
- 暂停停止记录，**网页继续执行**；恢复不补录暂停期间日志，也不恢复普通 JIT 性能。
- UI 只显示最近 1000 条预览；预览省略、详情显示截断不等于磁盘日志丢失。
- `complete` 只表示软件在当前范围内未发现关键缺口；不表示全站覆盖、补环境成功或服务端业务成功。

## 1. 按目标选择组合

下表缩写：**B** = HTTP、脚本源码、Console、Exception；**J** = 逐次 JSCall；**N** = Native 参数；**R** = 关联证据；**E** = Env Hook；**D** = 重定向正文补读；**O** = 对象快照。

所有组合默认关闭原始 CDP 消息、无关密集通道；“全通道”指原生组合所覆盖的通道，不包括仅 CDP 的采样和生命周期。时长是默认值，可调整；启用随停随开后按有效采集时间累计。MiB = 1024 × 1024 字节。

| UI 组合 / 配置 ID | 实际开启 | JS 调用值 | 目标脚本必填 | 秒 / 事件预算 / MiB | 用途与开销 |
| --- | --- | --- | --- | --- | --- |
| ① 网站初探 / 低开销基线 `capture-baseline` | B | J 未开启 | 否 | 30 / 300000 / 256 | 新站点首轮，低开销但并非零影响 |
| ② 补环境 + 纯协议（环境取证） `env-protocol` | B、E、D | J 未开启 | 否 | 30 / 1000000 / 512 | 本地 V8 执行关键 JS 所需的环境和网络证据，中开销 |
| ③ JSCall 结构定位（不读参数值） `jscall-structure` | B、J、R | 关闭 | 否 | 15 / 1000000 / 512 | 逐次定位入口、退出及来源，中高开销 |
| ④ 补环境 + 纯协议（目标调用详情） `env-target-detail` | B、E、D、J、N、R | 开启 | 是 | 10 / 1000000 / 1024 | 补齐已知目标调用值，高开销 |
| ⑤ 签名 / 加密（目标 JS 完整调用值） `sign-call` | B、J、N、R | 开启 | 是 | 10 / 1000000 / 512 | 定向计算入口、输出及 Native 边界，高开销 |
| ⑥ Native 参数（目标脚本 / 可选子类） `native-target` | B、N | J 未开启 | 是 | 10 / 1000000 / 512 | 只补 Native 值，可进一步选择子类，高开销 |
| ⑦ 对象结构快照（目标调用补证） `snapshot-target` | B、J、N、O、R | 开启 | 是 | 10 / 1000000 / 1024 | 对象依赖补证，很高开销；值图默认 3 层 / 128 节点 / 512 属性 |
| ⑧ Ignition Opcode / JSVMP 候选取证 `jsvmp-opcode` | B、Opcode | J 未开启 | 否 | 5 / 1000000 / 1024 | V8 实际指令与寄存器；后处理查 VM 候选，很高开销 |
| ⑨ WASM 内部指令（独立通道） `wasm-execution` | B、WASM | J 未开启 | 否 | 5 / 1000000 / 512 | DrumBrake 实际 WASM 指令与带类型栈值，很高开销 |
| ⑩ HTTP 正文 / 纯协议对照 `http-protocol` | B、D | J 未开启 | 否 | 30 / 300000 / 256 | 请求与响应对照，低至中开销 |
| ⑪ Console / 异常 / 调用结构排查 `exception-call` | 源码、Console、Exception、J | 关闭 | 否 | 15 / 1000000 / 512 | 异常调用结构；**没有 HTTP，也没有关联索引**，中高开销 |
| ⑫ CDP 性能 / 生命周期（采样） `cdp-performance` | B、CPU 采样、性能事件、生命周期 | J 未开启 | 否 | 30 / 300000 / 256 | 唯一默认 CDP 组合，中开销；不是完整参数证据 |
| ⑬ 全通道短时诊断（明确高开销） `all-short` | B、E、J、N、O、R、Opcode、WASM、NetLog | 开启 | 否 | 3 / 1000000 / 1024 | 极高开销；不含重定向正文补读，不作为复杂站点首轮 |

定向组合 ④⑤⑥⑦：在“定向组合：目标脚本 URL / 文件名 glob”填写具体规则后再应用。空白、`*`、`??*` 这类纯通配规则不作为有效定向范围。它们默认排除未知来源，并显示 `*diagnostic-hooks*`、`probe.js` 排除规则；排除也是证据范围的一部分。

应用组合会覆盖全部通道、调用过滤、JSCall 值模式、Native 子类、对象预算及宿主缓冲；保留网址、目录、无头选项、表达式和随停随开选项。之后修改目标范围，应**重新应用**，或直接在“调用过滤”修改实际规则。

### 两条常用路线

- **补环境 / 纯协议**：① → ② → 按缺口选择④、⑥、⑦。重点是本地 V8 跑通关键 JS，再以其输出驱动协议请求；组合本身只采日志，不执行这两步。
- **纯算 / JSVMP**：① → ③ → ⑤ → 必要时⑧或⑨ → 对原始证据分析与验证。没有“一键反编译并还原代码”的开关。

## 2. 两种采集方式

| 能力 | 安装版 Chrome + CDP | 定制 Chromium + CDP | 定制 Chromium + 原生无 CDP |
| --- | --- | --- | --- |
| HTTP 正文、源码、Console、未处理异常 | CDP 通道 | CDP 通道 | 对应原生插装通道 |
| Env Hook | 浏览器内诊断包装器 | 浏览器内诊断包装器 | 原生注入的诊断包装器；仍是 Hook |
| CPU 调用采样、Frame / Worker 生命周期 | 支持 | 支持 | 这两个选项不可用 |
| V8 / Blink 性能事件 | 支持 | 支持 | 支持；不等于 Native 参数 |
| 逐次 JSCall / Native 参数 / Opcode / WASM 内部 | 不具备对应插装 | 需要匹配插装版本 | 需要匹配插装版本 |
| 原始 NetLog | 本软件 CDP 模式不启用 | 本软件 CDP 模式不启用 | 独立网络诊断选项 |
| 原始 CDP 消息、页面停止表达式 | 可选 | 可选 | 不启用、不执行 |
| 随停随开 | 由 C++ 内核启停事件域 | 由 C++ 内核启停事件域 | 需 `v8trace/recording/1` 浏览器 |

非 CDP 不创建调试端口、不连接 CDP，也不自动回退。原版 Chrome 的原生模式只可用于相应基础诊断：取消随停随开，关闭需插装的通道，仅选择性能事件 / NetLog；不把它当作原生 HTTP 正文采集器。

## 3. 16 个采集项目速查

“初始”指刚打开 UI、尚未应用组合时的勾选值；初始方式为 CDP。**应用组合后以下初始值被组合替换。** JSON 布尔值为 `true` / `false`，不是字符串或 `1` / `0`。

| UI 开关 | JSON 字段 / 初始 | 记录什么、输出在哪里 | 条件与主要代价 |
| --- | --- | --- | --- |
| HTTP / 请求与响应 body | `http` / 开 | `events/http.jsonl`、`catalog/resources.jsonl` 及 body blob | 两模式；原生需 browser 通道；正文是浏览器可观察的内容，不是网卡包 |
| 重定向正文补读 / 定制浏览器（最多 5 秒） | `redirectBodies` / 关 | 对重定向事务进行有界正文补读，结论随资源记录保存 | 必须开 HTTP，需 `v8trace/redirect-body/1`；最多 5 秒并受单 body 预算限制，不保证每个重定向都有正文 |
| 网络诊断 / 原始 NetLog（非 CDP） | `netLog` / 关 | `native-output/netlog.json` 和规范化 `netlog-event` | 仅非 CDP；原始网络事件，不代替解码 HTTP 正文；体积较大 |
| 脚本源码 / eval / 动态脚本 | `scripts` / 开 | `catalog/scripts.jsonl`，源码 blob 及 UTF-16LE 副本 | CDP 取源码或原生 V8 source 通道；不表示暂停前脚本可补齐 |
| Env / API 参数、返回、异常（Hook） | `hooks` / 关 | 选定 API 的 `installed`、入口、返回、抛出等，`events/env.jsonl` | 诊断包装器会改变函数身份和时序；不是任意属性读取或全量环境清单 |
| JSCall / CPU 调用采样 | `cpuProfile` / 开 | `events/jscall.jsonl` 内的采样相关记录与 CPU profile blob | 仅 CDP；看热点，不保证逐次调用和实参 |
| Native / V8、Blink 性能事件 | `nativeTimeline` / 开 | `events/native.jsonl` 内的性能事件与原始 trace | 两模式；是时间线，不是完整 Native 值 |
| Frame / Worker / 上下文生命周期 | `contextEvents` / 开 | `events/lifecycle.jsonl` 及 CDP context / target 索引 | 仅 CDP；没有该选项不代表其他通道绝无 Worker 事件，覆盖按实际证据核对 |
| Console / 控制台日志 | `consoleEvents` / 开 | `events/console.jsonl` | Console API 日志，不等于所有浏览器内部诊断输出 |
| Exception / 未处理异常 | `exceptionEvents` / 开 | `events/exception.jsonl` | 未处理异常通道；不是所有被 `try/catch` 吃掉的异常。调用详情可另记录受观测调用的异常边界 |
| JSCall / 逐次调用结构（详情在调用过滤页选择） | `jsCallDetail` / 关 | `events/jscall.jsonl`、函数字典解码与调用计数凭据 | 需 detail 插装；分层结构模式需 call/2；值序列化进一步增加开销 |
| Native / API、Runtime、builtin 参数 | `nativeDetail` / 关 | `events/native.jsonl`，实际参数、返回和异常 | 需 detail 插装；仅已覆盖的 JS 可见 Native 边界，不是所有 C++ 函数 |
| WASM / 实际指令与带类型栈值 | `wasm` / 关 | `events/wasm.jsonl` 及原始 trace | 需 WASM 插装，DrumBrake 解释执行；不是静态反汇编，很高开销 |
| 对象快照 / 自有属性、数组及访问器描述 | `objectSnapshots` / 关 | 记录值中的对象图、属性和边界 | 需 objects/1，搭配值通道；读自有存储，不执行 getter / Proxy；不是全堆快照 |
| 关联证据 / 调用、源码与网络（含边界） | `correlations` / 关 | `catalog/calls.jsonl`、`script-index.jsonl`、`correlations.jsonl`、`hook-spans.jsonl` | 同时开启逐次 JSCall、源码、HTTP；后处理也耗时耗内存，候选关联不等于因果 |
| Opcode / 原生 Ignition 指令与寄存器 | `opcode` / 关 | `events/opcode.jsonl`、`opcode-summary.json`、原始 trace | 需 opcode 插装；解释执行，很高开销；JSCall 的脚本过滤不限制它 |

### Env Hook 的具体范围

当前选定成员包括 Navigator / WorkerNavigator 部分属性、Screen 尺寸、Document / Element 部分查询和属性操作、Canvas 部分 API、Storage 方法、XHR 方法、Crypto / SubtleCrypto 部分方法、`document.cookie`、`fetch`、定时器、`queueMicrotask`、`atob` / `btoa`、`Math.random`。以 `installed` / `install-failed` 及真实调用为准。

它不是“所有 DOM/BOM 属性都已覆盖”：例如任意不存在属性探测、全部接口库存、IndexedDB 全量读写值、闭包变量全集、每个 WebSocket 帧和 API 返回值覆盖，都没有与 RuyiTrace 同名同义的独立 UI 开关。不要由参考手册反推本软件已经支持。

## 4. JSCall 与 Native 的细分类

位置：左侧 **调用过滤** 页。过滤在生产端构造参数 / 快照之前执行，和右侧事件显示筛选不同。

| UI 选项 / 配置字段 | 取值及未应用组合时的状态 | 用途 / 生效条件 |
| --- | --- | --- |
| JSCall 参数 / this / 返回值 / 异常详情 `jsCallValues` | 初始开 | 必须启用 `jsCallDetail`。关掉后仍逐次记录结构，不读取 JS 调用值；不是 CPU 采样；不关闭 Native 参数 |
| 启用 JSCall / Native 生产端过滤 | 初始关；UI 总闸，无同名 JSON 字段 | 开启后 UI 才发送下面的规则。关掉时发送空规则、全部 Native 子类、保留未知来源、不排除内部脚本 |
| 调用范围：脚本白名单 `detailScriptInclude` | 多行 glob，初始空 | 留空表示不以已知脚本白名单限制调用；作用于 JSCall 来源及 Native 的实际 JS 调用方 |
| 调用范围：脚本黑名单 `detailScriptExclude` | 多行 glob，初始空 | 对已知脚本，黑名单优先于白名单 |
| JS 函数名白 / 黑名单 `jsFunctionInclude` / `jsFunctionExclude` | 多行 glob，初始空 | 限定 JSCall 的被调函数名称；不用于筛选 Native 的 JS 调用方函数名 |
| Native 名称白 / 黑名单 `nativeNameInclude` / `nativeNameExclude` | 多行 glob，初始空 | 限定 Native 名称；按实际日志名称填写，黑名单优先 |
| 仅 JS 详情：脚本白 / 黑名单 `jsValueScriptInclude` / `jsValueScriptExclude` | 多行 glob，初始空 | 与调用范围取交集。范围外仍记录已选 JS 调用结构，标记 `valuesCaptured=false` |
| 仅 JS 详情：函数白 / 黑名单 `jsValueFunctionInclude` / `jsValueFunctionExclude` | 多行 glob，初始空 | 同上，还需 `jsCallValues=true`；四个详情范围均空时，已选 JS 调用都取值 |
| Native 子类 `nativeKinds` | UI 初始全选；JSON `all` 或逗号分隔的下表 ID | 与来源、Native 名称条件共同生效；至少选一个；单项不自动打开 Native 通道 |
| 排除采集器内部、浏览器 UI 和扩展脚本 `detailExcludeInternal` | 控件初始勾选，但总闸关闭时不生效 | 按内部 scheme / 前缀排除。不是全网络资源过滤 |
| 来源未知的 Native / 动态代码 `detailUnknownSource` | 控件初始“排除来源未知”，但总闸关闭时实际为 `include` | `exclude` / `include`。未知来源按此项判定，选 `include` 时不再受已知脚本白名单限制；之后仍受对应名称 / 子类条件限制 |

JSON 没有 UI 的过滤总闸：直接填写规则即参与规范化。所有 10 个规则字段合计最多 128 条，单条最多 2048 个 UTF-16 码元，编码后的过滤 token 最多 16 KiB；超过会明确报错，不悄悄丢规则。

### 4.1 七类 Native

| UI 子类 | `nativeKinds` ID | 看什么 |
| --- | --- | --- |
| JS 可见 builtin | `builtin-js` | 已插装的 JS 可见 builtin 边界 |
| C++ builtin | `builtin-cpp` | 已插装的 C++ builtin 边界 |
| V8 Runtime | `runtime` | 已插装的 Runtime 调用 |
| API 方法 / 构造 | `api-operation` | Blink / DOM 等 API 方法、构造调用 |
| API 属性读取 | `api-getter` | API getter |
| API 属性写入 | `api-setter` | API setter |
| API 拦截器 / 枚举 | `api-interceptor` | 对应 API 拦截 / 枚举边界 |

JSON 例：`"nativeKinds": "api-operation,api-getter,api-setter,runtime"`。`all` 只可单独使用；不接受重复 ID 或未知 ID。

### 4.2 匹配规则与例子

| 规则 | 含义 |
| --- | --- |
| `target.js` | 文件名精确匹配；URL 的查询参数和片段先去掉 |
| `*target*.js` | 文件名 glob；匹配包含 target 且以 .js 结尾的文件名 |
| `https://host.test/assets/*.js*` | 含 `/`，匹配完整 URL；末尾 `*` 可匹配查询参数 |
| `*sign*` / `encode*` | 函数名 glob；不是自动子串检索，也不是正则 |
| 多行规则 | 同一白名单内部任一命中即可；不同维度共同限制；黑名单优先 |

不要把 `target.js,other.js` 写在同一行当作两个规则。`web`、`iframe` 会被当作普通文件名，而不是页面范围。未知来源可能是动态代码或缺少来源信息，不等于“浏览器噪声”；先看计数和目标覆盖再决定。

**只抓 target.js 的完整 JSCall 与 Native**：勾选两通道及 JSCall 详情，再开启生产端过滤，调用脚本白名单填 `target.js`，未知来源选排除；Native 子类按需求选择。HTTP、Env、Opcode 等仍按各自选项运行。

**结构多一点、值少一点**：打开逐次 JSCall 与详情；调用脚本白名单填实际需要的多个文件；“仅 JS 详情”脚本填 `target.js`，函数填 `sign*`。其余已选调用仍记结构。Native 详情不受“仅 JS 详情”限制。

## 5. 随停随开

| 控件 / 字段 | 初始 | 操作与条件 |
| --- | --- | --- |
| 启用随停随开（浏览器保持运行） `recordingControl` | UI 开；CLI 未设时关 | 开始前选择；非 CDP 需匹配 `v8trace/recording/1` 浏览器 |
| 启动后先不采集，手动开启 `startPaused` | 关 | 要求上一项开启；页面启动后先运行，准备好再点底部“开启采集” |
| 底部 暂停采集 / 开启采集 | 采集就绪并支持控制后可点 | 可反复切换。等待生产端确认时暂时禁用，不要把请求已发送当成已暂停 |
| 停止并保存 | 任务运行时可点 | 永久结束本次采集，排空缓冲、保存和解码，并关闭本次启动的浏览器；暂停期间也可使用 |

- 暂停与切换刷盘时间不计入有效采集时长；有效窗口合计到设定时长后结束。暂停不重置预算，也不创建新 capture。
- 暂停不暂停 JS、请求、登录态或页面计时器；不卸载包装器，不恢复原生 JIT。
- **跨窗口的请求、调用、异步回调可能不完整**。不要把开始暂停前的请求与恢复后的响应强行当作完整配对。
- CDP 恢复时可能枚举已有源码；原生模式不承诺追补此前已加载的源码。若需完整启动链，首轮不要“启动后先不采集”。
- 窗口、确认及计时见 `recording-windows.json`、`recording-control.jsonl`、`recording-summary.json`；原始 trace 分段保留。

## 6. 高级选项与预算

以下为新开 UI 的值，应用组合后会改变其中预算。数值均为正整数，**0 不是不限量**。

| UI 字段 / JSON 字段 | 初始 / UI 范围 | 作用与注意事项 |
| --- | --- | --- |
| 采集时长 `durationSeconds` | 60 秒 / 1–86400 | 不是目标动作完成保证；短时 Opcode 可能在关键脚本执行前就结束 |
| 事件预算 `maxEvents` | 100000 / 1–1000000 | 规范化事件准入上限，耗尽记录关键缺口，不抽样补齐 |
| 日志 / blob 预算 `maxBytes` | 256 MiB / 1–4096 MiB | 宿主日志 / blob 预算，不是整个目录或进程内存的硬上限；原始 trace、profile、重复副本和索引还占空间 |
| 单个 body 上限 `maxBodyBytes` | 16 MiB / 1–1024 MiB | 大正文超限会留下原因；它不是调用字符串长度上限 |
| 宿主日志写缓冲 `writeBufferBytes` | 8 MiB / 1–256 MiB | 满时背压等待；不采样、不丢行。CLI 最低可设 4 KiB |
| 宿主连续日志批量 `writeBatchBytes` | 1024 KiB / 4–4096 KiB | 不大于写缓冲；批量聚合，不改变完整事件内容 |
| 对象快照最大层数 `objectDepth` | 2 / 1–8 | 值图预算，达到边界明确标记；不代表完整堆 |
| 单个值图对象节点上限 `objectNodes` | 64 / 1–4096 | 同上；大对象图增加 CPU、内存、日志量 |
| 单个值图属性 / 元素上限 `objectProperties` | 256 / 1–65536 | 同上；访问器描述不等于执行 getter 的结果 |
| 无头模式（自动化测试） `headless` | 关 | 切换浏览器显示方式；与日志通道 / 非 CDP 是不同维度，页面行为可能不同 |
| 保留已启用通道的原始 CDP 消息 `rawCdp` | 关 | 仅 CDP，输出 `cdp.jsonl`；只含已启用域，额外占用预算，不是完整 CDP / 网络抓包 |
| 停止条件表达式 `untilExpression` | 空 | 仅 CDP，显式在页面求值，返回 `true` 后停止；暂停时不轮询，表达式本身可能改变页面 |
| 案例导出新目录 | 空 | 导出时使用；留空由 UI 生成新目录，不覆盖已有案例 |

**缓冲分四层看**：浏览器值序列化 / 插装 → Perfetto trace 缓冲 → C++ 宿主落盘队列 → 保存后 TCP 传输。调大宿主缓冲不消除前三者中的浏览器执行成本，也不保证 Perfetto 零丢失。超大单条记录保持完整、独占队列，实际内存仍需容纳该条数据。

## 7. TCP 日志传输

左侧 **TCP 日志传输**：接收端地址、端口（默认 `17880`）、令牌文件；可手动“传输当前采集包”，也可勾选“采集保存后自动 TCP 传输”。任务保存后再传，不是浏览器实时推流。

从发行目录运行，接收端和发送端分别执行：

```powershell
# 接收端：本机回环演示
.\runtime\v8trace-runtime.exe receive --root .\received --bind 127.0.0.1 --port 17880

# 发送端：替换为实际已保存的目录
.\runtime\v8trace-runtime.exe send --capture .\logs\capture-001 --host 127.0.0.1 --port 17880
```

跨机器时两端都加 `--token-file .\transfer.token`，文件内容相同且至少 16 字节，监听地址使用接收机实际地址。协议为明文 TCP，应放在可信网络或加密隧道内；令牌不是 TLS。

Zstd 无损压缩、4 MiB 分块、块 / 文件 / 最终提交 SHA-256 校验、断线续传；默认最多 4 次连接尝试。传完整原始 trace、日志、源码和正文，不传 browser profile，不删除发送端原件。它减少搬运字节，不减少原生采集的本地 trace 体积，不修复插装引起的页面白屏。

## 8. 输出文件在哪、怎么读

以下路径相对**本次采集目录**。只开启部分通道、采集失败或没有该类事件时，有些文件不生成；空文件 / 缺文件本身不构成“目标没调用”的证明。

| 路径 | 内容 / 阅读方式 |
| --- | --- |
| `summary.json`、`gaps.json`、`gaps.jsonl` | 总结论和最多 200 条 / 合计 4 MiB 的缺口样例摘要；完整明细见 gaps.jsonl，gapJournal 保存数量/字节/hash，omittedGapCount 仅是摘要省略数 |
| `launch.json`、`browser.json`、`capabilities.json` | 本轮启动参数、实际配置、浏览器身份与能力；按实际 transport 分析 |
| `events/http.jsonl`、`script.jsonl`、`env.jsonl` | HTTP、源码元数据、环境事件；每行一条 JSON |
| `events/jscall.jsonl`、`native.jsonl` | 逐次调用 / 采样、Native 详情 / 性能事件；靠 `kind` 和能力区分 |
| `events/opcode.jsonl`、`wasm.jsonl` | 规范化 Ignition / WASM 执行轨迹 |
| `events/console.jsonl`、`exception.jsonl`、`lifecycle.jsonl` | 控制台、异常、CDP 生命周期 |
| `catalog/resources.jsonl`、`catalog/scripts.jsonl` | 请求响应、正文和源码的 blob 引用、SHA-256、长度与覆盖结论 |
| `blobs/sha256/<哈希>` | 原始内容寻址文件，不一定是 UTF-8 文本；按引用中的 representation 解释 |
| `native-output/trace.json` 或 `native-output/cdp-trace.json` | 原始 Chrome trace，包含 `traceEvents`；JSVMP 分析使用它，不用规范化 JSONL 代替 |
| `native-output/trace-N.json` / `cdp-trace-N.json`、`netlog-N.json` | 随停随开时逐窗口原件；归并 trace 保留 `v8traceWindow` |
| `recording-control.jsonl`、`recording-windows.json`、`recording-summary.json` | 暂停 / 恢复确认、窗口索引、有效和暂停时长 |
| `jscall-writer-summary.json`、`detail-summary.json`、`detail-filter-summary.json`、`opcode-summary.json` | 字典 / 调用计数、详情和过滤、指令覆盖；按已用协议实际生成 |
| `browser-channels-summary.json`、`native-decoder.json` | 原生浏览器通道、流式解码与处理状态 |
| `catalog/calls.jsonl`、`script-index.jsonl`、`correlations.jsonl`、`hook-spans.jsonl` | 关联开启后生成的索引，路径均在 catalog 下；总体结论在 `correlation-summary.json` |
| `native-output/browser-stdout.log`、`browser-stderr.log` | 原始浏览器诊断字节；另有对应 JSONL 的 Base64 保真记录，不是网页 Console 业务通道 |
| `cdp.jsonl`、`tcp-transfer.json` | 分别为可选原始 CDP 消息、已完成传输回执 |
| `browser-profile/`、`native-control/` | 本次 profile、原生启停控制文件；不是分析结果，运行时勿删改 |

`events` 目录内的 `native.jsonl` 等是完整相对路径的省写，例如 `events/native.jsonl`。不要按 RuyiTrace 的 `jscall/*.bin` 格式读取这里的规范化 JSONL，也不使用其 Firefox 解码器。

### 最少验收清单

1. `captureConclusion` 不是 `failed`，且启动 / 导航 / 目标动作确实发生；零事件的“保存成功”不算目标取证成功。
2. 检查 `integrityConclusion`、`criticalGapCount`、`omittedGapCount`、全部 `gaps`；`partial` 保留原样，按影响范围补采。
3. 检查原始 trace 最终刷盘和丢失统计；用了 call/2 时检查函数字典、序号、最终计数凭据及 `jscall-writer-summary.json`。
4. 关键请求的 request / response / body / postBody 结论和 blob 可读、长度、SHA-256 对得上；Console 中有结果不代替 HTTP 正文证据。
5. 关键调用检查 `valuesCaptured`、入口 / 返回 / 异常、`parentExact`、实际脚本、进程 / isolate / context 和窗口；候选关联与确证事实分开。
6. 对象值图边界、body 预算、过滤排除、暂停边界都记录到交接文档。完整输出不等于无边界、无预算或无限覆盖。

## 9. 常用命令速记（C++，不依赖 Node.js）

工作目录为发行根目录；`capture-001`、`case-001`、`vm-001` 都替换成实际新目录 / 已有采集包。

```powershell
.\runtime\v8trace-runtime.exe --version
.\runtime\v8trace-runtime.exe help
.\runtime\v8trace-runtime.exe plan --config .\capture-options.json
.\runtime\v8trace-runtime.exe capture --config .\capture-options.json
.\runtime\v8trace-runtime.exe inspect --capture .\logs\capture-001
.\runtime\v8trace-runtime.exe export --capture .\logs\capture-001 --output .\cases\case-001
.\runtime\v8trace-runtime.exe vmp-discover --trace .\logs\capture-001\native-output\trace.json --output .\analysis\vm-001
```

`plan` 校验配置、浏览器身份及输出新目录，不启动浏览器；不是脱离二进制的纯语法检查。CLI 相对路径以进程当前工作目录为准，UI 则固定以软件目录为准。

导出后的 `main.template.js` 是待选入口，`replay.network.jsonl` 是网络证据，不是已经运行通过的复现实现。随停随开使用 UI 或 Worker 控制；不要把 `startPaused=true` 放入普通非交互 `capture` 后期待键盘命令能恢复。

## 10. 快速排障

| 现象 | 先查什么 / 怎么调整 |
| --- | --- |
| “一键应用”不可点 | 内核是否就绪、是否仍在采集 / 传输、定向组合是否填写了具体脚本范围 |
| 原生协议缺失 / 哈希不匹配 | `v8trace-opcode.json` 与实际 chrome.exe、引擎 DLL / Framework 是否配套；用支持对应功能的完整浏览器包，不手改身份文件绕过校验 |
| 启用随停随开后旧浏览器报错 | 新功能需 recording/1。使用配套新版；旧版若只需原持续采集可取消此项，仍需满足其他所选通道协议 |
| 详细采集白屏 / 很卡 | 从①开始，同一动作依次比较②、③、⑤、⑥、⑦、⑧ / ⑨；先收窄 JSCall / Native，再分离值、子类、快照与指令成本，不直接归因于 JIT |
| 只想抓某 JS，日志仍很大 | 检查过滤总闸、真实 URL、大小写、未知来源策略；Env、HTTP、源码、Opcode、WASM 不受这个过滤约束 |
| 只有结构没有参数 | `jsCallValues`、仅 JS 详情范围、`valuesCaptured`、后端版本；对象快照不会强行给结构模式补值 |
| HTTP 正文缺少 / partial | body 上限、重定向补读、请求是否完成、暂停边界、资源结论及原始错误；只开 NetLog 不会生成等价正文 |
| UI 事件条数少 | 最近 1000 条预览、尾部读取范围和显示筛选；直接核对磁盘 JSONL 与 summary 计数 |
| Chrome 启动退出 code 3 / 2147483651 | 看 browser-stderr、Windows 事件 / 崩溃证据、路径权限和模块；2147483651 为 `0x80000003` 断点类状态，不是唯一根因。不要一律判定 CDP、权限或输入法 |
| TCP 日志没立刻出现在远端 | 当前是保存后传输；检查接收进程、监听地址、令牌、端口、防火墙和完整提交回执 |

详细操作、证据交接模板及逐项排障见 [操作手册](操作手册.md)。

## 原生环境取证扩展

定向闭包：调用过滤页 `closureBindings` 默认关闭，需具体 JSCall 白名单；与参数值采集独立，按阶段记录 Context 捕获绑定。五项预算和动态/模块/栈边界见 [闭包快照说明](runtime-cpp/闭包快照说明.md)。

“原生环境取证”页四项均默认关闭：`envAbsentProbe` 缺失候选、`envInterfaceProbe` 接口查询、`envHostMutation` 宿主改动、`envInventory` 当前 realm 原始接口库存。它们有独立的 `envScriptInclude` / `envScriptExclude`，不沿用调用过滤。使用 Ignition 并关闭 JS 优化；库存不执行 getter、Proxy、惰性构造器。普通读取的 undefined 不等于属性不存在。具体输出和边界见 [原生环境取证说明](runtime-cpp/原生环境取证说明.md)。
