# GameWindowMapper 屏幕墙

供 32 位易语言调用的 Windows 原生窗口墙 DLL。按传入数组的原始顺序，裁剪窗口客户区并显示到可缩放网格。

## 当前状态

2026-09-11：用户已批准本次原型。x86 原生 DLL、演示宿主、动态测试源及易语言声明已经实现，正在进行开发端验收。实际通过项、环境限制和用户易语言测试状态以 [测试报告](docs/TEST_REPORT.md) 为准。HTML 原型中的画面与 HWND 仍是演示数据。

- [交互原型](docs/design/PROTOTYPE.html)：单文件、无外部资源，可直接双击打开。
- [界面规格](docs/design/UI_SPEC.md)
- [实施阶段](docs/IMPLEMENTATION_PLAN.md)
- [接口约定](docs/API_SPEC.md)
- [测试计划](docs/TEST_PLAN.md)
- [测试记录](docs/TEST_REPORT.md)

以 `input/GameWindowMapper_DLL_V2开发设计文档_修订版.md` 和本次用户确认的方案为准。旧提交只作历史参考，不继承离屏隐藏要求、旧原型审批或旧测试结果。

## 技术与交付

C++20 / MSVC / CMake / x86 / 静态 CRT；WGC + D3D11，Direct2D/DirectWrite 绘制文字，WIC 解码 BMP/PNG。运行基线 Windows 10 1903+、Windows 11。

交付内容包含 DLL、C 头文件、易语言调用声明及样例、测试 EXE、多尺寸 ICO、构建脚本和测试报告。用户负责易语言及其他目标窗口的实际接入测试；C++ 宿主验证不替代易语言测试。

## 构建与试用

安装 VS 2022 C++ Build Tools、Windows SDK 10.0.18362 或更新版本、CMake 3.25+。在项目根目录执行：

```powershell
./scripts/build.ps1 -Test
./scripts/build.ps1 -X64Source
./out/build/x86/Release/mapper_demo.exe --hwnds 123456,456789 --rows 1 --cols 2 --region 0 0 800 600
```

请将示例数值替换为真实顶层 HWND。所有槽位从 1 开始，源窗口可以来自 x86/x64 进程。产物位于 `out/build/x86/Release`。打包使用 `./scripts/package.ps1 -SkipBuild`，输出路径由脚本返回。

- [易语言调用与日志](docs/E_LANGUAGE.md)
- [DLL 声明](examples/易语言DLL声明.txt)
- [易语言调用示例](examples/易语言调用示例.txt)
- [C 头文件](include/GameWindowMapper.h)

每次卸载 DLL 或退出宿主前调用 `WM_Stop` 并等待其返回。关闭画布不会关闭源窗口。DLL 不会自动寻找窗口，也不改变宿主全局 DPI。

## 文档与原型检查

```powershell
py -3 scripts/verify_prototype.py
```

此命令只验证本阶段产物，不代表 DLL、捕获性能或真实窗口测试通过。
