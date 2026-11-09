# macOS arm64 二进制发行

发行页：<https://github.com/727735746/V8trace/releases/tag/v2.2.1-macos-arm64-community>

构建机：macOS 26.4 (25E246)，Apple M4，Xcode 26.5 Build 17F42。Xcode 通过 `DEVELOPER_DIR` 显式指定，未修改全局 `xcode-select`。

## 资产

| 文件 | 内容 | SHA256 |
|---|---|---|
| `V8Trace-Community-2.2.1-macOS-arm64.zip` | Community 完整包：定制 Chromium + `v8trace-runtime` 2.0.0 + `app/v8-trace` + licenses + 验收 | `92df160095d31136a0ac4087ca1f8b5d2b16bb7aefe0949740d1c4ea8279d3fc` |
| `V8Trace-Chromium-Community-151.0.7922.72-macOS-arm64.zip` | 单独的 Community 定制 `Chromium.app` | `17b5184751a973b6dcf5c1a116909e089d39cd183711b533bd1a766586256983` |
| `V8Trace-Community-runtime-2.0.0-macOS-arm64.zip` | 单独的 Community 运行时、共享配置与许可证 | `07b6cc32c97ac9356f5d7e47cc819ff96df593d366556d7acbaee6bd4c5af9a6` |
| `V8Trace-Community-macOS-acceptance-20261109.zip` | 构建回执、四项矩阵、环境预设、CTest 与描述文件 | `ceec12bd507734bdec423fd67dfb6b82a4ed2fbcd8b63b1313c57bbc03088ef1` |
| `SHA256SUMS-Community-macOS-2.2.1.txt` | 上述 ZIP 的校验清单 | `03ee80d82f1935fc18d4ba674d05f53eb65ece3a471a560e58f5fd9f17349090` |

## 安装

```sh
ditto -x -k V8Trace-Community-2.2.1-macOS-arm64.zip .
xattr -dr com.apple.quarantine ./V8Trace-Community-2.2.1-macOS-arm64/chromium/Chromium.app
xattr -dr com.apple.quarantine ./V8Trace-Community-2.2.1-macOS-arm64/runtime/v8trace-runtime
./V8Trace-Community-2.2.1-macOS-arm64/chromium/Chromium.app/Contents/MacOS/Chromium --version
./V8Trace-Community-2.2.1-macOS-arm64/runtime/v8trace-runtime --version
```

最低系统：Chromium 为 macOS 13.0，运行时为 macOS 12.0；仅 arm64。

只使用单独浏览器时：

```sh
ditto -x -k V8Trace-Chromium-Community-151.0.7922.72-macOS-arm64.zip .
xattr -dr com.apple.quarantine ./Chromium.app
./Chromium.app/Contents/MacOS/Chromium --version
```

## 构建身份

```text
Chromium             151.0.7922.72
Chromium revision    2903d8558c752b5a554a1a47b4ea7219ba1a31ef
V8 revision          792d9716fea48312ad7ce4413c538e00628b1d50
buildAttemptId       macos-community-20261109T050224Z
validationEvidence   9092a5cf8b7b9b1baa33e7b5145e36eb96a7794f5aa9aa0144d957eb721765e1
Chromium executable  dcd037c6ab85ae05a56282ac40fe8d0e54cb7e395ac3f6f8e2aae2496b7159cb
Chromium Framework   3c532e2662fb3a7ece3961210b4cef272ac4f92a9945509b3a0161855eb24826
runtime              9d0a921c7770517a5c46ee7e86b51f239c82bcaff6c7e1f35afd63ef5f89f1b8
descriptor           3e0c6dfd7f5b9746e721f3620732e5bb042661f87e1d929b8f5487d1b517b17f
```

## 验收

- Community 定制 Chromium 四项原生夹具矩阵：4/4 PASS
- Community 环境预设：`env-protocol`、`env-target-detail` PASS，关键缺口 0
- Community 运行时 CTest：31/31 PASS
- 源码边界：`professionalImplementationCount=0`
- 完整包解压后浏览器与运行时实际执行并输出版本号；包内 407 个文件 SHA256 校验通过

## 边界

- 未签名、未公证；`codesign --verify --deep --strict` 不通过属于已知状态
- 只验证 `127.0.0.1` 本地夹具；任意网站、远端台架、长期稳定性未验证
- 媒体运行时播放/编码未验证：`runtimePlaybackValidated=false`、`runtimeEncodingValidated=false`
- macOS 没有 WinForms UI，只有 CLI 内核
- Community 不包含 Opcode、WASM 指令级 trace、JSVMP/VM、完整 HTTP causality、DOM trace、crypto trace

## 许可

Community 源码为 Apache-2.0；Chromium、V8、Perfetto 及第三方组件保留各自许可证，完整文本在包内 `licenses/` 与 `NOTICE`。本公开仓库与公开 Release 不包含 Professional 源码或 Professional 二进制。
