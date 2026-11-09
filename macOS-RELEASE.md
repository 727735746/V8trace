# macOS arm64 二进制发行

发行页：<https://github.com/727735746/V8trace/releases/tag/v2.2.1-macos-arm64>
发布日期：2026-11-09

构建机：macOS 26.4 (25E246)，Apple M4，Xcode 26.5 Build 17F42。Xcode 通过 `DEVELOPER_DIR` 显式指定，未修改全局 `xcode-select`。

## 资产

| 文件 | 内容 | SHA256 |
|---|---|---|
| `V8Trace-2.2.1-macOS-arm64.zip` | Professional 完整包：定制 Chromium + `v8trace-runtime` 2.0.0 + `app/v8-trace` + licenses | `d53d135b8aa18699cbeb2fba3abc4b9d74595357c57364cf5d81e859e8bf93cb` |
| `V8Trace-Chromium-151.0.7922.72-macOS-arm64.zip` | 单独的定制 `Chromium.app` | `876213d0e34c02050c84a4a3c5b33589ed9c37f6737dbdefb6cd44dd2708b9a0` |
| `V8Trace-Community-2.2.1-macOS-arm64.zip` | Community 运行时，不含定制 Chromium | `fed828c99a694f1723046e665d3f4cbe2efa1cba877c10d5350c01ed0c1f494c` |
| `V8Trace-d8-15.1.206.10-macOS-arm64.zip` | V8 shell `d8` | `1cb4ab74c556c2dfc0972530fd53e7988b00e4dda4dc6f1bbee8b1a2d91f416d` |
| `V8Trace-macOS-acceptance-20261108.zip` | 验收证据：浏览器矩阵、d8 通道、CTest、源码边界回执 | `e4ed8a44cfdb30d73ae4f75e342c1b9c3249b6107ec3d2a60a44a7454c228990` |
| `SHA256SUMS-macOS-2.2.1.txt` | 上述 ZIP 的校验清单 | `457512aa3e091ecf0f0892931febb70118421faa2db2cad868e36ddbe6a21909` |

## 安装

```sh
ditto -x -k V8Trace-2.2.1-macOS-arm64.zip .
xattr -dr com.apple.quarantine ./V8Trace-2.2.1-macOS-arm64/chromium/Chromium.app
xattr -dr com.apple.quarantine ./V8Trace-2.2.1-macOS-arm64/runtime/v8trace-runtime
./V8Trace-2.2.1-macOS-arm64/chromium/Chromium.app/Contents/MacOS/Chromium --version
./V8Trace-2.2.1-macOS-arm64/runtime/v8trace-runtime --version
open ./V8Trace-2.2.1-macOS-arm64/chromium/Chromium.app
```

最低系统：Chromium 与 d8 为 macOS 13.0，运行时为 macOS 12.0；仅 arm64。

## 构建身份

```text
Chromium             151.0.7922.72
Chromium revision    2903d8558c752b5a554a1a47b4ea7219ba1a31ef
V8 revision          792d9716fea48312ad7ce4413c538e00628b1d50
buildAttemptId       macos-20261108T175427Z
validationEvidence   368c39f52d5f4b6e59812d739f82d126d1ec2ab32fb9bbca277013eeae1eb11b
Chromium executable  dcd037c6ab85ae05a56282ac40fe8d0e54cb7e395ac3f6f8e2aae2496b7159cb
Chromium Framework   80b8158d1aa97fdc05d36440741214e720979bdbdf5e69455e7e916bfc4a4895
runtime Professional fd935230a163250f63209c851edc28c4bbaa4279d787050f826421e6e47f1ec3
runtime Community    9d0a921c7770517a5c46ee7e86b51f239c82bcaff6c7e1f35afd63ef5f89f1b8
```

## 验收

- Professional 定制 Chromium 六项本地原生夹具矩阵：6/6 PASS
- Professional 运行时 CTest：42/42 PASS
- Community 运行时源码树 CTest：31/31 PASS
- 公开导出树冷构建 CTest：30/30 PASS
- d8 Professional 四通道：PASS
- Community 源码边界：`professionalImplementationCount=0`

五个 ZIP 均做过解压回读：浏览器、两个运行时和 d8 实际执行输出版本号；上传后 GitHub 服务端摘要与本地 SHA256 一致，浏览器包下载回读哈希一致。

## 边界

- 未签名、未公证；`codesign --verify --deep --strict` 报 `code has no resources but signature indicates they must be present`
- 完整 Professional HTTP causality 浏览器通道：`causality=disabled`
- 媒体运行时播放/编码未验证：`runtimePlaybackValidated=false`、`runtimeEncodingValidated=false`
- 仅验证 `127.0.0.1` 本地夹具；任意网站、远端台架、长期稳定性未验证
- macOS 没有 WinForms UI，只有 CLI 内核
- Community 定制 Chromium 的 macOS 构建未完成；Community 包不含浏览器
- d8 仍保留 `native-trace-flush-not-verified` 缺口

## 许可

- Chromium / V8 / Perfetto 及第三方组件沿用各自许可证，完整文本在包内 `licenses/` 与 `NOTICE`
- Professional 运行时与定制 Chromium 二进制为专有软件，仅按作者发布的二进制条款使用，不适用仓库根目录的 Apache-2.0
- 本公开仓库只包含 Community 源码，不包含 Professional 源码
