# macOS 26.4 arm64 本机验收

范围：Community C++ 主机内核与公开源码导出；另附 Professional 定制 Chromium 验收记录；该 Professional 源码不在本仓库，二进制已通过 Release 资产发布。

## 结论

| 项目 | 状态 | 结果 |
|---|---|---|
| Community C++ 主机内核 | 已验证 | macOS 26.4 arm64 构建成功 |
| Community 源码树 CTest | 已验证 | 31/31 通过 |
| 公开导出树冷构建 | 已验证 | 30/30 通过；`build-supervisor` 私有测试不在公开导出中 |
| 公开导出树原生单元测试 | 已验证 | browser_metadata 7/7、compile_graph 8/8、isolated_compile 9/9、media_build 5/5、media_browser 2/2 |
| Community d8 环境通道 | 已验证 | PASS；98 个环境事件；getter 调用 0 |
| Community 源码边界 | 已验证 | `professionalImplementationCount=0`；`runtime-compile-source-boundary-passed` |
| Professional 定制 Chromium | 已验证 | Chromium 151.0.7922.72；六项本地原生夹具矩阵 6/6 PASS；二进制随 `v2.2.1-macos-arm64` 发行 |
| Community 定制 Chromium macOS 构建 | 未验证 | 当前 `native/build_community_chrome.py` 是 Windows x64/MSVC 入口 |
| 完整 Professional HTTP causality 浏览器通道 | 未验证 | 本机矩阵明确 `capabilities.causality=disabled` |
| 媒体播放/编码 | 未验证 | 描述文件保持 `runtimePlaybackValidated=false`、`runtimeEncodingValidated=false` |
| 代码签名/公证 | 受阻 | 本机 Chromium 为 ad-hoc/linker-signed，不能称已签名发行包 |
| 任意网站/远端台架 | 未验证 | 只用 `127.0.0.1` 本地夹具 |

## 环境

```text
macOS 26.4 (25E246), arm64
Apple M4
Xcode 26.5, Build 17F42
Apple clang 21.0.0 (clang-2100.1.1.101)
CMake 4.4.5
Chromium bundled Ninja 1.12.1
Chromium revision 2903d8558c752b5a554a1a47b4ea7219ba1a31ef
V8 revision 792d9716fea48312ad7ce4413c538e00628b1d50
Chromium version 151.0.7922.72
```

本机 Xcode 通过 `DEVELOPER_DIR=/Users/lcc/Applications/Xcode-26.5.app/Contents/Developer` 显式使用，未改全局 `xcode-select`。没有把这次结果写成 Xcode 降级验收；更换 Xcode 版本必须重新构建并重跑矩阵。

## Community 主机内核与导出

源码树 CTest：

```text
100% tests passed out of 31
```

公开导出树是 `v8trace-community-export-20261108-r4`，使用同一套固定源码生成，冷构建目录为 `v8trace-community-build-20261108-r4`。冷构建 CTest：

```text
100% tests passed out of 30
```

源码边界回执保存在 `verification/source-boundary-macos-arm64.json`，对应编译命令快照为 `verification/source-boundary-macos-arm64.commands.json`。回执字段：

```text
protocol v8trace/edition-source-boundary/1
edition community
status runtime-compile-source-boundary-passed
professionalImplementationCount 0
nativeSourceExportValidated false
publicSourceReleaseEligible false
```

`SOURCE-MANIFEST.json` 当前仍标记：

```text
professionalImplementationIncluded=false
nativeVerificationComplete=false
publicSourceReleaseEligible=false
independentBuildVerified=false
```

这表示 Community 主机内核已经验收，但 Community 定制浏览器整包没有被标记为稳定发行。公开树不含 `dist/` 二进制、Chromium 二进制、证书、令牌或构建缓存。

## d8 与原生单元测试

Community d8 环境通道：

```text
conclusion PASS
env events 98
inventories 1
getterInvocations 0
```

公开导出树实际执行的单元测试：

```text
test_browser_metadata.py  7 tests OK
test_compile_graph.py     8 tests OK
test_isolated_compile.py  9 tests OK
test_media_build.py       5 tests OK
test_media_browser.py     2 tests OK
```

d8 结果不是 Chromium 页面验收；d8 的 trace-flush 缺口仍在，测试显式记录 `native-trace-flush-not-verified`。

## Professional 定制 Chromium（二进制发行，源码不随仓库）

本机额外构建了 Professional `Chromium 151.0.7922.72`，用于验证原生浏览器通道。Professional 源码不在本公开仓库；二进制通过 [macOS arm64 发行](https://github.com/727735746/V8trace/releases/tag/v2.2.1-macos-arm64) 提供：

| 文件/描述 | SHA256 |
|---|---|
| `Contents/MacOS/Chromium` | `dcd037c6ab85ae05a56282ac40fe8d0e54cb7e395ac3f6f8e2aae2496b7159cb` |
| `Chromium Framework` | `80b8158d1aa97fdc05d36440741214e720979bdbdf5e69455e7e916bfc4a4895` |
| `v8trace-opcode.json` | `1c13ba8c12bb44bedb1fa23d51f5adefa12b8e7f2fd2f0682665a6c0242d199d` |

六项本地原生夹具矩阵全部 PASS：

| 模式 | 关键结果 |
|---|---|
| `env` | 1050 个 env 事件；页面/iframe/Worker 语言 `en-US,en`；getterCalls=1；proxyCalls=0 |
| `closure` | 20 个 jscall；状态 `5 / 32`；异常 `closure-throw-33` |
| `wasm-module` | 20 个 wasm-module；六种加载路径均返回 42 |
| `state` | cookie 17、storage 5、indexeddb 80、websocket 211；IndexedDB 与 WebSocket Blob 各 100123 字节 |
| `vmp-opcode` | 3955 个 opcode；算术寄存器 `[7,10,0,0]` |
| `vmp` | 24 snapshots、12 entries、12 paired boundaries；typed registers 包含 `[7,10,0,0]`、`[0,0,3,0]` |

该矩阵的 Professional HTTP 记录 `producerEditions` 全部为 `[null]`，不再出现 Community 标记。矩阵只覆盖 `127.0.0.1` 本地夹具，不访问任意网站。

代码签名检查失败：

```text
code has no resources but signature indicates they must be present
```

`codesign -dv` 显示 `flags=0x20002(adhoc,linker-signed)`、`Signature=adhoc`、`TeamIdentifier=not set`。因此它不是已签名、已公证或 Gatekeeper 可分发安装包。

## 发行资产

二进制发行：<https://github.com/727735746/V8trace/releases/tag/v2.2.1-macos-arm64>

| 资产 | SHA256 |
|---|---|
| `V8Trace-2.2.1-macOS-arm64.zip` | `d53d135b8aa18699cbeb2fba3abc4b9d74595357c57364cf5d81e859e8bf93cb` |
| `V8Trace-Chromium-151.0.7922.72-macOS-arm64.zip` | `876213d0e34c02050c84a4a3c5b33589ed9c37f6737dbdefb6cd44dd2708b9a0` |
| `V8Trace-Community-2.2.1-macOS-arm64.zip` | `fed828c99a694f1723046e665d3f4cbe2efa1cba877c10d5350c01ed0c1f494c` |
| `V8Trace-d8-15.1.206.10-macOS-arm64.zip` | `1cb4ab74c556c2dfc0972530fd53e7988b00e4dda4dc6f1bbee8b1a2d91f416d` |
| `V8Trace-macOS-acceptance-20261108.zip` | `e4ed8a44cfdb30d73ae4f75e342c1b9c3249b6107ec3d2a60a44a7454c228990` |
| `SHA256SUMS-macOS-2.2.1.txt` | `457512aa3e091ecf0f0892931febb70118421faa2db2cad868e36ddbe6a21909` |

解压、移除隔离属性与运行命令见 [macOS-RELEASE.md](macOS-RELEASE.md)。Professional 二进制为专有软件，不适用仓库根目录 Apache-2.0；公开仓库只含 Community 源码。

## 未验证或受阻

- Community 定制 Chromium 在 macOS 上未构建；公开仓库没有 Community Chromium 的 macOS 运行收据。
- 完整 Professional HTTP causality 浏览器通道未执行；矩阵能力字段为 `causality=disabled`。
- 媒体运行播放/编码未验证；媒体构建选项与实际播放分开记录。
- 代码签名、公证、Gatekeeper 首次启动和跨机器分发未验证。
- macOS GUI 未验证；WinForms UI 仍仅 Windows，macOS 只有 CLI 内核。
- 本机通过不等于远端通过，也不等于任意站点或长期稳定性通过。

## 复现：Community 主机内核

```sh
cmake -S runtime-cpp -B out/macos-arm64-community -G Ninja   -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64   -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 -DV8TRACE_EDITION=community
cmake --build out/macos-arm64-community --parallel 6
ctest --test-dir out/macos-arm64-community --output-on-failure
```

公开导出树需先由 `editions/export-community.py` 生成，再以相同 CMake 参数构建；本轮导出树冷构建结果为 30/30。

## 边界

本文件记录本机源代码和主机运行验收。Professional 定制 Chromium 的源码不进入本公开仓库，二进制只通过 Release 资产提供；`SOURCE-MANIFEST.json` 的 `publicSourceReleaseEligible=false` 仍有效。`dist/` 二进制未进入源码仓库。
