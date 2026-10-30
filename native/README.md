# 社区版定制 Chromium 独立构建

作者 Alcohol。本文为 UTF-8 中文。此目录包含社区版专用源码差异、固定版本清单、构建监督器与浏览器身份/运行文件清单生成器，执行不依赖专业版源码或专业版补丁生成器。

## 源码边界

- `community-chromium.patch`：共享浏览器通道、网络正文、脚本/Console/异常、状态证据、录制开关、启动兼容和默认英文设置。
- `community-v8.patch`：社区 JSCall/Native、环境探针、完整选定值、对象与闭包快照、WASM 模块证据。
- `community-source.json`：锁定的 Chromium、V8、Perfetto 版本，原始/投影文件哈希和补丁哈希。
- 专业 Opcode、WASM 内部指令、JSVMP 和高级因果生产者不在社区实现中；共享读取器和协议名称不代表开放专业采集能力。

清单固定的三个仓库版本分别为：

| 仓库 | 版本 |
| --- | --- |
| Chromium | `2903d8558c752b5a554a1a47b4ea7219ba1a31ef` |
| V8 | `792d9716fea48312ad7ce4413c538e00628b1d50` |
| Perfetto | `ec4b996c753b3e301b183736f54566239b0df68d` |

先按 Chromium 自身流程准备并同步到上述固定版本的完整 Windows 源码树及依赖。使用独立社区工作树，不与正在构建的专业源码窗口共用一棵树。

## 编译

需要 Windows x64、Visual Studio C++/Windows SDK、Python 3.10+、PowerShell 7 和 Chromium depot_tools。使用已经配置好本机编译工具链的 x64 开发者终端；本机构建可设置 `DEPOT_TOOLS_WIN_TOOLCHAIN=0`，保留实际 Visual Studio 与 SDK 配置。

在社区源码根目录执行，替换示例 checkout 路径：

```powershell
python -B .\native\build_community_chrome.py --checkout G:\chromium-community\src --out-name V8TraceCommunity --evidence G:\chromium-community\build-evidence --jobs 1
```

构建入口先校验版本和已知源码，再在临时稀疏树中验证补丁并安装逐文件验证后的社区源码；遇到未知本地编辑会保留并报错。随后运行 GN、同步 Ninja 构建图，再固定链接器单线程规则并验证后续构建不会再次生成规则。Ninja 并行数默认 1。

线程参数按驱动模式区分：EXE/DLL 链接使用 LLD `/threads:1`，Rust 通过 `-Clink-arg=/threads:1` 传给链接器；静态库归档保持 `/lib` 为首个参数，不混入链接线程参数。构建工具回归同时检查这三种调用模式。

若已准备好依赖的单个 CXX 节点明确报告 LLVM 内存分配失败，入口最多对三个不同节点使用小构建图重试，再继续完整构建。原始 Clang 命令、检查、优化和插桩参数保持逐字节一致；先备份并校验 Ninja 命令/依赖日志，其余编译缓存保留，前后回执分别保存。普通编译错误、同一节点再次失败或缓存一致性异常会停止，而不是取消检查或把失败记成通过。

两种版本均使用媒体构建配置：`proprietary_codecs=true`、`ffmpeg_branding="Chrome"`、`media_use_openh264=true`、`media_use_ffmpeg=true`、`media_use_libvpx=true`。这些选项分别接入对应解码器和软件编码器；配置通过不计作实际视频播放/编码验收。媒体回归用合成 MP4/H.264/AAC 与 WebM/VP9/Opus，验证播放时钟、解码帧、AAC 解码、H.264/VP8/VP9 WebCodecs 实际编码再解码，以及 MediaRecorder 输出回放，并在日志采集开启时执行。

可用 `--stage apply`、`verify`、`configure`、`build`、`package` 分阶段执行。已有且哈希相符的社区源码可重复验证；失败日志保留在指定 evidence 目录。

## 产物和验收

成功编译后，输出目录含 `chrome.exe`、`chrome.dll`、`chrome_elf.dll`、`v8_context_snapshot.bin`；`package` 阶段依据实际编译回执、核心文件哈希、社区生产者标识及 GN `runtime_deps` 生成 `v8trace-opcode.json` 和 `v8trace-runtime-files.json`。

描述文件中的 `v8trace/opcode/1` 和 `chromium-opcode` 是既有原生传输身份格式，社区版同时明确记录 `edition=community`、`opcodeSupported=false` 和仅有的共享能力；它们不表示社区支持专业 Opcode 采集。

源码应用、GN 生成、编译与原生运行验收分开记录。构建脚本成功仅说明编译和身份生成通过；运行验收仍应覆盖实际环境/HTTP、JSCall/Native、两种补环境组合、录制暂停/继续及专业选项负例。

原生浏览器经验证后，再从软件源码根目录执行：

```powershell
.\build.ps1 -Edition community -NativeChromePath G:\chromium-community\src\out\V8TraceCommunity\chrome.exe -BuildRoot .\.cache\community -OutputPath .\dist\V8Trace-Community -Jobs 1
```

构建不要把整个 Chromium `out` 目录打入发行包；软件打包器按运行文件清单复制浏览器，不带 `obj`、PDB 或编译缓存。当前构建入口针对 Windows x64，其他平台另行原生构建及验收。

## 许可证

社区自有修改按 Apache-2.0 分发。Chromium、V8、Perfetto 等第三方组件分别遵循自身许可证，所附文本见 `licenses`；原有著作权声明保留。
