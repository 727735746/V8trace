# MCP 搜索分析说明

编码：UTF-8。作者：Alcohol。查询和服务均在 `v8trace-runtime.exe` 的 C++ 进程执行；WinForms 仅负责软件界面、进程启停及配置复制。生产执行不需要 Rust、Node.js 或 Python。Python 仅用于仓库的协议回归测试。

## 1. 在软件中使用

1. 打开右侧 **MCP 搜索 / 分析**。
2. 选择已有日志根目录。已载入采集包时可点“当前包”。MCP 只读取此根下的文件；这不是自动开启采集。
3. 确认索引目录、导出目录、令牌文件及本地端口。默认分别为 `./mcp/index`、`./mcp/exports`、`./mcp/token.txt`、`17881`。日志根默认为 `./logs`；不存在时请先选择已有目录。
4. 点“启动 MCP”。首次启动为当前用户生成随机令牌，已有令牌不被覆盖。收到 C++ 真实监听回执后显示 `http://127.0.0.1:17881/mcp`。
5. 点“复制 HTTP 配置”，导入支持 `mcpServers` 格式的客户端；其他客户端按其格式填相同 URL 和 Authorization 请求头。界面显示并复制完整配置值。
6. “停止服务”取消待完成查询并结束此软件启动的服务；软件退出也执行停止。停止超过 10 秒时只结束自有进程，界面明确提示；原始日志保留，缓存下次核验。

也可“复制 stdio 配置”，由客户端直接启动独立 C++ 进程；此方式不依赖界面里的 HTTP 服务，也不需要网络令牌。每个进程各自维护一个活动行文件及一个活动采集包。不要把不同客户端的隐式状态混用。

界面相对路径始终以软件目录解析，不落到固定 C 盘目录。生成的客户端配置按当前位置给出绝对路径，避免客户端工作目录不同造成误读；移动软件后重新复制。软件不自动注册全局 MCP，不修改客户端配置文件。

## 2. 命令行启动

在便携软件目录运行 PowerShell；目录和源文件路径按实际位置填写。

```powershell
# 本地 stdio；日志只走 MCP 消息，诊断不混入 stdout。
.\runtime\v8trace-runtime.exe mcp --transport stdio `
  --source-root 'G:\project\trace' `
  --index-dir '.\mcp\index-stdio' --export-root '.\mcp\exports'

# HTTP；token.txt 可由 UI 的“生成令牌”创建，或自备 24～256 字节的随机 ASCII 令牌。
.\runtime\v8trace-runtime.exe mcp --transport http `
  --source-root 'G:\project\trace' `
  --index-dir '.\mcp\index' --export-root '.\mcp\exports' `
  --bind 127.0.0.1 --port 17881 --token-file '.\mcp\token.txt'
```

HTTP 所有接口都要求 `Authorization: Bearer TOKEN`；`GET /healthz` 可查看运行时、根目录、预算及请求计数。`POST /shutdown` 是 UI 使用的带认证本地管理接口。Ctrl+C 也会结束 HTTP 服务。默认回环绑定；只接受具体 IPv4 地址，不接受 `0.0.0.0`。指定其他网卡时使用相同地址的 Host / Origin；HTTP 本身不加密，跨机使用应经过受控加密通道。

可选运行参数：`--checkpoint-bytes 8388608`、`--search-threads 0`（自动，最多 8；显式最多 32）、`--query-concurrency 4`、`--max-content-bytes 16777216`。并行数还受内存规划缩减；配置值不等于进程 RSS 硬上限。

## 3. 六个行搜索工具

| 工具 | 用途 / 关键参数 |
|---|---|
| `open_file` | `path`，可选 `force_rebuild`；启动后台索引，返回状态与 `index_reused` |
| `get_file_status` | `no_file/indexing/ready/failed`、字节进度、行数、速率、ETA 或错误 |
| `close_file` | 取消当前索引并关闭活动文件，保留可复用缓存；已开始的独立查询快照正常结束或由请求取消 |
| `read_lines` | `start_line`、`line_count`；保留原换行，响应有原始字节数、`lossy_utf8`、`next_line`、`eof` |
| `search_lines` | `pattern`、`start_line`、`max_scan_lines`；可选 `max_matches`（默认 100）、`regex`、`case_sensitive` |
| `export_lines` | `start_line`、`line_count`、`output_path`、`overwrite`（默认 false）；原始字节原子导出 |

顺序示例（`tools/call` 的 `name` 与 `arguments`）：

```json
{"name":"open_file","arguments":{"path":"CASE/events/jscall.jsonl"}}
{"name":"get_file_status","arguments":{}}
{"name":"search_lines","arguments":{"pattern":"functionName","start_line":1,"max_scan_lines":100000,"max_matches":50}}
{"name":"read_lines","arguments":{"start_line":120,"line_count":8}}
{"name":"export_lines","arguments":{"start_line":120,"line_count":1000,"output_path":"selected-jscall.jsonl"}}
{"name":"close_file","arguments":{}}
```

等待 `ready` 后查询。换文件先关闭。行号从 1 开始，空文件 0 行，末尾 LF 不多算一行，EOF 游标是 `line_count+1`；零行数是有效空范围。读取和导出保留 CRLF/LF；搜索逐行匹配原始字节前去掉一个末尾 LF、再去一个 CR。搜索结果每个匹配行仅返回一次，按原行序排列。

继续查询必须使用 `next_line`。`scan_limit_reached`、`match_limit_reached`、`content_limit_reached` 分别表示本页触及的范围；不是源文件丢失。内容预算不足时不切碎匹配行，游标停在尚未返回的那一行；首个匹配行已超限则明确报错，改用原始导出或更小的目标范围。

正则执行器是 **RE2 2025-11-05**，固定 16 MiB 编译预算；不使用回溯型 `std::regex`。不接受反向引用和环视。`\w` 等字符类采用 RE2 语义，与 Rust `regex::bytes` 不完全相同；不支持的模式明确报错，不冒充方言等价。字面量忽略大小写采用 Unicode 简单大小写折叠。

## 4. v8trace 结构化证据工具

| 工具 | 用途 |
|---|---|
| `v8trace_open_capture` | `capture` 指向含 `summary.json` 的已完成采集目录；在原生线程构建只读投影索引；长调用可取消 |
| `v8trace_capture_status` | 查询结构化索引构建进度、状态及索引边界 |
| `v8trace_close_capture` | 关闭或取消结构化索引，旧查询保留自身快照 |
| `v8trace_query` | 按 `category/script/function/request/window/call/operation/activation/process/isolate/producer/session/context/url/identity` 精确筛选；`limit` 为 1～200，使用 `after=nextCursor` 翻页 |
| `v8trace_read_evidence` | 用 `rowId+generation` 或真实 blob `reference` 分段读取；`offset` 从 0 开始，单段最多 65536 字节，`base64` 是精确原值 |

```json
{"name":"v8trace_open_capture","arguments":{"capture":"CASE"}}
{"name":"v8trace_query","arguments":{"category":"jscall","script":"https://fixture.test/app.js","limit":50}}
{"name":"v8trace_read_evidence","arguments":{"rowId":"ROW_ID","generation":"GENERATION","offset":0,"limit":65536}}
```

`category` 对应保存后的日志类别，例如 `jscall`、`opcode`、`native`、`http`、`script`。过滤匹配真实元数据，不是任意全文子串；全文模糊线索先用 `search_lines`。每行返回源文件、原行号、SHA-256、`rowId`、`generation`、预览边界及 blob 引用。预览不是完整参数；通过 `v8trace_read_evidence` 按 `nextOffset` 读到 `hasMore=false`，或用行导出取得完整 JSON。

结构化行读取的字节范围是 JSON 记录本身，不含分隔 LF；行搜索的 `read_lines/export_lines` 则保留分隔符。二者均按原文件字节定位，不重新生成原始值。Blob 只接受 `blobs/sha256/HASH` 形式，核验长度和实际内容哈希。

当前共享元数据关联不等于全链路因果证明；明确返回 `completeCausality=false`、`algorithmRestored=false`。本功能没有修改 Chromium 插装生产端、补齐缺失日志或自动完成算法还原。采集结论与缺口请从 `report/summary`、`report/gaps` 等实际报告查询，再读取原值。

## 5. 默认预算、持久索引与取消

- 行索引检查点默认间隔 8 MiB，构建读缓冲 32 MiB；定位扫描 1 MiB，搜索每工作线程 4 MiB，原始导出 8 MiB 缓冲。索引及查询使用 64 位行号和偏移，不把整份日志映射或读进内存。
- MCP 的结构化 SQLite 临时索引也放在所选索引根下的自有 `analysis-cpp-*` 目录；正常关闭自动清理。不会因默认系统 TEMP 路径把这个大索引固定写入 C 盘。原有 GUI 工作台索引入口保持原来的缓存行为。
- 单次读取最多 100000 行、默认内容 16 MiB；搜索最多 10000 个匹配、默认单行 64 MiB、模式 16 KiB；导出默认最多 1 亿行。`max_scan_lines` 支持完整 uint64 范围并按实际剩余行数收敛，没有隐藏扫描行数截断。
- MCP 请求最多 1 MiB、响应最多 32 MiB。JSON 转义、文本兼容副本也占响应预算；超限返回明确错误，建议小页读取或原字节导出，不返回裁剪后的“成功”。
- 索引格式为独立 `V8TSIDX2`、小端 64 位检查点与 CRC32、带 SHA-256 校验的元数据；损坏或身份变化会重建。关闭/重启可恢复已验证的部分检查点。不复用上游 Rust 二进制索引。
- 源身份含规范路径、OS 文件身份、长度、修改时间及首尾各至多 64 KiB 样本；查询前后复核身份。它不是每次查询的整文件内容证明，同长度且恢复时间/身份的中间改写可能绕过轻量核验；请查询不可变副本。结构化索引另保存各日志全文件 SHA-256。
- stdio 用 `notifications/cancelled` 取消请求；HTTP 关闭该请求的 SSE 响应连接会取消查询并等待工作线程结束后释放许可。关闭活动文件主要取消索引，不替代取消已经开始的独立查询。
- 导出目标只在 export-root 下，父目录须已存在，禁止源文件自身或其硬链接，也拒绝路径逃逸。使用同目录私有临时文件、同步落盘和原子发布；失败清理自身临时文件。根目录、索引缓存和导出父目录应由操作者控制，未声明抵抗外部进程恶意并发替换目录的无竞态遍历保证。

## 6. MCP 版本与交付边界

支持现代 `2026-07-28` 的每请求 `_meta`、`server/discover`、HTTP 镜像头校验和 `resultType=complete`；同时支持 `2025-11-25` 的 `initialize` 客户端。HTTP 接口是请求级 Streamable HTTP：POST `/mcp`，长工具调用用请求级 SSE，未提供独立 GET 事件流或会话恢复。客户端必须声明接受 `application/json, text/event-stream`；现代请求携带 `MCP-Protocol-Version`、`Mcp-Method`，工具调用还携带 `Mcp-Name`，与请求体一致。

本轮交付为 Windows x64 主机功能候选，原稳定目录保留。C++ 源码含 POSIX 路径及文件实现，但 macOS / Linux 的实际构建验收不在本轮 Windows 结果内。候选 EXE 未签名，不导入证书信任根。

来源、接口差异、依赖固定版本与许可证声明见 [trace-search 来源与适配](trace-search来源与适配.md)。

## 7. Windows 超长索引目录

Community 与 Professional 的 C++ 索引入口将 Windows 索引目录转换成内部 Unicode 扩展路径，规范化后继续保留扩展形式；元数据临时文件、锁、检查点、同步及原子替换沿用同一实际目录。结构化证据和因果图 SQLite 索引使用 win32-longpath VFS。完整哈希和 UUID 保持原样，不迁移到隐藏的短缓存。

界面填写或 MCP --index-dir 传入普通绝对路径即可，包括中文、空格和超过 260 字符的索引根；无需手动添加前缀或修改 LongPathsEnabled。对外 indexRoot、indexOwner 与 index_path 继续显示普通路径。随包 V8Trace.exe.config 为界面启用 .NET Framework 的长路径处理，请与 EXE 一同保留。

路径适配保留根目录约束、私有构建锁、源身份校验、损坏重建、取消与断点恢复。原始日志仍只读；API 预算及文件系统的单段文件名、总路径和可用磁盘限制仍有效。此修复针对索引缓存链路，不据此声明任意源日志或导出路径的所有长度组合已验收。UNC 转换与实际网络共享是不同验证层级，交付说明会列出本轮实际范围。

实现依据：[Windows 路径长度说明](https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation)、[.NET 路径规范化开关](https://learn.microsoft.com/en-us/dotnet/framework/migration-guide/mitigation-path-normalization)。SQLite VFS 以项目锁定的实际源码与运行测试为准。
