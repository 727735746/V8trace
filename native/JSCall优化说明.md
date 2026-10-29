# JSCall 分层与生产端优化

本文为 UTF-8 中文。新宿主仍读取旧 JSON 协议；新插装需要重新编译的 Chromium，旧浏览器不会被标记为已升级。

## 顺序与默认行为

1. 函数名称、脚本来源、函数定义位置和函数元数据使用字典；过滤判定缓存到函数级。同一脚本的 URL 在缓存中共享，不逐次复制。
2. 调用结构与值采集分开。`jsCallDetail=true` 启用逐次结构；`jsCallValues` 默认 `true`，保持原来的值采集语义。显式设为 `false` 才进入结构模式。
3. `V8TraceCall` 使用 Perfetto 的类型化二进制注解；不再逐次构建包含 schema、来源和函数名称的 JSON 信封。值仍完整编码，Perfetto 负责二进制缓冲、批量提交及异步传输；最终原始 trace 仍可导出 JSON。
4. 结束时输出线程级计数凭据。字典、序号、尾部凭据或记录数量缺失会成为缺口，不把有日志文件等同于完整。

没有引入每函数封顶、采样或自动关闭通道。对象内容不跨调用缓存；对象快照的原有预算与边界标记保持不变。Opcode 解释执行以及 Native、WASM 通道的原有完整值策略保持不变。

## 使用

完整采集：

```json
{"jsCallDetail":true,"jsCallValues":true}
```

仅逐次调用结构（不是 CPU 采样）：

```json
{"jsCallDetail":true,"jsCallValues":false}
```

保留已选择调用范围内的全部结构，只对某个脚本和函数记录完整值：

```json
{
  "jsCallDetail":true,
  "jsCallValues":true,
  "jsValueScriptInclude":"target*.js",
  "jsValueFunctionInclude":"sign*\nencode*"
}
```

- 调用范围继续使用 `detailScriptInclude/Exclude` 和 `jsFunctionInclude/Exclude`；不匹配的调用不会记录。
- 详情范围新增 `jsValueScriptInclude/Exclude`、`jsValueFunctionInclude/Exclude`；范围外的已选择调用仍记录结构，并标记 `valuesCaptured=false`。
- 每行一条规则，支持 `*`、`?`。脚本规则没有 `/` 时匹配去掉查询参数和片段后的文件名；有 `/` 时匹配整个 URL。函数名规则匹配完整名称。
- 黑名单优先。详情范围与调用范围取交集。详情范围留空表示对所有已选择 JS 调用采集值。
- Native 原有子类、来源脚本和名称过滤保持独立；JS 详情范围不会默默关闭 Native 参数。
- UI 的“调用过滤”页提供详情总开关与四个详情范围输入框。关闭详情后，详情规则暂不发送；重新勾选时保留原输入。
- 对象快照至少需要一个值采集通道。全局对象快照选项不会让结构模式重新读取 JS 调用的参数。

## 数据与完整性

- `v8trace/function/1`：按进程、线程、producerId、隔离实例及函数字典编号归属。SFI 稳定编号及 GC 代际用于缓存，地址戳只用于失效检测，不在后台解引用堆对象。GC 后重登记避免旧地址复用造成错误命中。
- `v8trace/call/2`：逐次 phase、callId、functionId、frameAddress、argc、producerSeq、contextId、valuesCaptured。入口的 `parentExact=false` 表示直接父帧不在可证明的采集范围内，不虚构相邻关系。
- 函数实例地址及 instanceType 每次观察，字典模板不会把不同闭包、GC 前后对象错误合并成同一对象实例。
- 生成器继续按激活分段记录 resume/suspend，详情策略在分段内保持一致。
- 异常展开后的栈帧复用先收尾再执行新调用过滤；被过滤函数的返回不会冒充前一次调用的正常返回。
- 宿主先按函数字典编号进行磁盘归并，再按生产者序号归一化。不要求原始数组按时间排序，也不把全部函数字典放入内存。
- `v8trace/call-stats/1`：检查 `attemptedCalls = selectedCalls + filteredCalls`、选中调用数与入口/恢复数、写入事件数与解码数。停止时先关闭准入，最多等待 250 ms 收齐已有写入者，仍未结束则标为非最终凭据。正常 V8 关闭也收尾；系统强杀不会冒充最终凭据。
- `jscall-writer-summary.json` 保存验收依据。原生 trace 统计中的缓冲丢失、最终 flush 失败仍独立判定；计数凭据不是对底层缓冲丢失的豁免。
- 当前宿主每次启动独立浏览器并拥有一个采集会话。并行外部跟踪会话不属于本次验收范围。

## 编译与边界

源码补丁通过 `make_patch.py --checkout <固定版本的 V8 checkout>` 生成；生产者与宿主必须成对升级。浏览器描述文件新增 callProtocol、functionDictionaryProtocol、callStatsProtocol、callValueFilterProtocol，宿主会校验二进制哈希并对分层选项检查版本。

旧 Node 宿主文件仅保留为迁移参考，不用于新紧凑协议。新宿主、归一化器和测试均由独立 C++ 程序执行。

缓存微基准只验证缓存自身，不代表网站提速。OTTO 是否恢复显示、完整详情的 CPU/内存/落盘开销，需要以新浏览器的实际对照为准；宿主测试通过不等于新 Chromium 已编译完成。
