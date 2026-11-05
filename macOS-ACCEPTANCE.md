# macOS 26.4 arm64 本机验收

日期：2026-11-05。目标：Community C++ 主机内核与系统 Google Chrome 的 CDP 通道；不把系统浏览器写成定制浏览器。

## 已验证

| 项目 | 结果 |
|---|---|
| macOS 26.4 arm64 内核编译 | Community 构建成功 |
| Community 源码树 CTest | 31/31 通过 |
| 公开导出树 CTest | 30/30 通过；`build-supervisor` 私有测试不在公开导出中 |
| 公开导出树集成测试 | 6 项通过、3 项按条件跳过；系统 Chrome CDP 与 TCP/Worker 实际执行 |
| 系统 Google Chrome 157.0.8103.38 CDP 实采 | 6 项通过、3 项按条件跳过 |
| CDP 夹具覆盖 | HTTP 完整正文、Console、脚本、Env、TCP 分块/续传、Worker 退出 |

CDP 测试使用 `127.0.0.1` 本地夹具，不访问外部网站。原始运行输出中的 `test_real_chrome_cdp ... ok`、TCP 四项和 Worker 一项均为实际执行；原生 JSCall/Native/Opcode/WASM 没有执行。

## 未验证或受阻

- ARM64 定制 Chromium：未编译、未打包 `.app/Contents/Frameworks`、未生成浏览器身份描述及 `v8trace-opcode.json`。
- 原生 JSCall/Native/WASM/Opcode 与暂停/恢复通道：未执行；系统 Chrome CDP 不能替代。
- macOS GUI：未验证；WinForms UI 仍仅 Windows，macOS 只提供 CLI 内核。
- Perfetto protobuf 转换在 macOS 的可用性：当前固定件只有 Windows x64；无转换器时 `recording` 测试显式跳过两个 protobuf 模式并记录 `protobufConverter=false`。

## 定制 Chromium 阻塞

固定源码版本仍为 Chromium `2903d8558c752b5a554a1a47b4ea7219ba1a31ef`、V8 `792d9716fea48312ad7ce4413c538e00628b1d50`。本机检查：

```text
$ xcodebuild -version
xcode-select: error: tool 'xcodebuild' requires Xcode, but active developer directory '/Library/Developer/CommandLineTools' is a command line tools instance

$ xcrun --find actool
xcrun: error: unable to find utility "actool", not a developer tool or in PATH

$ xcrun --find ibtool
xcrun: error: unable to find utility "ibtool", not a developer tool or in PATH
```

网络复核结果：

```text
chromium.googlesource.com (depot_tools info/refs)  200
chrome-infra-packages.appspot.com                 200
commondatastorage.googleapis.com                  400（裸根可达）
```

现有 `native/build_community_chrome.py` 是 Windows x64/MSVC 入口，macOS 不能直接复用；需要完整 Xcode、ARM64 GN 参数和独立 `.app` 运行文件清单。网络不再是阻塞项，完整 Xcode 未安装是当前阻塞项。

## 复现

源码树构建与测试：

```sh
cmake -S runtime-cpp -B out/macos-arm64-community -G Ninja   -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64   -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 -DV8TRACE_EDITION=community
cmake --build out/macos-arm64-community --parallel 6
ctest --test-dir out/macos-arm64-community --output-on-failure
```

公开导出树需先由 `editions/export-community.py` 生成，再以相同 CMake 参数构建；本轮导出树结果为 30/30。

## 边界

本地通过不等于远端发布通过，也不等于定制浏览器/台架/任意网站通过。`dist/` 二进制未进入源码仓库；本文件只记录源代码与主机运行的验收范围。
