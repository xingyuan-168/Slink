# Open Source Research

## Requirement

Windows x86 DLL 将多个外部 HWND 的客户区 ROI 映射到可缩放画布，提供底图、标题、线程安全 C ABI 和只移动位置的离屏停放。禁止注入、Hook、重新挂父窗口以及通过最小化、透明化或隐藏源窗口实现停放。

## Candidates

### Microsoft PowerToys Crop And Lock
- URL: https://github.com/microsoft/PowerToys/tree/main/src/modules/CropAndLock
- License: MIT，https://github.com/microsoft/PowerToys/blob/main/LICENSE
- 解决什么：窗口缩略图裁剪和显示。
- 可直接复用：不直接依赖整个应用；它不提供本项目的易语言 C ABI、网格会话和共享停放语义。
- 可二开：可以，但会引入与 DLL 无关的应用结构。
- 值得学习：ThumbnailCropAndLockWindow.cpp 中的 DWM 生命周期、源客户区到窗口坐标转换、resize 目标区域更新。
- 风险：其 Reparent 模式不符合本项目要求；不能把示例中的坐标或窗口控制方式不加区分地搬入源窗口控制。

### Win32CaptureSample
- URL: https://github.com/robmikh/Win32CaptureSample
- License: MIT，https://github.com/robmikh/Win32CaptureSample/blob/master/LICENSE
- 解决什么：在 Win32 中通过 HWND 使用 Windows Graphics Capture 和 D3D11。
- 可直接复用：不作为运行时依赖。
- 可二开：可作备用 PoC 的参考，但整个示例不是多会话 DLL。
- 值得学习：捕获项创建、FramePool、帧事件和设备生命周期。
- 风险：帧事件不证明游戏内容更新；部分新接口高于 Windows 10 1903，需要能力检测。只有 DWM 自测失败才进入 WGC 路线。

## Decision
- build
- reason: 自建小型原生 DLL；参考公开 API 用法和生命周期设计，不复制完整开源项目，不引入第三方运行时库。先验证 DWM，必要时才验证 WGC；两个后端均无法保证离屏持续更新时，报告不兼容。

## API evidence

- https://learn.microsoft.com/en-us/windows/win32/dwm/thumbnail-ovw
- https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmregisterthumbnail
- https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowpos
- https://learn.microsoft.com/en-us/windows/apps/develop/media-authoring-processing/screen-capture

2026-09-11 已阅读上述源码、许可证与官方文档。DWM 注册/更新成功不代表帧持续更新；连续性必须通过动态源及画布取样验证。

