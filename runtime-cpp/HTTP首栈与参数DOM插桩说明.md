# HTTP 首栈、加密参数、payload 与 DOM 取证

状态：R7 Windows x64 本地配套候选已完成有限链与真实未决边界验收；范围、原始失败和哈希见《完整链路验收与交接-20261027》。仅在描述、二进制与实际记录一致的范围内声明支持；旧稳定包不会自动获得本页能力。

## 正确的起止位置

请求前因链是：**实际触发入口 → 同步调用与异步登记/触发 → DOM 依赖 → 加密输入/输出 → 参数与 payload 组装 → 编码/序列化 → HTTP 发出位置**。

HTTP 发出时的同步栈只是链路中的一个观察点，不替代更早的事件、Promise、定时器或 Worker 入口。采集必须早于触发动作开始；事后开启采集不会补回历史。请求发出后的回调和资源结束，使用独立的下游方向查看。

这里的“完整线程”指可证明的逻辑同步/异步任务链，不是把同一个 OS 线程上所有无关请求拼起来。上游只沿真实父任务、登记/触发关系与唯一匹配的原生 IPC 身份回溯；不按 URL、时间接近或调试 nonce 猜测。

## 开关与使用

- 一键组合：`http-root-chain`，界面名称“HTTP 首栈全链 / 同步异步插桩”。
- `httpRootTrace`：原生任务、微任务、资源、Worker、消息及 HTTP 首栈账本；默认关闭。
- `cryptoTrace`：已覆盖 WebCrypto API 的实际复制输入、规范化算法参数、调用派发、结果与异常；默认关闭，需同时开启 `httpRootTrace`。
- `domTrace`：原生 DOM 基线、查询和变更日志；默认关闭，需同时开启 `httpRootTrace`。
- JSCall、Native、Opcode、源码与 HTTP 正文保持独立通道。自定义 JS/JSVM 中的算法仍须结合调用值、字节码和源码核验，不凭名称自动宣布语义还原。

以上组合使用原生传输，没有 CDP 或页面 JS 包装兜底。新能力要求对应 Chromium descriptor 的协议、补丁、能力及二进制 SHA-256 匹配；不满足时预检明确报错。

先载入完成的采集包，在“HTTP 根链 / 插桩”建立只读磁盘索引，选择目标 HTTP 根：

1. “触发入口 → HTTP 发送”查看实际上游节点、入口及请求序号边界。
2. “参数 / payload / DOM 关键点”查看实际原生证据及文件、行号、字节偏移和哈希；支持分类和分页。
3. “HTTP → 后代结束”查看请求后的任务、资源、取消与终止。
4. 大值使用完整原始日志或精确源引用读取；界面的分页与内联预算不裁剪原始采集数据。

## 每个请求的逐函数 / 方法调试

选定 HTTP 根后点击“逐函数 / 方法插桩”，分别读取 `jscall`、`native`、`opcode`、`wasm` 和 `http` 通道。函数的每次实际执行、递归和重复调用按真实调用身份保留；参数、返回、异常和源码信息从对应原始行读取。通道未开启、过滤、预算或录制开始过晚造成的缺口不会由分析器补造。

默认显示触发入口至请求发送的证据前缀；“包含发送后记录 / 返回”只调整只读查询，便于读取请求发出后才发生的返回。下游方向仍是独立的请求后代集合。列表按因果节点和原始行分页，不是跨进程全局时间排序。只有同生产者的计数器等精确身份能够证明前后位置时才标注；其他位置显示未证实。

选中行读取原始参数，使用“下一段原始参数”继续读取大值；HTTP 通道中选择真实正文引用，再点击“读取完整 payload / 引用”。每次读取至多 64 KiB，持续读取至 `hasMore=false` 获得完整字节。页面 UTF-8 显示不改变原始字节；程序交接使用返回的 base64 和源哈希。

这里的调试指插桩日志和证据分析，不含交互式断点、单步或修改执行现场。覆盖范围是实际执行的 JS 函数和 JS 可见 Native API，不是所有 Chromium 内部 C++ 函数或未执行的静态分支。

## 不打印采集日志，直接记录文件

原生 JSCall、Native、Opcode、因果链、DOM 与加密证据直接进入 trace / JSONL 文件，不借网页 `console.log` 或终端逐条打印。浏览器进程 stdout/stderr 也重定向为 `native-output/browser-stdout.log`、`browser-stderr.log`，不转发到终端。网页自身原有 Console/异常按所选通道保存，不主动覆盖网页行为。

CLI 的 `capture`、`import` 运行状态、最终摘要和错误直接写 UTF-8 `<采集输出路径>.runtime.jsonl`，stdout/stderr 保持为空。例如输出 `./logs/case-001` 对应 `./logs/case-001.runtime.jsonl`；配置尚未读出的错误写当前路径 `v8trace-runtime.jsonl`。运行日志不提前创建采集目录，也不改写既有采集包。最终证据以采集目录中的 `summary.json` 和原始事件为准。

运行日志文件写入失败时返回退出码 `74` 并停止采集，不改为控制台打印。UI 的隐藏 worker 和 MCP stdio 仍使用专用 JSON 数据管道；这是内部协议，不是面向终端的日志。`plan`、`query` 等机器查询命令的结构化协议输出保持兼容。

## 加密与 payload 的边界

WebCrypto 的输入在算法规范化后、实际字节复制完成的位置采集，而不是把 API 入口处的可变 ArrayBuffer 当成最终被消费的数据。异步结果在原生完成点、Resolve 之前取证，并保留操作身份；不靠 Promise 地址或回调时间配对。

字节内容分块并有结束回执；核对所有 offset、字节数和完成状态后再拼接。导入密钥输入与密钥元数据有证据边界；非导出 CryptoKey 的内部密钥材料不因“记录了调用参数”就视为已经取得。

自定义 JS 加密、JSVM 字节码、对象内部槽、共享内存并发状态均按实际观测报告。关键参数与最终 payload 的完整数据依赖，还需要源码/调用身份与值的交叉核验；节点可达性本身不证明逐字节数据流。

## DOM 的边界与性能

旧浏览器的 Perfetto 共享内存提示保持每生产进程 8 MiB；原版 Chromium 的子进程共享内存序列化上限为 8 MiB，直接指定更大值会触发启动断言。新定制版本同时编译 32 MiB 的有界序列化上限，并通过实际构建回执及 `v8trace/trace-transport/1` 身份声明提供兼容门禁；只有该声明及容量匹配时，详细调用、Opcode、VM 探针和 HTTP 根链才使用 32 MiB。未知或旧声明仍为 8 MiB。高量通道页大小为 32 KiB，轻量通道为 4 KiB，轻量总量仍为 8 MiB。此配置与消费端 trace 缓冲、落盘字节预算和 TCP 写缓冲不同，不是把日志条数裁小。生产端仍保持非阻塞策略，任何实际丢包继续作为完整性缺口报告；编译与参数检查通过不等于实际采集已经零丢包。

每个 document/录制 epoch 建立一次基线，后续用原生增量；HTTP 点引用对应 document、baseline、revision，不为每个请求重新遍历整棵树。

DOM 解析记录节点身份、父子结构、原始属性/文本、查询与增删改，并区分包含于基线的变更。取证不运行 JS/getter、不强制布局、不同步 lazy style/SVG 属性。UA Shadow、CSSOM、计算样式和表单实时 property 等不等同于原始 DOM 属性；边界保持显式。

基线和增量可能来自请求祖先之外的任务。读取依赖必须使用 HTTP 点引用的精确 document/baseline/revision，而不是按时间或同 URL 拼接。

## MCP

先 `v8trace_open_capture`，再调用：

- `v8trace_causal_roots`：HTTP 根列表。
- `v8trace_causal_root`：根状态、起点与证据边界。
- `v8trace_causal_nodes`：`direction=upstream|downstream`。
- `v8trace_causal_checkpoints`：参数/DOM/HTTP 原生检查点，默认上游；`kindPrefix` 可选空、`crypto-`、`dom-`、`http-`。
- `v8trace_request_functions`：`channel=jscall|native|opcode|wasm|http`；支持 `direction`、`script`、`function` 和 `includeAfterRequest`，分页读取每次实际调用。返回行的 `rowId` 与该行 `generation` 交给 `v8trace_read_evidence`；HTTP `blobRefs` 可作为 `reference` 分块读取。

后续请求带当前 `generation` 与 `rootId`；使用响应的 `nextCursor` 分页。原始日志可通过既有文件搜索/逐行读取工具，按源引用继续读取。图索引不修改采集包，不执行日志里的代码，不修改全局 MCP 配置。

`rootId` 是当前只读图索引的临时标识，不是浏览器发出的原生身份；重建后重新读取。原生请求由进程、资源层、资源 ID 和源引用定位。图的 `generation` 与函数行的证据 `generation` 属于不同索引，读取原值时使用行内版本。

## 完整性结论

### 同步消息消费续段与常驻 Job

同步 IPC 回复到达时，旧任务可能已经执行并发出请求。回复只对消费后的执行产生影响，不追溯改写整个旧任务的父边。原生日志保留 `message-consume-point`，在实际消费点创建 `message-consume-continuation` 执行段，关联此前执行段和精确消息身份；后续函数、任务与异步登记沿新执行段取证。

重复消费以真实下一消费点结束前一续段；最后一段在所属 Native/V8/资源作用域实际退出时结束，原任务不会被提前结束。暂停、cutoff 或容量达到上限不补造结束；容量、LIFO 或上下文缺口仍明确影响完整性。公共 V8/Base Hooks ABI 与模型 2 标识不变，具体实现版本以补丁及二进制哈希为准。

原生 `job-native-origin` 记录 `sourceFile`、`sourceFunction`、`sourceLine` 和终结策略，完整内容在因果事件的 `dataJson` 中。Job 的登记/生命周期与每次实际回调执行分开：常驻工作池可能继续保留 handle，甚至在本次全部函数已返回后仍未结束。不按 Job 类型整体忽略，也不凭空闲、GC 或录制结束取消它。

- `pendingCount`：未观测结束的执行义务或资源；包括实际尚未结束的 Job。
- `startedWithoutEndCount`：已经开始、尚未记录结束的作用域。
- `localClosureMissingCount`：**已经结束但尚无本地子树闭合回执**的节点数，不包含尚未结束的 Job 本身。

三者含义不同。R7 本地双 Worker/307 对照中，消费续段均记录到真实结束且图无环，但 Chromium `cc/raster/categorized_worker_pool.cc::Start` 创建的一项栅格工作池 Job 尚无终结记录；三个目标根继续是 `completeCausality=false`、各 `pendingCount=1`。只读复核通过的是“正确保留真实未决边界”，不是完整根闭合。已开始作用域结束、运输无损和整个请求后代资源结束分别报告；不将后台服务的未决状态改写成成功。

### Promise 结算与聚合来源（模型 2）

原生结算点记录弱 Promise 身份；后续才订阅的 `then/catch`、已完成 Promise 的优化 `await`，仍关联真实结算者。`all/allSettled/any` 记录每个实际参与结果的输入回调，保留早于最后一个输入的来源；不按 URL、时间或数值相同猜测依赖。

`promise-provenance` 仅是结算/聚合身份引用，不代表等待执行的回调，故不阻塞已经结束的创建者。真正待执行的 reaction、微任务、HTTP 正文及其他资源仍计为未决。引用保留到原生弱身份/订阅释放；引用开始执行、原因未知、记录丢失或结算来源缺失均明确报告缺口，不忽略。

每个生产者在 `session-start.reason` 声明 `v8trace/causality-model/2`；采集预检同时核对浏览器描述符。旧模型日志可继续搜索和读取，但保留模型未观测的完整性边界。采集器自身明确的 Perfetto/Tracing 内部任务独立隔离，并记录策略；普通同步/异步应用工作和嵌套应用回调保持采集。

正常返回、取消、context/Worker 结束、待执行注册和录制 cutoff 分开保存。暂停、过滤、预算、丢记录、缺前序、未知/歧义消息桥、DOM/字节块缺口均会影响相应范围的完整性；空闲、停止采集或 SQLite 成功建表不是完成证明。

原生 tracing 服务如果报告 `Timed out waiting for processes to ack BeginTracing`，采集器记录关键缺口 `native-tracing-start-ack-timeout`。即使稍后 trace 成功结束，也保留启动确认缺失，整体证据仍是 partial；不通过增加等待时间把它改成完整。识别限定原生 `consumer_host.cc` 的 ERROR 日志头，不把网页 Console 同名文本或 GCM/GPU 普通错误当成该缺口。缺口引用原始文件及字节偏移；PID 列表等全部内容仍在原始日志和无损 Base64 文件中，同一输出流的该诊断只建立首个缺口，不删任何原文。

`completeCausality`、声明范围内日志完整、参数数据流已还原、纯算输出一致及远端服务端接受是不同结论。交接文档全程使用 UTF-8 中文，分别写出已验证事实、源引用、缺口及下一轮最小补采。
