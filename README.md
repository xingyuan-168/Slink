# CQ_SLink 屏幕墙

供 32 位易语言调用的 Windows 原生多窗口映射 DLL。

需求源：`input/slink_dll_codex_spec_v3.md`，以本次确认的调整为准：DWM 优先、12 路主验收、LOGO 仅为窗口图标、外部游戏及截图脚本不纳入本项目联调，DWM 连续性以自测验收而非任意内容的自动冻结检测保证。

## 当前阶段

UI 已由用户批准，x86 DWM/WGC 技术验证程序已编译。当前停在离屏隐藏兼容性门禁：本机两种后端均出现离屏画面冻结，失败后已自动恢复。正式 DLL 尚未交付；请勿将 HTML 原型视为真实 DWM 映射。

- [交互原型](docs/design/PROTOTYPE.html)：直接用浏览器打开，无网络依赖。
- [界面说明](docs/design/UI_SPEC.md)
- [架构与实施阶段](docs/IMPLEMENTATION_PLAN.md)
- [开源调研](docs/OPEN_SOURCE_RESEARCH.md)
- [验收清单](docs/TEST_PLAN.md)
- [实测结果与当前阻塞](docs/TEST_REPORT.md)

支持目标为 Windows 10 1903+ / Windows 11，x86；编译使用 VS 2022 C++20、Windows SDK、CMake 和静态 CRT。

## 构建和复现

在已安装 VS 2022 C++ 构建工具和 Windows SDK 的 PowerShell 中运行：

```powershell
.\scripts\build.ps1
.\out\build\x86\Release\dwm_poc.exe --backend d3d --cycles 1 --seconds 2
.\out\build\x86\Release\dwm_poc.exe --wgc --backend d3d --cycles 1 --seconds 2
.\scripts\qualify.ps1
.\scripts\qualify.ps1 -GeometryOnly
```

构建脚本优先查找 PATH 中 CMake，亦支持本机项目专用工具缓存。验证程序启动并关闭它自己的测试窗口；不操作用户现有游戏窗口。画布需保持可见，勿在其上覆盖其他置顶窗口。`qualify.ps1` 输出的 `out/qualification/*.txt` 为本机运行日志；本次证据快照随 `docs/evidence/` 提交。

`--geometry-only` 仅测位置/尺寸/样式和恢复，不证明画面连续性。完整兼容性失败返回非零退出码，不能将此测试改为默认成功。`dwm_poc` 虽保留历史名称，`--wgc` 会切换到 WGC + D3D11 验证。

