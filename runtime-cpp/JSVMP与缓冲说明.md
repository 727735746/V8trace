# JSVMP 动态读取定位与日志缓冲

本文使用 UTF-8 中文编码。

## 当前覆盖与边界

1. 已有 Opcode 是 V8 Ignition 的指令，不是网站自定义 VM 的指令集。
2. 新增 C++ `vmp-discover`，从真实 Opcode 的 `GetKeyedProperty` 入口 / 出口提取索引、读取对象与完整读取值，按脚本、函数起点及指令偏移汇总候选。可指定某个候选，只输出该点的所有已记录读取。
3. 普通数组、操作数、常量池和 handler 表读取也可能成为候选。分析器不猜 opcode 编号、不把每次读取认定为一条虚拟指令、不自动重写网站 JS。
4. `vmp-discover` 本身仍是**采集后的候选分析**，不自动激活点位。另有已确认描述驱动的原生 VM 状态探针，详见《VM探针与WASM模块说明.md》；核对源码/字节码双 SHA-256、分发循环和存储布局后才启用，不把候选直接冒充虚拟指令。
5. 字符串 `charCodeAt`、解密后临时计算的 opcode、WASM 自定义解释器等不一定产生受支持的数组读取。候选为空不是“无 JSVMP”的证明。没有还原完整算法或交付业务复现的自动声明。

## 使用

先在 UI 选择 Opcode 并保留原始 trace，设置实际需要的时间和日志预算。Opcode 本身有明显开销，建议先对目标脚本和较短业务阶段取证。此次分析器不降低此前采集过程的全量 Opcode 成本。

在发行目录运行：

```powershell
.\runtime\v8trace-runtime.exe vmp-discover `
  --trace .\logs\capture-001\native-output\trace.json `
  --output .\analysis\vm-001
```

CDP 原始 trace 的名称可能是 `native-output/cdp-trace.json`。`--trace` 指向包含 `traceEvents` 的原始 Chrome trace JSON，不是 events 下的归一化 JSONL。输出目录必须是新目录。

查看 `candidates.json` 中的来源、读取次数及数值特征，结合解释器源码确认目标点，再对**同一份 trace**选择候选：

```powershell
.\runtime\v8trace-runtime.exe vmp-discover `
  --trace .\logs\capture-001\native-output\trace.json `
  --output .\analysis\vm-dispatch-only `
  --site CANDIDATE_SHA256
```

把 `CANDIDATE_SHA256` 替换为候选的 64 位 `siteId`。候选身份包含本次进程、isolate 和脚本编号；不要把它当成跨次运行稳定的探针地址。

产物：

- `analysis-summary.json`：原 trace SHA-256、分析状态、完整性、计数及写缓冲凭据。
- `candidates.json`：全部已观察候选及来源；读取次数排序只帮助审阅，不自动确认语义。
- `reads.jsonl`：完整的索引读取及原始入口 / 出口证据；没有匹配读取时该文件可以不存在。
- `gaps.jsonl`：源 trace 未验证 flush、序号断裂、未配对读取等缺口；没有缺口时可以不存在。

数组地址仅为当次观察身份，GC / 地址复用后没有持续对象身份保证。`sourcePosition` 是 V8 记录的源位置，不是推测的 JS 行号。读取点中的 `index` 是属性索引，只有核对解释器后才可解释成虚拟 PC。

默认最多保留 65536 个候选与 65536 条等待出口的读取。达到分析表预算会停止、保留 partial 凭据并明确报错，不静默淘汰。需要时显式增加 `--maxCandidates` / `--maxPendingReads`，上限均为 1048576。外部排序仍使用既有临时 spool；数据量大时需要足够磁盘空间，正常 / 异常结束均清理自身 decoder spool。

## 缓冲优化

新增宿主 `JournalWriter`：

- 默认 8 MiB 待写数据，连续同文件记录默认合并为最高 1 MiB 批量；短批聚合等待窗口为 5 ms。
- 有界队列同时限制待写字节和记录数，正在写盘的批次也占用额度。另有批量拼接内存和队列元数据，不把上述额度宣传成进程 RSS 上限。
- 满时等待写入完成，不丢行、不覆盖旧记录、不采样。
- 单条记录超过缓冲时，等待其他记录完成后独占额度，原样写入，不按缓冲大小裁剪内容；因此内存上限至少需要容纳这一条完整记录。
- flush 使用队列屏障；停止先排空再写成功摘要。后台写入 / flush 失败会传播到调用方，不生成“完整”成功凭据。
- `summary.json/journalWriter` 记录实际写入行数、字节数、批次数、高水位、背压次数及尾部排空状态。
- flush 是用户态流 flush，不是承诺断电持久化的 fsync。

UI“高级选项”新增“宿主日志写缓冲（MiB）”和“宿主连续日志批量（KiB）”。CLI 配置等价字段：

```json
{
  "writeBufferBytes": 8388608,
  "writeBatchBytes": 1048576
}
```

`writeBufferBytes` 支持 4 KiB 至 256 MiB；`writeBatchBytes` 支持 4 KiB 至 4 MiB 且不大于缓冲。UI 为 MiB / KiB 单位。原日志事件 / 字节预算与对象快照预算保持原规则，耗尽仍记录真实缺口。

这段缓冲位于**宿主完成值序列化之后**，不把 V8 堆对象送入后台线程，也不消除浏览器侧逐指令、对象快照或 JSON 构造成本。Perfetto 生产者缓冲、trace 最终导出与已落盘包 TCP 传输仍是不同层；此次没有接入浏览器直接 TCP 推流，也没有以环形覆盖或隐藏丢失换取速度。OTTO 白屏需要新浏览器的独立对照验收。

## 面向目标样本的下一步

提供解释器 JS 或实际 Opcode 路径后，先核对候选读取点与原始输出，再限定 dispatcher / 指令阶段的插桩位置，验证插桩前后结果一致。需要逐条还原时，要分别明确取指、操作数读取、指令结束和异常边界，避免把一条多操作数指令拆成多条虚拟指令。交接文档和测试记录继续使用 UTF-8 中文。
