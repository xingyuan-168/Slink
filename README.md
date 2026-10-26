# GameWindowMapper 屏幕墙

供 32 位易语言调用的 Windows 原生窗口墙 DLL。按传入 HWND 数组的原始顺序，裁剪客户区并显示到可缩放网格。

本版按 2026-10-13 确认的回退方案、于 2026-10-26 接续完成：撤回虚拟屏隐藏功能，映射和自动释放恢复到 `0c1659d`，保留 GPU 拉伸铺满。当前版本没有驱动安装、隐藏菜单或恢复辅助进程；实际验证与历史试验结果见测试报告。

## 使用与交付

正式交付只有 `output/GameWindowMapper.dll` 和 `output/使用说明.md`。唯一命令是 `WM_StartMapping`（易语言命名“开始映射”），包含全部启动配置；运行期间不更新。关闭画布自动释放映射资源，无需停止命令；改变参数时关闭后重新启动。

新版采用 14 参数声明，不能沿用旧版 12 参数声明。[使用说明](docs/E_LANGUAGE.md) 包含完整声明、默认参数、易语言示例、直接传PNG/BMP/JPEG底图、可选配置字节集格式及返回码。LOGO 内嵌于 DLL。开发源码、测试 EXE、ICO、头文件和测试报告留在仓库，不复制到交付目录。

## 技术与开发验证

C++20 / MSVC / CMake / x86 / 静态 CRT；WGC + D3D11、D2D/DirectWrite、WIC。Windows 10 1903+ / Windows 11。源 HWND 可以来自 x86/x64 应用。

```powershell
./scripts/build.ps1
./scripts/package.ps1 -SkipBuild
# 仅在开发时：运行单入口/关闭释放与 GPU 铺满两项短验证
./scripts/build.ps1 -Test
```

构建中间产物位于 `out/`，不下载或构建虚拟屏组件。打包仅覆盖 `output/` 的两份正式文件并拒绝链接目标，保留用户新增的 .e、图片和说明。此前整理到 `examples/demo.e`、`examples/用户测试.e`、`examples/黑.jpg` 的资产保持原样。

实际验证和限制见 [测试报告](docs/TEST_REPORT.md)。易语言和其他目标窗口由用户验收；本次不追加长时间压力测试。需求优先级为用户最新确认 > input 修订版 > 历史对话。input 只读。

- [接口约定](docs/API_SPEC.md)
- [架构](docs/ARCHITECTURE.md)
- [范围](docs/SCOPE.md)
- [批准的原型](docs/design/PROTOTYPE.html)
