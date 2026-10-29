# V8 Trace Community · 主机源码预览

作者 Alcohol；Apache-2.0；本文为 UTF-8 中文。

本仓库只公开 Community C++ 采集/索引/MCP 内核、共享界面、测试及必要依赖声明。
不包含 Professional 实现、专业版软件包、完整内部 Chromium 补丁、用户日志或凭据。

## 当前交付边界

- 本次为 **Community Host Preview**，不是已完成的社区版定制 Chromium 整包。
- 配套 Community 原生浏览器仍在独立源码拆分/验证；本仓库不提供其生成链或伪造描述文件。
- 使用本机 Chrome 的 CDP 通道可运行主机已有采集功能。原生细调用等需要配套浏览器的能力不以采样冒充。
- 保留补环境场景组合、所选日志原值、HTTP 正文、脚本、状态、基础搜索、TCP、暂停和 MCP；实际通道以运行时能力/预检为准。
- Opcode、WASM 内部指令、JSVMP 探针和高级 HTTP 根图属于 Professional；修改 UI 或 JSON 不会给社区内核增加这些实现。
- 其余手册包含完整产品的历史说明，遇专业能力以版本能力目录及以上预览边界为准。

## Windows 构建

需要 Visual Studio C++ 工具链、Windows SDK、CMake 3.24+、Ninja、Python 3 和 .NET Framework C# 编译器。
在 x64 Native Tools 开发者终端执行：

```powershell
.\build.ps1 -Edition community -BuildRoot .\.cache\cpp-community -OutputPath .\dist\V8Trace-Community -Jobs 1
```

该命令配置、编译并运行主机测试，再生成 WinForms EXE 和独立 C++ runtime。运行产物不需要 Node.js。
首次 CMake 构建下载固定哈希的第三方依赖；许可证随包复制。不要只复制 EXE，保留全部目录及 V8Trace.exe.config。

## 路径、MCP 与版本

索引目录可填普通中文长路径，不要求更改系统长路径注册表；详细说明见 runtime-cpp/MCP搜索分析说明.md。
公开主机测试不包含私有 Chromium 构建监督器/Native 插装静态检查；其注册仅在 Professional 分支生效。
这不是删除共享主机回归：其余独立可用的 C++、MCP、文件日志和编译依赖测试全部保留并执行。
SOURCE-MANIFEST.json 列明明确允许清单、源哈希和该项导出投影。

## 许可

社区共享源码使用 Apache-2.0，详见 LICENSE/NOTICE；第三方组件保留各自许可证。
Professional 私有实现与私有发行不在本仓库的授权/分发内容中。
