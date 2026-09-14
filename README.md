# GameWindowMapper 屏幕墙

供 32 位易语言调用的 Windows 原生窗口墙 DLL。按传入 HWND 数组的原始顺序，裁剪客户区并显示到可缩放网格。

## 使用与交付

正式交付只有 `output/GameWindowMapper.dll` 和 `output/使用说明.md`。唯一命令是 `WM_StartMapping`（易语言命名“开始映射”），包含全部启动配置；运行期间不更新。关闭画布自动释放映射资源，无需停止命令；改变参数时关闭后重新启动。

新版采用 14 参数声明，不能沿用旧版 12 参数声明。[使用说明](docs/E_LANGUAGE.md) 包含完整声明、默认参数、易语言示例、可选配置字节集格式及返回码。LOGO 内嵌于 DLL。开发源码、测试 EXE、ICO、头文件和测试报告留在仓库，不复制到交付目录。

## 技术与开发验证

C++20 / MSVC / CMake / x86 / 静态 CRT；WGC + D3D11、D2D/DirectWrite、WIC。Windows 10 1903+ / Windows 11。源 HWND 可以来自 x86/x64 应用。

```powershell
./scripts/build.ps1
./scripts/package.ps1 -SkipBuild
# 仅在开发时：编译测试并运行单入口/关闭释放短验证
./scripts/build.ps1 -Test
```

构建中间产物位于 `out/`；打包只允许输出到项目 `output/`，发现未知文件时拒绝覆盖或删除。用户的 `demo.e` 已按本次授权整理至 `examples/demo.e`，未修改内容。

实际验证和限制见 [测试报告](docs/TEST_REPORT.md)。易语言和其他目标窗口由用户验收；本次不追加长时间压力测试。需求优先级为用户最新确认 > input 修订版 > 历史对话。input 只读。

- [接口约定](docs/API_SPEC.md)
- [架构](docs/ARCHITECTURE.md)
- [范围](docs/SCOPE.md)
- [批准的原型](docs/design/PROTOTYPE.html)
