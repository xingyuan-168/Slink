# GameWindowMapper 屏幕墙

供 32 位易语言调用的 Windows 原生窗口墙 DLL。按传入 HWND 数组的原始顺序，裁剪客户区并显示到可缩放网格。

REQ-GWM-002 已接入正式原生画布：GPU 拉伸铺满、右键隐藏／显示、异步驱动初始化和关闭前恢复。2026-10-10 新版原型已获用户批准；大漠和易语言按用户选择在正式 DLL 上验收。最新验证状态与未测项见 [测试报告](docs/TEST_REPORT.md)。

## 使用与交付

正式交付只有 `output/GameWindowMapper.dll` 和 `output/使用说明.md`。唯一命令是 `WM_StartMapping`（易语言命名“开始映射”），保留原 14 参数与全部启动配置；运行期间不更新。右键格子隐藏／显示源窗口，关闭画布先恢复隐藏窗口再释放；恢复失败保留画布重试，无需停止命令。改变参数时关闭后重新启动。

新版采用 14 参数声明，不能沿用旧版 12 参数声明。[使用说明](docs/E_LANGUAGE.md) 包含完整声明、默认参数、易语言示例、直接传PNG/BMP/JPEG底图、可选配置字节集格式及返回码。LOGO 内嵌于 DLL。开发源码、测试 EXE、ICO、头文件和测试报告留在仓库，不复制到交付目录。

## 技术与开发验证

C++20 / MSVC / CMake / x86 / 静态 CRT；WGC + D3D11、D2D/DirectWrite、WIC。Windows 10 1903+ / Windows 11。源 HWND 可以来自 x86/x64 应用。

```powershell
./scripts/build.ps1
./scripts/package.ps1 -SkipBuild
# 仅在开发时：编译测试并运行单入口/关闭释放短验证
./scripts/build.ps1 -Test
# 显式选择：已安装项目驱动、已解锁本地桌面且没有活动映射时，短测完整集成
./scripts/build.ps1 -Test -VirtualDisplayTests
```

构建中间产物位于 `out/`；构建先核验固定驱动包并生成 x64 辅助程序，再嵌入 x86 DLL。打包只允许输出到项目 `output/`，仅覆盖两份正式文件并拒绝链接目标，不搬动或删除用户新增的文件。用户的 `demo.e` 已按此前授权整理至 `examples/demo.e`，未修改内容。后续提供的非空工程和JPEG分别保存在 `examples/用户测试.e`、`examples/黑.jpg`，同样保持原样。

实际验证和限制见 [测试报告](docs/TEST_REPORT.md)。易语言和其他目标窗口由用户验收；本次不追加长时间压力测试。需求优先级为用户最新确认 > input 修订版 > 历史对话。input 只读。

- [接口约定](docs/API_SPEC.md)
- [架构](docs/ARCHITECTURE.md)
- [范围](docs/SCOPE.md)
- [已批准交互原型](docs/design/PROTOTYPE.html)
