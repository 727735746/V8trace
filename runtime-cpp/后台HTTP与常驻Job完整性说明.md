# 后台 HTTP 与常驻 Job 完整性

本文使用 UTF-8 中文记录。R8 是独立候选修复，不覆盖已发布稳定版；配套 Chromium 完成编译和真实采集验收后，才具有对应的原生能力。

## 后台 HTTP

- Network 层在请求建立时，把确切父请求 ID 绑定到同一个 `URLRequest`。重定向正文开始时即记录父 ID、`v8trace/http-parent/1`、URL 与响应头，不再依赖稍后才发生的重定向通知。
- 请求在重定向通知之前取消时，已收到的正文仍归属其真实父请求。记录 `bodyPrefix`、原始字节哈希、实际失败错误和取消回执；正文保持 `complete:false`，请求保持 `failed`。
- `URLLoader` 销毁且尚未完成时补记真实 `ERR_ABORTED` 终止；已完成请求不会重复结束，也不会为未观测请求制造新状态。
- 父 ID、进程、协议、URL 冲突，字节偏移或数量缺失、结束回执缺失、超时和预算截断，仍产生关键缺口。记录停止时仍在进行的后台请求也保留未完成状态。
- 旧日志按原有确切重定向 ID 导入。导出旧格式时不附加空的 `bodyPrefix` 字段，保持逐字段兼容。

## 有见证的结束顺序（新配套候选）

本节描述 2026-10-31 的修复候选，不代表既有发布包已更新，也不代替配套浏览器的实际编译和网页验收。

- 当同时选择 Native HTTP、NetLog，且浏览器身份声明 `v8trace/http-drain/1` 时，宿主使用受身份令牌约束的原生结束协议。即使未启用用户暂停按钮，也只在内部协调录制；不静默开启未选择的网络见证。
- 时长结束后，先将本采集器浏览器中的标签导航至 `about:blank`，停止页面继续生成请求，并记录导航提交回执。该导航属于明确的排空阶段，不是业务成功或验证通过。
- 在 trace 仍开启时，增量读取原生 NetLog 的 `REQUEST_ALIVE` 起止身份，并校验其来源为 `URL_REQUEST`，等待已观测请求全部结束及至少 1500 ms 安静窗口，再发出真正的停止和 flush。事件类型和来源均以当前 Chromium 原始常量表为准；未知表仍报采集缺口。安静窗口本身不是完整性证明，最终仍检查请求、上传、正文、原生计数器和根图。
- `http-drain.json` 保存见证统计；未结束请求、解析异常或排空超时仍保留关键缺口。旧浏览器保持原命令，未观测终态仍按未完成处理，不伪造请求完成。
- 支持 `v8trace/native-flush-timeout/1` 的浏览器接收实际 `traceFlushTimeout`，用于 Perfetto producer flush 与 data-source stop。内部传输接受 100–3600000 ms；宿主为两阶段及文件提交保留独立的有界等待。缺失或失败的 flush 统计仍按原始事实报告。

## 常驻 Job

任务源的生命周期和某次调度工作批次不同。任务源保持分配，不表示它始终还有待执行工作；反过来，短暂空闲也不是任务结束证据。

原生 `v8trace/causality-model/3` 将它们拆开：

1. 实际入队、原生确认有可运行工作的通知或显式 Join 建立 `job` 工作批次；同一任务源再次唤醒时使用新的批次身份。任务源已经在队列中、通知省略再次入队的路径也会记录新工作；零工作虚假通知不制造新批次。`activationNativePoint` 标明实际激活位置。
2. 调度通知记录为 `job-notification-provenance`。Chromium 允许虚假唤醒通知，因此通知本身不是一个独立待执行回调。日志明确记录其语义及是否被执行消费。
3. worker 实际获准运行、join 和轮询时，连接已知通知来源。通知发生在已开始的 worker 中时，建立实际进入的后续执行段，不给旧调用补入反向父节点。
4. 批次结束只接受原生调度点的证明：实际 worker 数为零且最大并发为零，或者取消后 worker 已全部返回；零 worker 的实际 Cancel 也有独立回执。证明包含批次代数、通知数量、原生位置、是否取消、通知消费情况。
5. 并发入队或新 worker 活动会使旧快照失效；整个原生准入决策区间（包括读取状态和最大并发、原子计数增加及记账）都受保护，检查时还必须没有正在准入的 worker。决策完成时只提交该决策自身仍有效的票据，不在旧读数之后重新取一个新票据冒充新状态。通知提示本身不伪造新的可执行批次，迟到的已取消准入拒绝也不制造新批次或虚假的丢失登记缺口。
6. `job-source-lifetime-end` 仅记录实际析构；批次结束不会伪称任务源被销毁。已有 worker、异步后继和未决回调仍分别等待真实终止。

## 分析与兼容

- 新分析器继续读取 model 2 的旧日志；旧日志缺少新原生工作证明时，仍保留原本的未决 Job，而不是重新判定为完整。
- 新 Job 通知语义要求 model 3 和确切元数据。新的工作结束原因还要求唯一、顺序正确、类型正确的原生零工作证明。
- `passiveProvenanceCount` 仍仅统计 Promise 来源；`passiveJobNotificationCount` 单独统计调度通知来源。
- `executionObligation:false` 只表示被动来源节点没有独立回调义务，不代表它的真实执行后继已经完成。
- `completeCausality` 是逐请求的证据闭合判断，不等于网站业务复现成功，也不等于所有浏览器后台网络都已结束。

## 验收要求

原生脚本事件的 `sourceStorageProtocol: v8trace/native-script-source/2` 使用已审计的内容寻址引用保存 `source`（UTF-8）与 `sourceUtf16`（完整 UTF-16 code units），脚本目录引用保持一致。每次编译/创建事件、原生身份、原始 trace 和完整源文本均保留，不在事件 JSONL 中再次内联整份大脚本。读取完整文本请解析该 blob 引用或脚本目录；这避免重复源码占满输出预算后挤掉最终 HTTP 记录。仍超过真实数据预算的采集继续明确标记缺口，不截断源码或静默抬高预算。

自动 HTTP 采集（HTTP 与 NetLog 已选择、配套 Native 具有排空协议、用户暂停控制关闭）使用 Chromium 原生 startup buffer，并由唯一的受控 tracing consumer 接管相同配置。`--trace-startup-owner=system` 只指定启动缓冲的归属；不连接 DevTools，也不启动第二个常驻 consumer 或独立 NetLog。原生配置的启动自动停止时长为零，最终仍通过真实排空与 finish/flush 回执结束。

这保留了浏览器启动期间已经发送、但在受控窗口开始后才完成的请求头和正文，避免把跨窗口请求补成猜测值。`recording-summary.json` 与 `recording-windows.json` 明示 startup handoff 和可能早于常规窗口开始的原生前缀；配置本身不冒充实际接管成功，仍须核对原始前缀、请求/发送尝试、正文、丢失统计和逐根证据。显式用户暂停/继续采集不启用这条启动前缀路径。

必须同时检查：主机完整回归、配套 Chromium 实际编译与身份绑定、取消前重定向字节 oracle、多 Worker/307 请求链、真正未决与截断阴性样本、UI/MCP/TCP 回归。候选验收文档记录实际结果，构建中或尚未运行的项目不计为通过。

## 受控原生 trace 的 protobuf 落盘

配套内核声明 `nativeTraceExportProtocol: v8trace/perfetto-file-export/1` 时，受控 HTTP/录制采集在真实最终 flush 与 DisableTracing 回执之后，通过原生 `ReadBuffers` 保留完整 `trace-N.pftrace`。主机随后使用随包固定 SHA-256 的 Perfetto Trace Processor，将这个只读原始文件导出为 `trace-N.decoded.json`，再走原有完整 JSON 流合并、导入和逐根验证。原始 protobuf、转换结果、工具身份、退出状态、输入/输出哈希与转换时限保存在 `recording-summary.json`；成功转换不等于采集完整。

这条路径避免高密度首栈全链在 TracingService 内构建整份 JSON 索引而触及其进程内存上限。沙箱、Job 限额、浏览器安全参数、类别选择和因果完整性条件保持不变。启动前缀使用相同 protobuf 格式与唯一 consumer；显式暂停控制仍不添加前缀。未声明协议的旧内核继续采用原有 JSON 格式，不向它发送新开关；未受控的独立 startup 采集也仍属于旧 JSON 路径。

转换组件固定为上游 v56.1 Trace Processor，使用 `SELECT EXPORT_JSON` 直接写入新的受控文件。没有采用会在 Windows 临时文件重开失败后仍返回 0、输出空用户事件的 `traceconv json` 包装路径。回归实际核对事件、UTF-8、完整 uint64、进程/线程与绝对微秒时钟，而非只核对进程退出码。

转换最多等待 `2 × traceFlushTimeout + commandTimeout`，原始与解码文件均执行明确字节上限；失败保留原始数据、部分输出与子进程诊断，不换成空事件成功结果。空 Native 输出直接失败；单路 trace 合并失败也保留已经确认的独立 NetLog 与合并错误回执。NetLog 诊断不是已采集的解码 HTTP 正文。最终仍要求 Native flush、无丢失/预算截断、真实点击时序、发送头/正文见证和完整的逐根续体证据同时成立。

## 有界持续落盘协议 /2

源码中的 `v8trace/perfetto-file-export/2` 使用 Perfetto 原生 `write_into_file`，每 1,000 毫秒排空到同一个全新原始文件；文件字节上限来自当前采集原有的 `maxBytes`，不会扩大 1 GiB 内存 trace 缓冲或沙箱/Job 限额。协议 /1 的最终一次 `ReadBuffers` 对有限小样本仍可使用，但整段记录超过内存缓冲时可能在最终排空前已经丢块；最终 flush 成功不抵消 `chunks_discarded`、`patches_failed` 等真实损失。

Windows 桥接将浏览器通过 Mojo 交付的受控文件 HANDLE 以 `_O_WRONLY | _O_BINARY` 接入 CRT 描述符；只有接入成功后才转移所有权。由 tracing service 持续写入并在最终生产者排空后 flush、关闭，随后发送真正的停止回执，浏览器不会再次创建文件端点覆盖该原始文件。显式的采集会话命名空间把提前停止、文件上限、写入/短写和最终 flush 错误报告为失败；其他 Perfetto 会话原有的文件上限语义保持不变。

主机同时核对编译身份中的 /2、实际窗口回执中的周期/协议/十进制字节上限，并检查活动 trace 与 NetLog 的剩余原始字节预算。`recording-summary.json` 的 `nativeLiveFileWitness` 记录经过身份验证、仍为 `recording` 状态且尚未发出 finish 请求时的实际文件长度、首末时间和采样次数。这是文件增长见证，不是配置即成功；实际验收还须检查前后增长、最终原始哈希、转换/full-merge、无丢失与全部 HTTP/逐根证据。新路径的构建、运行与原页面验收以各自回执为准，源码接入不冒充已完成发布。

原生周期排空和文件上限的上游契约见 [Perfetto trace configuration](https://perfetto.dev/docs/concepts/config)。

### 配置经过 IPC 后的真实见证

浏览器设置的 Perfetto 配置不等于 tracing service 收到的配置。固定 Chromium 的 TraceConfig Mojo 映射原先漏传文件周期和上限，并把会话名称限制为普通 UnguessableToken；早期受控 /2 负测因此出现 64 KiB 配置却写出 135,239 字节，主机按原有预算正确判失败，但 Native 回执错误结束为 finished。该失败样本保留，不计入通过。

修复以明确的 uint32 周期、uint64 文件上限和可选 owned-session 字段传递，不借用其他数据源的 opaque 字符串。既有会话 token 字段不变；新命名空间后缀来自完整 128 位随机 UnguessableToken。服务端反序列化同时验证命名空间、有效非零 token、文件模式、1,000 ms、非零上限和互斥的旧会话名。声明 `traceConfigIpcProtocol: v8trace/perfetto-file-config-ipc/1` 前还须匹配全部新增 IPC 源文件，控制器回显不足以声明能力。

验收从只读原始 protobuf 的前 16 MiB 内解析实际 `TracePacket.trace_config`（字段 33），直接核对服务端周期、精确 uint64 上限与随机 owned-session。它是初始服务配置见证，不是整份 trace 验证，也不替代文件持续增长、真实 Native 上限失败、完整流导入、无丢失和逐根完整性。新的实际测试采用独立包/采集目录，不覆盖旧失败回执。

### 连续文件采集的最终统计快照

真实点击回归暴露：定期文件写出已生效，全部七组独立 HTTP 正文匹配，但终止快照未写出；六个原始 TraceStats 的 final_flush_outcome 均为 0。转换器与完整性门禁正确保留失败。原因是定期 ReadBuffers 消耗 should_emit_stats，停用后的最终文件 drain 没有重新设置该标记。

修复只在自有 v8trace/streaming-file/2/ 会话的最终 drain 前重新请求原有统计快照，记录实际最终 flush outcome 与丢失计数，不设置虚假成功、不减弱门禁。缓冲区、周期、捕获预算、事件及安全限制保持原值。最终 Native flush/loss、点击回归及原站验收必须以新的真实产物为准，历史失败不改写。

### 高流量生产端提交与 SMB 丢包

真实开启首栈的回归发现：最终 flush 成功，但仍有 8 次 writer packet loss。原始包同时保留生产端 257（DATA_LOSS_PRESENT | DATA_LOSS_SMB_FULL）和服务端 1 标志，确认是生产端共享内存耗尽，不是转换器误报或首包标记。失败的原始数据和门禁保持原样。

新增精确源码绑定的 v8trace/perfetto-producer-commit/1 能力。仅高流量采集且 Native 明确支持时，经原有 kPerfettoBackendParams 请求 v8trace_batch_commits_ms=0；SDK 在下次调度机会立即批量提交。Native 接受 0..1000ms，默认及越界值保持原版 1000ms。旧版、未知能力及轻量采集保持原有行为。缓冲区容量、页大小、文件写出周期、预算、kDrop 防死锁策略与零丢包验收均不变。参数配置不等于无损证据，仍须由实际统计、正文和首栈全链回归证明。

### 最终 flush 超时的真实 IPC 配置

零延迟提交回归的原始生产端丢包标志降为 0，但真实最终 flush 失败。核验原始服务 TraceConfig 后确认：浏览器已请求 120000ms，原有 GetDefaultPerfettoConfig 也已设置 flush_timeout_ms 与 data_source_stop_timeout_ms；自定义 Mojo 模式及双向 traits 却遗漏这两个字段，服务端原始配置中两者均缺失。

本次只补齐已有设置的透传，未上调请求值、内存/文件预算或安全限制；生产端提交策略保持已验证的 0ms。新增源码绑定的 v8trace/perfetto-flush-timeout-ipc/1 能力，服务侧对自有会话保持既有 100..3600000ms 校验。验收从原始 TracePacket 的配置字段 14/23 核对实际值，拒绝缺失、错值、错线型、重复或溢出。配置正确仍不是 flush 成功或无损证据，最终真实统计、正文和首栈全链标准保持不变。校验器源码按哈希快照到各案例目录，后续工具更新不会替换新回执的校验器内容。

### Windows 大流量文件写出与 IPC 公平性

实际原站压力采集虽然最终 flush 成功且中央缓冲无溢出，仍发生 135 次生产端 SMB 丢包。观察到自有采集服务为约 1.46GB 原始文件执行约 720 万次写操作；原版 Windows writev 为每个向量逐次 WriteAll，且文件 drain 在同一服务线程同步排空所有数据，阻塞 CommitData IPC。

仅自有 Windows streaming-file/2 会话使用固定 64KiB 合并写。周期采集在原有 1MiB 读取块之间让出任务调度，后续块跳过重复 Flush，但保留下一周期原有 Flush；最终或停用状态仍同步排空、flush、关闭后才确认。迟到回调对已关闭文件直接退出。普通会话及其他平台保持原行为。短写/错误仍失败，字节顺序、文件预算、所有缓冲/超时/事件标准不变。新增精确源码绑定的 v8trace/perfetto-file-io/1 能力。

生产候选头已通过 11 项真实 C++ 字节/错误边界测试；实际原始包前缀 67108935 字节的哈希逐字节一致，写调用从 383550 降为 1127，测试环境用时从 2.91747s 降为 1.99593s。此为算法/Windows CRT 形状探针，不是 Perfetto 服务或原站验收；真实编译、点击和原站门禁仍需重新完成。


## 原生脚本源码的无损字符串引用

`v8trace/native-script-interning/1` 在原始 Perfetto 中使用标准序列级字符串引用，保留每一条 compile-input/compiled-source 事件、生产序号、上下文和脚本元数据。源码沿用原有 UTF-16 JSON 序列化；记录携带字面量字节数和 SHA-256，主机重建后继续使用原有 `v8trace/native-script-source/2` UTF-8/UTF-16 blob 存储。缺失引用、散列/长度不匹配、冲突或未知编码直接报错，旧版内联源码仍受支持。

查找索引每个增量状态最多256个键，淘汰只移除查找项；IID 不复用，重新出现或状态重置时重新写入定义。这不是事件抽样，也不增大生产端 SMB、服务缓冲、字节/事件预算、超时或安全限制。

Job joining 回调中的应用工作不再被整段 ScopedDeferTaskPosting 包围；提交延迟仅保护返回侧 bookkeeping/worker_lock_ 区域，外部既有延迟范围、Job 生命周期与采集判据保持原样。上述实现和本地测试不代表原站或远端验证成功。


## 锁区结束与生产者提交

JobWorkObservation 构造只保存标识，先于自有 ScopedDeferTaskPosting 构造，使观察析构发生在锁和自有延迟发布作用域退出之后。ThreadGroup 命令执行器在 AssertNoLockHeldOnCurrentThread 通过后显式释放自有延迟发布作用域，再执行锁外 Flush 和任务源成员清理。调用者已有的外层延迟作用域保持不变；锁内任务发布保护、任务图事件及统计不裁剪。

隔离重现覆盖12种启用、取消与外层作用域组合，使用固定 Chromium 延迟发布实现；压力/锁兼容类型为建模。两处真实目标完整代码生成与隔离通过均不代替 Native 链接、实际页面点击及零丢包首栈验收。缓冲区、采集预算、超时、沙箱和完整性标准保持原值。


## 标准字符串驻留与采集压力

脚本源 JSON 与因果事件的 schema、protocol、storageProtocol、kind、producerId、taskKind、reason 共用 Perfetto field29 的同一驻留索引；每个事件和字段仍然输出，dataJson 保持原路径。256项查找缓存使用 LRU，仅淘汰查找项，IID 单调且不复用；增量状态及序列由标准 Perfetto 管理。脚本 UTF16 转义、源字节长度与哈希协议保持不变。

编译与固定转换器的隔离测试不代表实际网页零丢包。固定转换器对任意含 NUL 的普通注解仍有既存截断边界，内联与驻留路径一致；源 JSON 会转义 NUL，HTTP 字节验收保持原标准。锁区退出修复保留；缓冲、采集预算、超时和完整性门禁保持不变。
