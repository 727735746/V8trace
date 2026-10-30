import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
from atomic_export import ExportTransaction


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def export_into(output):
    root = Path(__file__).resolve().parents[1]
    output = Path(output).absolute()
    if output.exists() or output.is_relative_to(root):
        raise RuntimeError('A fresh export outside the internal source tree is required')
    allowed = {
        'build.ps1', 'build-cpp.ps1', 'hooks.js', 'scenario_presets.json', 'case_prompt_templates.json',
        'README-cheatsheet.md', '操作手册.md', '编译指令.txt', 'UI主题与日志管理说明.md',
        'editions/runtime-identity.ps1', 'editions/verify-runtime.ps1', 'editions/verify-host-package.py',
        'editions/verify-distribution.ps1',
        'editions/export-community-host.py', 'editions/atomic_export.py', 'editions/test_atomic_export.py',
        'editions/release_state.py', 'editions/test_release_state.py',
        'editions/artifact_binding.py', 'editions/test_artifact_binding.py',
        'editions/workspace_plan.py', 'editions/test_workspace_plan.py', 'editions/artifact-tools.ps1',
        'editions/stage-native-runtime.py', 'editions/test_stage_native_runtime.py',
        'editions/产物绑定与工作区说明.md',
        'editions/发行恢复与导出说明.md',
        'editions/verify-source-boundary.py', 'editions/source-policy.json', 'editions/版本拆分说明.md',
        'editions/community/LICENSE', 'editions/community/NOTICE',
        'native/JSCall优化说明.md', 'native/v8trace-function-cache.h', 'native/v8trace-call-ledger.h',
        'runtime-cpp/CMakeLists.txt', 'runtime-cpp/CMakePresets.json',
        'runtime-cpp/cmake/edition_build.h.in', 'runtime-cpp/cmake/msvc_utf8_launcher.py',
        'runtime-cpp/licenses/sqlite.txt', 'runtime-cpp/mcp-client.example.json',
        'runtime-cpp/tests/fixtures/correlation_legacy.cpp',
    }
    sources = '''browser_import.cpp capture.cpp causality_import.cpp cdp.cpp closure.cpp community_services.cpp
configuration.cpp scenario_contract.cpp share.cpp correlate.cpp edition.cpp env_import.cpp env_options.cpp export.cpp import.cpp index.cpp
journal.cpp json.cpp main.cpp mcp_http.cpp mcp.cpp options.cpp platform.cpp recording_native.cpp recording.cpp
runtime.rc state_correlate.cpp state_import.cpp state_options.cpp store.cpp stream.cpp tcp.cpp trace_line_index.cpp
trace_search_export.cpp trace_search_internal.h trace_search_matcher.cpp trace_search.cpp trace.cpp viewer.cpp'''.split()
    headers = '''causal_queries.h causality.h core.h edition.h index.h journal.h mcp.h recording.h state.h
stream.h studio.h scenario_contract.h share.h trace_search.h vmp_probe.h'''.split()
    tests = '''edition_tests.cpp core_tests.cpp integrity_tests.cpp http_integrity_tests.cpp env_tests.cpp
closure_tests.cpp state_tests.cpp share_tests.cpp stream_tests.cpp index_tests.cpp jscall_tests.cpp catalog_tests.cpp buffer_tests.cpp
recording_tests.cpp studio_tests.cpp scenario_contract_tests.cpp trace_search_tests.cpp long_path_tests.cpp compiler_dependencies_test.py
compiler_launcher_test.py stream_kill_test.py mcp_integration_test.py file_logging_test.py integration_test.py
community_presets.py community_presets_test.py
ui_catalog_smoke.ps1 ui_mcp_smoke.ps1 ui_edition_smoke.ps1 ui_studio_smoke.ps1 ui_tcp_smoke.ps1
ui_storage_theme_tests.cs ui_storage_theme_tests.ps1 ui_theme_log_smoke.cs ui_theme_log_smoke.ps1
ui_scenario_contract_tests.cs ui_scenario_contract_tests.ps1
ui_queue_fixture.cs ui_queue_benchmark.cs ui_queue_benchmark.ps1 ui_queue_ready.py compare_ui_queue_benchmarks.py
ui_guided_workflow_tests.cs ui_guided_workflow_tests.ps1
ui_evidence_link_tests.cs ui_evidence_link_fixture.py ui_evidence_link_tests.ps1
ui_share_copy_tests.cs ui_share_copy_tests.ps1 share_path_tests.ps1'''.split()
    ui = '''AssemblyInfo.cs McpService.cs TraceWindow.Causality.cs TraceWindow.cs TraceWindow.Edition.cs
TraceWindow.Mcp.cs TraceWindow.Studio.cs TraceWindow.Cyber.cs TraceWindow.Logs.cs LogStorage.cs ThemePreferences.cs
assets/alcohol-v8trace.png assets/alcohol-v8trace.ico assets/Build-Icon.ps1
V8Trace.exe.config V8Trace.exe.manifest WorkerClient.cs'''.split()
    docs = '''README.md JSVMP与缓冲说明.md 大日志索引说明.md 原生环境取证说明.md 闭包快照说明.md
网络状态专项说明.md 持续分块说明.md VM探针与WASM模块说明.md 配置与证据索引说明.md 分享副本说明.md
HTTP首栈与参数DOM插桩说明.md 后台HTTP与常驻Job完整性说明.md MCP搜索分析说明.md trace-search来源与适配.md'''.split()
    allowed.update('runtime-cpp/src/' + name for name in sources)
    allowed.update('runtime-cpp/include/v8trace/' + name for name in headers)
    allowed.update('runtime-cpp/tests/' + name for name in tests)
    allowed.update('runtime-cpp/' + name for name in docs)
    allowed.update('ui/' + name for name in ui)
    for relative in sorted(allowed):
        source = root / relative
        if source.is_symlink() or not source.is_file() or not source.resolve().is_relative_to(root):
            raise RuntimeError('Invalid explicit export input: ' + relative)
        if source.suffix.lower() in {'.png', '.ico'}:
            magic = b'\x89PNG\r\n\x1a\n' if source.suffix.lower() == '.png' else b'\x00\x00\x01\x00'
            if not source.read_bytes().startswith(magic):
                raise RuntimeError('Invalid reviewed UI image: ' + relative)
            continue
        data = source.read_text('utf-8-sig')
        if re.search(r'github_pat_[A-Za-z0-9_]{20,}|gh[pousr]_[A-Za-z0-9]{20,}|-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----', data):
            raise RuntimeError('Credential-shaped content requires review: ' + relative)
    output.mkdir(parents=True)
    for relative in sorted(allowed):
        target = output / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(root / relative, target)
    cmake = output / 'runtime-cpp/CMakeLists.txt'
    original = cmake.read_text('utf-8')
    native_test = '  add_test(NAME build-supervisor COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/build_supervisor_test.py)'
    if original.count(native_test) != 1:
        raise RuntimeError('Native test projection no longer matches reviewed source')
    projected = original.replace(native_test, '  if(V8TRACE_BUILD_PROFESSIONAL)\n' + native_test + '\n  endif()')
    cmake.write_text(projected, encoding='utf-8', newline='\n')
    shutil.copyfile(root / 'editions/community/LICENSE', output / 'LICENSE')
    shutil.copyfile(root / 'editions/community/NOTICE', output / 'NOTICE')
    readme = '''# V8 Trace Community · 主机源码预览

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
.\\build.ps1 -Edition community -BuildRoot .\\.cache\\cpp-community -OutputPath .\\dist\\V8Trace-Community -Jobs 1
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
'''
    (output / 'README.md').write_text(readme, encoding='utf-8', newline='\n')
    (output / '.gitignore').write_text('dist/\n.cache/\nbuild/\n__pycache__/\n*.pyc\n*.pdb\n', encoding='utf-8')
    rows = [dict(path=path.relative_to(output).as_posix(), bytes=path.stat().st_size, sha256=digest(path))
            for path in sorted(output.rglob('*')) if path.is_file()]
    manifest = dict(protocol='v8trace/community-host-source/1', edition='community', license='Apache-2.0',
                    scope='host-source-preview; no Community Chromium source or binary',
                    professionalImplementationIncluded=False, fullNativeExportIncluded=False,
                    nativeVerificationComplete=False, independentBuildVerified=False,
                    transformations=[dict(path='runtime-cpp/CMakeLists.txt', inputSha256=digest(root / 'runtime-cpp/CMakeLists.txt'),
                                          outputSha256=digest(cmake), reason='Exclude private Chromium-only build-supervisor tests from host-only snapshot')],
                    files=rows)
    (output / 'SOURCE-MANIFEST.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    return manifest


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    with ExportTransaction(args.output, Path(__file__).resolve().parents[1]) as transaction:
        manifest = export_into(transaction.output)
        transaction.commit()
    print(json.dumps(dict(scope=manifest['scope'], files=len(manifest['files']), output=str(transaction.destination))))


if __name__ == '__main__':
    main()
