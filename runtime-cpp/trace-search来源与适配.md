# trace_search 来源与独立 C++ 适配

编码：UTF-8。参考项目：[AndroidReverser-Test/trace_search](https://github.com/AndroidReverser-Test/trace_search)。

- 阅读版本：`fd8f5c781c69b10fad463d094b727c79c0d24364`。
- 参考归档 SHA-256：`099cf46d46e7d864c4953bab6d3e8b7f8a105bb59f4bffafa0ed1663617c9295`。
- 实际阅读：`src/engine.rs`、`src/index.rs`、`src/config.rs`、`src/main.rs`、`src/mcp.rs`、公开测试及设计说明。未执行上游程序。
- 上游 `LICENSE` 文件是 AGPL-3.0 文本，`Cargo.toml` 的 package license 声明为 Apache-2.0，两处不一致。此处记录事实，不把其任一声明当成已解决的许可结论，也不更改本项目整体许可证。

本实现是依据架构意图、公开工具行为和测试场景进行的独立 C++ 设计，不包含或逐行翻译上游 Rust 函数体，不打包上游二进制，也不通过子进程调用 Rust / Python / Node。该设计选择不代替对后续直接引入上游代码时的许可审查。

## 行为对应

| 上游关注点 | C++ 适配 |
|---|---|
| 六个工具、单活动文件 | 保留名称和主要参数 / 分页字段；每个进程独立状态 |
| 后台稀疏行索引、CRC、部分恢复 | C++ 独立格式 `V8TSIDX2`；原子发布；取消时刷新检查点 |
| LF/CRLF、EOF、空范围、UTF-8 替换 | 移植为原生行为测试；完整 raw 导出保留原字节 |
| 有界并行搜索、按行序归并 | 检查点分块、独立句柄、有界工作线程和输出；明确限额游标 |
| Rust regex::bytes | 使用 RE2；不是全部正则方言兼容，不使用 std::regex 回溯执行 |
| HTTP MCP | C++ 实现 HTTP 与 stdio；新增现代元数据及旧初始化协议分支 |
| 任意本机源路径 | 增加显式 source-root 读取边界及 export-root 写入边界 |
| trace 文本搜索 | 新增 v8trace 既有 EvidenceViewer 的精确元数据筛选、文件哈希和原值分页 |

新索引不与上游二进制格式互读。轻量身份核验不等于全文件持续哈希认证。搜索预览、MCP 响应预算、源日志实际缺口是不同概念，均明确输出，不把限页当成源丢失，也不把日志关联说成完整线程因果链。

## 新增原生依赖

| 依赖 | 固定版本 | 归档 SHA-256 | 许可 |
|---|---|---|---|
| google/re2 | 2025-11-05 | `87f6029d2f6de8aa023654240a03ada90e876ce9a4676e258dd01ea4c26ffd67` | BSD-3-Clause，发行包 `licenses/re2.txt` |
| abseil/abseil-cpp | 20250512.1 | `9b7a064305e9fd94d124ffa6cc358592eb42b5da588fb4e07d09254aa40086db` | Apache-2.0，发行包 `licenses/abseil.txt` |

CMake 通过 HTTPS 拉取固定归档并核验 SHA-256，Windows 统一静态 CRT；未关闭证书验证。原有 JSON、IXWebSocket、压缩库及 SQLite 依赖继续保留原许可证。

MCP 行为参考：[2026-07-28 发现](https://modelcontextprotocol.io/specification/2026-07-28/server/discover)、[Streamable HTTP](https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/streamable-http)、[工具](https://modelcontextprotocol.io/specification/2026-07-28/server/tools)。仅声明实际实现的工具能力，不冒充完整 SDK 的所有可选能力。
