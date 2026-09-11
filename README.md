# GameWindowMapper 屏幕墙

供 32 位易语言调用的 Windows 原生窗口墙 DLL。按传入数组的原始顺序，裁剪窗口客户区并显示到可缩放网格。

## 当前状态

2026-09-11：本次重建的需求、接口设计与交互原型已建立，等待用户确认本次原型。尚未实现或交付正式 DLL，尚未执行记事本和易语言联调。原型里的 HWND、窗口画面及状态都是模拟数据。

- [交互原型](docs/design/PROTOTYPE.html)：单文件、无外部资源，可直接双击打开。
- [界面规格](docs/design/UI_SPEC.md)
- [实施阶段](docs/IMPLEMENTATION_PLAN.md)
- [接口约定](docs/API_SPEC.md)
- [测试计划](docs/TEST_PLAN.md)
- [测试记录](docs/TEST_REPORT.md)

以 `input/GameWindowMapper_DLL_V2开发设计文档_修订版.md` 和本次用户确认的方案为准。旧提交只作历史参考，不继承离屏隐藏要求、旧原型审批或旧测试结果。

## 技术与交付

C++20 / MSVC / CMake / x86 / 静态 CRT；WGC + D3D11，Direct2D/DirectWrite 绘制文字，WIC 解码 BMP/PNG。运行基线 Windows 10 1903+、Windows 11。

后续交付 DLL、C 头文件、易语言调用声明及样例、测试 EXE、多尺寸 ICO、构建脚本和测试报告。先由开发端用独立记事本窗口自测，再通知用户自行进行易语言及其他窗口测试。

## 文档与原型检查

```powershell
py -3 scripts/verify_prototype.py
```

此命令只验证本阶段产物，不代表 DLL、捕获性能或真实窗口测试通过。
