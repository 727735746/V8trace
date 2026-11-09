# V8 Trace Community

作者 Alcohol；Apache-2.0；本文为 UTF-8 中文。

社区版公开 C++ 采集/日志/索引/MCP、共享配置，以及社区专用定制 Chromium 的完整源码差异和构建工具。运行不需要 Node.js。

## 当前状态

Community 定制 Chromium 已在 macOS 26.4 arm64 完成编译、打包与原生夹具验收：

- Chromium `151.0.7922.72`，Chromium revision `2903d8558c752b5a554a1a47b4ea7219ba1a31ef`，V8 revision `792d9716fea48312ad7ce4413c538e00628b1d50`。
- 原生夹具矩阵 4/4 PASS：`env`、`closure`、`wasm-module`、`state`。
- Community 环境预设 `env-protocol`、`env-target-detail` 均 PASS，关键缺口 0。
- Community 运行时 CTest：31/31 PASS。
- 公开源码边界：`professionalImplementationCount=0`；Community 二进制不含 Professional 生产者。

macOS 发行页：<https://github.com/727735746/V8trace/releases/tag/v2.2.1-macos-arm64-community>。资产、校验值与安装命令见 [macOS-RELEASE.md](macOS-RELEASE.md)，验收明细见 [macOS-ACCEPTANCE.md](macOS-ACCEPTANCE.md)。

Windows x64 构建入口保留在 `native/build_community_chrome.py`；macOS arm64 使用 `native/args-macos-arm64.gn`、GN/Ninja 与 `native/package_macos_community.py`。两种平台共用同一份 Community 补丁和源码清单。

## 功能边界

保留两种补环境组合、环境 Hook/探针、HTTP 完整正文、脚本、Console/异常、所选 JSCall/Native 完整值、对象/闭包及共享状态证据、基础搜索/MCP、TCP 和录制暂停/继续。实际开启范围由采集配置与能力预检明确记录。

Opcode、WASM 指令级 trace、JSVMP/VM 与高级 HTTP 因果生产/分析实现属于独立 Professional，未进入此公开源码；共享日志读取协议名称不代表开放这些生产者。`wasm-module` 通道只覆盖模块字节与编译输入，不是 WASM 指令级 trace。

## 编译软件与配套浏览器

macOS arm64 的完整构建步骤、依赖版本和打包命令见 [native/README.md](native/README.md#macos-arm64)。要点：

```sh
cp native/args-macos-arm64.gn out/macos-arm64-community/args.gn
ninja -C out/macos-arm64-community chrome -j 6
python3 -B native/package_macos_community.py   --app out/macos-arm64-community/Chromium.app   --checkout .   --state chrome-build-state.json   --evidence package-evidence
```

Windows x64 继续使用 PowerShell 入口；请完整解压运行，保留 runtime、app、chromium、配置及许可证目录，不要单独复制 EXE。

## 验证与许可

`SOURCE-MANIFEST.json` 记录导出清单与源码哈希；`native/community-source.json` 固定原生源码版本、补丁与文件哈希。编译成功与原生运行验收分开记录；系统 Chrome 的 CDP 结果不能替代定制 Chromium 的原生通道。

社区共享源码使用 Apache-2.0；Chromium、V8、Perfetto 和其他第三方组件保留各自许可证及声明。Professional 源码、Professional 二进制、用户采集数据和凭据不在本快照中。
