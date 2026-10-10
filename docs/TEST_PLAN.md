# 测试计划 — 屏幕墙

## REQ-GWM-002 当前阶段

只做针对性短验证：GPU 横纵拉伸/偏移/越界、已安装项目设备的非主扩展屏、WGC 动态更新、位置尺寸恢复、重复 HWND、多宿主退出与路径丢失恢复、唯一导出和四次关闭重开。安装与 UI 取消/重试、恢复失败提示、易语言兼容在正式画布接入后验收；大漠效果由用户验收。不得以测试源和后端通过替代用户或 UI 验收。

开发构建先运行 scripts/build.ps1，构建会核验/获取固定驱动包、构建原生 x64 helper 后再嵌入 x86 DLL。gpu_geometry 与 single_entry_lifecycle 不安装驱动；virtual_capture、recovery_integration 需要项目驱动已安装，只操作专用测试 HWND，且测试后恢复/撤销本项目路径。后两项默认不加入 CTest，需显式配置 -DGWM_ENABLE_DISPLAY_TESTS=ON，并串行单独运行；已有恢复辅助进程或活动项目路径时，恢复集成测试跳过，不能算通过。

可复现本阶段四项短检查：scripts/build.ps1 -Test -VirtualDisplayTests。脚本只构建这些测试目标并显式启用对应 CTest；默认 -Test 仍只运行单入口生命周期，不隐式更改显示路径。

### 大漠人工验收入口（开发资产，不放入 output）

驱动已安装后，在 PowerShell 中用当前真实 HWND 替换最后一个数值：

```powershell
& 'D:\Projects-tools\屏幕墙\out\build\x86\Release\recovery_integration_tests.exe' --park 'D:\Projects-tools\屏幕墙\out\build\x86\Release\GameWindowMapper.dll' 真实窗口句柄
```

工具只登记这个 HWND，移到本项目虚拟屏后等待回车；这时用户验证大漠后台绑定、输入和画面更新。完成后在工具控制台按回车，确认恢复原位置。工具退出时独立恢复程序仍负责保护。该工具不会创建映射画布、也不代表正式 UI；用户明确选择 HWND 并手动运行，不自动移动其游戏。此人工模式尚待用户实测。

正式集成后验证新右键操作、初始化进度/失败/取消授权、关闭恢复/失败重试及旧易语言例子。打包只更新两份正式文件并比对哈希，保留用户既有 .e、图片及说明附件。实际结果分别记录后端短测、原生 UI、用户易语言/大漠，见 TEST_REPORT.md。

## REQ-GWM-001 短回归

本轮只运行短验证，不追加旧方案的100次启停、30分钟/16路压力矩阵。

1. 检查 x86 PE：只有一个 WM_StartMapping 固定导出，不含停止、设置、查询和测试专用导出。
2. 使用本轮新建的两个 GDI 窗口作为真实 WGC 源；乱序、重复 HWND 和空槽位启动。验证实际合成像素、可选底图及复制输入后画面不变。
3. 检查配置截断、容量不足和损坏图片被拒绝；运行中重复启动返回3。
4. 四次启动/关闭/重开短循环，依次传高级配置、原始BMP、原始PNG、用户JPEG，三次重开保持宿主DLL加载；不调用 Stop。最后一次调用方释放 LoadLibrary 引用后，活动画布仍正常，关闭后模块自动卸载，源窗口继续存在。无效HWND显示直传底图，正常HWND仍显示WGC画面。
5. 打包只生成/更新 output 的 DLL 和使用说明，DLL 与构建哈希一致；保留该目录的用户资产。demo.e 原样保存在 examples，不改动 input。

测试命令：scripts/build.ps1 -Test，或 CTest 的 single_entry_lifecycle。几何测试源码保留，可按后续具体问题单独调用，本轮不重跑无关检查。

易语言和其他目标窗口由用户验证，报告必须区分本轮原生短验证、旧版记事本/动态源历史结果与待用户测试。
