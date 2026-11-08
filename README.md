# V8 Trace Community

作者 Alcohol；Apache-2.0；本文为 UTF-8 中文。

社区版公开 C++ 采集/日志/索引/MCP、共享 WinForms 界面，以及社区专用定制 Chromium 的完整源码差异和独立构建工具。运行不需要 Node.js。

媒体构建包含 H.264/AAC、Chrome FFmpeg、OpenH264 软件编码及 VP8/VP9。编译选项与采集时实际播放/编码分开验收，DRM 和硬件专属能力按实测单列。

当前为社区源码候选。Community 主机内核已在 macOS 26.4 arm64 通过构建与测试；Community 定制 Chromium 的 macOS 构建尚未完成。`SOURCE-MANIFEST.json` 仍显式标记 `publicSourceReleaseEligible=false`、`nativeVerificationComplete=false`，不把主机源码候选写成稳定整包发行。

## macOS 本机验收（2026-11-08）

macOS 26.4 arm64 的本机验收范围：

- Community C++ 主机内核：源码树 CTest 31/31 通过；公开导出树冷构建 30/30 通过（`build-supervisor` 私有测试不在公开导出中）。
- Community 主机 d8 环境通道：PASS，98 个环境事件，getter 调用 0。
- 公开导出源码边界：`professionalImplementationCount=0`，状态 `runtime-compile-source-boundary-passed`。
- 本机附加验收，不随本仓库发行：Professional 定制 Chromium `151.0.7922.72` 的六项本地原生夹具矩阵 6/6 PASS。Chromium 可执行文件 SHA256 `dcd037c6ab85ae05a56282ac40fe8d0e54cb7e395ac3f6f8e2aae2496b7159cb`，Chromium Framework SHA256 `80b8158d1aa97fdc05d36440741214e720979bdbdf5e69455e7e916bfc4a4895`，描述文件 SHA256 `1c13ba8c12bb44bedb1fa23d51f5adefa12b8e7f2fd2f0682665a6c0242d199d`。
- 该 Professional Chromium 未签名、未公证，只是本机定制构建；源码和二进制都不在本公开仓库。
- ARM64 Community 定制 Chromium 仍未构建；现有 `native/build_community_chrome.py` 是 Windows x64/MSVC 入口，macOS 不能直接复用。

详细计数、边界与未验证项见 [macOS-ACCEPTANCE.md](macOS-ACCEPTANCE.md)。系统 Chrome CDP 结果不能替代原生 JSCall/Native/WASM/Opcode 通道；macOS 也没有 WinForms UI，只有 CLI 内核。

## 功能边界

保留两种补环境组合、环境 Hook/探针、HTTP 完整正文、脚本、Console/异常、所选 JSCall/Native 完整值、对象/闭包及共享状态证据、基础搜索/MCP、TCP 和录制暂停/继续。实际开启范围由采集配置与能力预检明确记录。

Opcode、WASM 内部指令、JSVMP 与高级 HTTP 因果生产/分析实现属于独立 Professional，未进入此公开源码；共享日志读取协议名称不代表开放这些生产者。专业源码、专业软件包、用户采集数据和凭据不在本快照中。

## 编译软件与配套浏览器

先按 native/README.md 准备固定版本的 Chromium/V8/Perfetto 源码并编译社区浏览器，再在 x64 开发者终端执行：

```powershell
python -B .\editions\stage-native-runtime.py --source G:\chromium-community\src\out\V8TraceCommunity --output F:\native-runtime-community --edition community
.\build.ps1 -Edition community -NativeChromePath F:\native-runtime-community\chrome.exe -BuildRoot .\.cache\community -OutputPath .\dist\V8Trace-Community -Jobs 1
```

当前产物面向 Windows x64。请完整解压运行 V8Trace.exe，保留 runtime、app、chromium、配置及许可证目录，不要单独复制 EXE。

## 验证与许可

SOURCE-MANIFEST.json 记录明确导出清单、源码哈希与测试适配。native/community-source.json 固定原生源码版本及补丁/文件哈希。编译成功与原生运行验收分开记录；临时源码或单独的主机预览不算已通过完整包验收。

社区共享源码使用 Apache-2.0；Chromium、V8、Perfetto 和其他第三方组件保留各自许可证及声明。其余功能手册中的专业能力以当前版本能力目录为准。
