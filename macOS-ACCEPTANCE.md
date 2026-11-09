# macOS arm64 验收

验收范围：Community C++ 主机内核、公开源码导出，以及 Community 定制 Chromium 的 macOS arm64 构建、打包与原生通道。Professional 源码和二进制不在本仓库，也不在本验收范围内。

## 验收结论

| 项目 | 状态 | 结果 |
|---|---|---|
| Community C++ 主机内核 | 已验证 | macOS 26.4 arm64 构建成功 |
| Community 源码树 CTest | 已验证 | 31/31 通过 |
| 公开导出树冷构建 | 已验证 | 30/30 通过；`build-supervisor` 私有测试不在公开导出中 |
| 公开导出树原生单元测试 | 已验证 | browser_metadata 7/7、compile_graph 8/8、isolated_compile 9/9、media_build 5/5、media_browser 2/2 |
| Community 源码边界 | 已验证 | `professionalImplementationCount=0`；`runtime-compile-source-boundary-passed` |
| Community 定制 Chromium macOS 构建 | 已验证 | `151.0.7922.72`；`.app/Contents/Frameworks` 完整；身份描述已生成 |
| Community 原生夹具矩阵 | 已验证 | `env`、`closure`、`wasm-module`、`state` 四项 4/4 PASS |
| Community 环境预设 | 已验证 | `env-protocol`、`env-target-detail` PASS；关键缺口 0 |
| Community 定制 Chromium 打包 | 已验证 | 解压后浏览器与运行时实际执行并输出版本号；包内 407 个文件校验通过 |
| 媒体播放/编码 | 未验证 | `runtimePlaybackValidated=false`、`runtimeEncodingValidated=false` |
| 代码签名/公证 | 受阻 | Chromium 为 ad-hoc/linker-signed，不是 Gatekeeper 可分发安装包 |
| 任意网站/远端台架 | 未验证 | 只使用 `127.0.0.1` 本地夹具 |

## 验收环境

```text
macOS 26.4 (25E246), arm64
Apple M4
Xcode 26.5, Build 17F42
Chromium 151.0.7922.72
Chromium revision 2903d8558c752b5a554a1a47b4ea7219ba1a31ef
V8 revision 792d9716fea48312ad7ce4413c538e00628b1d50
```

Xcode 通过 `DEVELOPER_DIR` 显式指定，未修改全局 `xcode-select`。更换 Xcode 或 Chromium revision 必须重新构建并重跑矩阵，旧结果不能复用。

## Community 主机内核与源码导出

源码树 CTest：

```text
100% tests passed out of 31
```

公开导出树由同一套固定源码生成，冷构建在独立目录完成，CTest 30/30 通过。源码边界回执保存在 `verification/source-boundary-macos-arm64.json`，关键字段：

```text
protocol v8trace/edition-source-boundary/1
edition community
status runtime-compile-source-boundary-passed
professionalImplementationCount 0
```

公开树不含 `dist/` 二进制、Chromium 二进制、证书、令牌或构建缓存。

## Community 定制 Chromium

构建命令：

```sh
cp native/args-macos-arm64.gn out/macos-arm64-community/args.gn
third_party/ninja/ninja -C out/macos-arm64-community chrome -j 6
```

构建结果：

```text
57137/57137
exit 0
Chromium 151.0.7922.72
```

打包命令：

```sh
python3 -B native/package_macos_community.py   --app out/macos-arm64-community/Chromium.app   --checkout .   --out-dir out/macos-arm64-community   --state chrome-build-state.json   --evidence package-evidence   --validated-report matrix-evidence/result.json
```

构建身份：

```text
buildAttemptId       macos-community-20261109T050224Z
Chromium executable  dcd037c6ab85ae05a56282ac40fe8d0e54cb7e395ac3f6f8e2aae2496b7159cb
Chromium Framework   3c532e2662fb3a7ece3961210b4cef272ac4f92a9945509b3a0161855eb24826
runtime              9d0a921c7770517a5c46ee7e86b51f239c82bcaff6c7e1f35afd63ef5f89f1b8
descriptor           3e0c6dfd7f5b9746e721f3620732e5bb042661f87e1d929b8f5487d1b517b17f
buildState           776ab8cff390308f609e102204728f467c546c1c3c209b2dc2611953f6794d41
validationEvidence   9092a5cf8b7b9b1baa33e7b5145e36eb96a7794f5aa9aa0144d957eb721765e1
V8 patch             a7cb7f91a94c54da39ff2691a318ca20980a1a08522449aabbbd2e214c37611f
Chromium patch       bf4e66023d39c1f6ee2cd6a035a4b3fc703e34b5c0aeadb46fcded9ab28c7f67
```

## 原生夹具矩阵

| 模式 | 关键结果 |
|---|---|
| `env` | 1050 个环境事件；页面/iframe/Worker 语言 `en-US,en`；接口、缺失、宿主变更、清单与内部排除全部通过 |
| `closure` | 20 个 JSCall；同一参数不同闭包绑定 `5 / 32`；GC 后绑定保持；异常 `closure-throw-33` |
| `wasm-module` | 20 个模块事件；直接、重复、异步、动态流式、网络流式均返回 42；空/非法模块按预期抛错 |
| `state` | WebSocket 211、IndexedDB 80、Cookie 17、Storage 5；IndexedDB 与 WebSocket Blob 各 100123 字节 |

矩阵同时执行两个 Community 环境预设：

| 预设 | 结论 | 关键缺口 |
|---|---|---|
| `env-protocol` | PASS | 0 |
| `env-target-detail` | PASS | 0 |

原始结果、进程日志和分项 summary 在 `V8Trace-Community-macOS-acceptance-20261109.zip` 的 `matrix-evidence-r2-full/` 中。

## 发行资产

发行页：<https://github.com/727735746/V8trace/releases/tag/v2.2.1-macos-arm64-community>

| 资产 | 内容 |
|---|---|
| `V8Trace-Community-2.2.1-macOS-arm64.zip` | Community 完整包：定制 Chromium + runtime + app + licenses + 验收 |
| `V8Trace-Chromium-Community-151.0.7922.72-macOS-arm64.zip` | 单独的 Community 定制 `Chromium.app` |
| `V8Trace-Community-runtime-2.0.0-macOS-arm64.zip` | 单独的 Community 运行时与共享配置 |
| `V8Trace-Community-macOS-acceptance-20261109.zip` | 构建、矩阵、CTest 与源码边界证据 |
| `SHA256SUMS-Community-macOS-2.2.1.txt` | 上述 ZIP 的 SHA256 清单 |

解压、移除隔离属性与运行命令见 [macOS-RELEASE.md](macOS-RELEASE.md)。

## 未覆盖项

- 只使用 `127.0.0.1` 本地夹具；任意网站、远端台架和长期稳定性未验证。
- 媒体运行播放/编码未验证；媒体构建选项与实际播放分开记录。
- 代码签名、公证、Gatekeeper 首次启动和跨机器分发未验证。
- macOS 没有 WinForms UI，只有 CLI 内核。
- Community 不包含 Opcode、WASM 指令级 trace、JSVMP/VM、完整 HTTP causality、DOM trace、crypto trace。

## 复现：Community 主机内核

```sh
cmake -S runtime-cpp -B out/macos-arm64-community -G Ninja   -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64   -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0   -DV8TRACE_EDITION=community
cmake --build out/macos-arm64-community --parallel 6
ctest --test-dir out/macos-arm64-community --output-on-failure
```

公开导出树需先由 `editions/export-community.py` 生成，再以相同 CMake 参数构建；本轮的导出树冷构建结果为 30/30。

## 边界

本文件只记录 Community 源码、Community 运行时与 Community 定制 Chromium。`dist/` 二进制未进入源码仓库，二进制只通过 Release 资产提供。
