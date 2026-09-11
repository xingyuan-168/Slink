# Open Source Research

## Requirement

requirement_id: REQ-GWM-001
summary: 32位易语言调用的多HWND客户区裁剪显示DLL，固定数组槽位、标题和底图。
scope:
  - capture
  - renderer
  - window
  - dll
updated_at: 2026-09-11

## Candidates

### Win32CaptureSample

- URL: https://github.com/robmikh/Win32CaptureSample
- License: MIT，https://github.com/robmikh/Win32CaptureSample/blob/master/LICENSE
- 解决什么：Win32 通过 HWND 创建 WGC 捕获项并使用 D3D11 显示。
- 可直接复用：不作为运行时依赖；不直接复制整个应用。
- 可二开：可以，但其单窗口应用框架不等同于线程安全易语言 DLL。
- 值得学习：CreateForWindow、CreateFreeThreaded、FrameArrived、帧池尺寸更新和资源生命周期。
- 风险：较新可选 API 需能力检测；收帧事件不能证明内容变化，不能承诺最小化持续捕获。

### PowerToys CropAndLock

- URL: https://github.com/microsoft/PowerToys/tree/main/src/modules/CropAndLock
- License: MIT，https://github.com/microsoft/PowerToys/blob/main/LICENSE
- 解决什么：窗口区域裁剪和显示。
- 可直接复用：不直接引入 PowerToys 应用或服务。
- 可二开：可以，但本需求需要独立 x86 DLL、固定槽位和易语言 ABI。
- 值得学习：裁剪交互、缩略显示布局及窗口资源管理。
- 风险：Reparent 行为不属于本次需求；DWM缩略图不能代替所选WGC纹理合成路线。

## Decision

decision: build
reason: 自建轻量 DLL，参考公开 API 的生命周期与设计，不复制整套应用。WGC提供按HWND捕获与GPU纹理，适合逐槽位区域裁剪和合成；普通桌面截图无法满足被遮挡窗口的语义。

## API evidence

- https://learn.microsoft.com/en-us/windows/win32/api/windows.graphics.capture.interop/nf-windows-graphics-capture-interop-igraphicscaptureiteminterop-createforwindow — HWND接口最低Windows 10 1903。
- https://github.com/microsoft/Windows.UI.Composition-Win32-Samples/blob/master/cpp/ScreenCaptureforHWND/README.md — HWND捕获示例与最小化限制。
- https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getclientrect — 客户区坐标和半开边界。
- https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setforegroundwindow — 前台激活受系统限制。
- https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-seticon — 窗口大小图标。

本次已阅读项目说明、许可证、接口文档以及 Win32CaptureSample 的 SimpleCapture.cpp/.h 和 CropAndLock 的 ThumbnailCropAndLockWindow.cpp。2026-09-11 核验 HEAD：Win32CaptureSample `49fefe79fd9b11025f0b5eb91783a98888516070`；PowerToys `bb656da414e09808f6d25c98dca4313e7e37f828`。仅参考 API 与生命周期设计，未复制项目代码、未引入这些项目的二进制或源代码依赖。

实施选择：自建线程和槽位管理，采用自由线程帧池轮询及有界 GPU 纹理交接；不继承样例的应用框架或 PowerToys 的 reparent 行为。
