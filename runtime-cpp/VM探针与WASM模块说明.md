# 已确认 VM 探针与 WASM 模块

本文为 UTF-8 中文。新增通道默认关闭，CDP 与非 CDP 均要求带 `auxProtocol=v8trace/aux/1` 的新定制内核。模块字节、VM 状态、DrumBrake 指令、V8 Opcode 是四个不同选项。

## WASM 模块字节

`wasmModules=true` 记录 API 已拷贝的 BufferSource、同步/异步编译输入、Streaming Finish 已有完整输入、NativeModule 的 wire storage。空输入/错误输入也记录；非 BufferSource 拒绝单独说明，不伪造模块。超出 API 大小限制的原始视图按原生字节观测，明确不是引擎已经消费的拷贝。

模块开关不强制 DrumBrake，也不关闭 JS/WASM JIT。动态构造的 ArrayBuffer/TypedArray、网络流输入都由内核观察，而不是用 URL 猜内容。HTTP 正文另由 HTTP 开关记录；相同 SHA-256 只证明字节一致，不凭此宣称请求因果。

- `events/wasm-module.jsonl`：所有观察/编译结果事件。同步结果有真实成功/失败与模块 ID；异步输入不冒充 Promise 成功回执。
- `catalog/wasm-modules.jsonl`：每次输入观察及 wire blob 引用。
- `catalog/wasm-module-index.jsonl`：按 SHA-256 去重的完整字节；重复观察不会丢行。
- 中止/编译停止保留已收到的前缀，并写关键缺口，不将前缀当完整模块。
- 在策略检查更早拒绝、尚未进入字节输入路径的调用，请同时选择 Native / Exception 通道；模块字节开关本身不是全部 WebAssembly API 参数通道。

## VM 两步流程

### 1. 发现和确认

先短窗口采 Opcode，运行 `vmp-discover`。候选仍是索引读取，不自动等同虚拟取指；同一读取点也可能读取操作数/常量。Opcode 在每次函数入口保留当时完整字节数组的 `functionBytecodeSha256`，并另附 `functionBytecodeIdentitySha256` / `v8trace/bytecode-identity/1` 稳定身份。固定 V8 的 EmbeddedFeedback 操作数会随运行原地更新，原始 hash 因此可能改变；两种 hash 不混用。

```powershell
.\runtime\v8trace-runtime.exe vmp-spec --source .\target.js --url https://host/target.js --output .\target.vmp.json
```

生成的是 `confirmed=false` 待确认模板。结合源码和真实字节码，确认函数起点、取指/执行结束点、活跃寄存器/参数/Context 槽之后再填写。`bytecodeSha256`、引擎 revision、完整源码的 UTF-16LE SHA-256 都必须匹配，单填一个候选编号不会激活。

新版模板采用 `bytecodeHashPolicy="embedded-feedback-zeroed-v1"`，`bytecodeSha256` 填实际 Opcode 的稳定身份字段。身份计算只在 hash 中将引擎标识的 `kEmbeddedFeedback` 字节视为零，不更改执行字节、不忽略其他操作数；所有原始 Opcode bytes/raw hash 继续保留。新策略预检要求匹配浏览器能力，回执策略必须一致。旧描述缺省仍是严格 raw hash，不会被静默迁移；热身/GC 后 raw hash 改变可能产生明确不匹配。稳定身份不证明常量池或未知 VM 的业务语义。

描述的 `entryPoint` / `exitPoints` 使用 **Ignition 指令起点（含 Wide 前缀）**，即 Opcode 的 `instructionOffset` 或 `--print-bytecode` 的 `@ offset`，不是 `dispatchOffset`。点位还需明确 `before` / `after`。不同调用开关会改变生成字节码，例如逐次 JSCall 插入入口/退出记录；应在同一编译设置下取指纹。

### 2. 原生点位采集

UI「已确认 VM 探针」中选择 UTF-8 JSON 描述并启用；源码通道必须同时开启。无需同时开启全量 Opcode。当前每次采集配置一个目标函数描述，不自动重写原样本。

支持的存储选择器：

```json
{
  "pc": {"kind":"register","index":0},
  "opcode": {"kind":"register","index":1},
  "operands": [{"kind":"register","index":2},{"kind":"parameter","index":0}],
  "stack": {"kind":"context","depth":0,"index":4},
  "registers": {"kind":"none"}
}
```

还支持 `accumulator`。`none` 仅用于明确不用的状态；PC/opcode 必须有真实存储。没有任意表达式求值，不运行 getter/Proxy。Context 不跨进 NativeContext 全局区。描述必须确认所选槽在该点确实代表该 VM 状态，不把机器临时寄存器名称猜成业务含义。

`operandCountByOpcode` 可填写已经确认的不同指令操作数数量，`opcodeNames` 可填写语义名称。原始选择器结果全部保留，另用索引说明有效操作数；未知编号保留缺口，不自猜。

`pcAdjust` 可为整数或按 `entry` / `exit` / `exception` / `function-return` 的映射。例如取指后 PC 需回退 3，而分支执行后的 PC 保留目标位置时使用 `{"entry":-3,"exit":0}`。原始 PC 不被覆盖，调整值标明来自已确认描述。

## 状态和边界

- `events/vmp.jsonl` / `catalog/vmp-probes.jsonl` 保存入口、出口、返回指令前和原生抛出时的完整所选状态。
- `catalog/vmp-instructions.jsonl` 只按实际 operation/activation/window 身份配对；`vmp-summary.json` 明确 `algorithmRestored=false`。
- 每次快照内共享值图节点；不跨调用缓存可变值。TypedArray 顶层状态保存完整原始字节和类型/字节序；共享内存声明为并发逐字节观测，不冒充原子的全状态快照。
- 默认值图深度 2、节点 256、属性 4096；TypedArray 单次 16 MiB、活跃帧 4096。UI 可调，达到预算或遇到未覆盖布局会写缺口，不静默省略。
- 原生 throw 边界没有可验证的解释器 accumulator 存储，因此此时选择 accumulator 会明确 unavailable；需要异常状态时优先选择已确认仍活跃的寄存器/Context。抛出 0 不被当作缺失。
- 源码/字节码错版、错点位、未观察到已确认点、未配对窗口均留证据。字节码缓存按存储地址/GC epoch 区分；存在断点信息时额外复核当前字节码。
- 保持解释执行仍有逐指令检查成本；不是零开销，也不是通用自动反编译器。暂停不冻结网页或恢复 JIT，不补录暂停区间。

## 已知夹具与验收

`native/test-vmp-toy.js` 覆盖固定三单元指令、变长语义操作数、分支/回跳、TypedArray 寄存器和抛出 0。旧 d8 已核对原始结果和完整 309 字节函数：入口 123，回跳指令起点 301；对应描述仅适用于该已知夹具，不用于猜测真实站点。

`native/test-wasm-modules.js` 覆盖直接、重复、异步、262186 字节模块、空输入和错误输入；浏览器另测试动态 Response 和网络流。旧 d8 结果基线不是新插装成功证明。C++ 协议回归、固定版本影子语法与真正新 Chromium 运行是分开的门槛，完整原生运行仍须通过最终发行矩阵。

原生 AccessorInfo 属性记录描述符身份及 invoked=false，不展开引擎回调内部；数组的实际 length 另有独立值。用户 getter/setter 函数身份保留，采集过程不调用它们。普通数据字段的 opaque/partial、缺失字节及并发字节观察继续作为关键缺口。
