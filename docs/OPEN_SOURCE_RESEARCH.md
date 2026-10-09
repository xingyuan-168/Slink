# Open Source Research

## Requirement

requirement_id: REQ-GWM-002
summary: 保留单命令屏幕墙，在签名虚拟扩展屏中停放源窗口，并在关闭或宿主退出时恢复。
scope:
  - capture
  - renderer
  - window
  - dll
  - virtual-display
  - window-recovery
updated_at: 2026-10-09

## Candidates

### VirtualDrivers Virtual Display Driver

- URL: https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/tag/25.7.23
- License: MIT，https://github.com/VirtualDrivers/Virtual-Display-Driver/blob/25.7.23/LICENSE
- 解决什么：Windows IDD 虚拟显示器、显示模式与显示设备管理。
- 可直接复用：25.7.23 发布包中的原始签名驱动；集成前核验 INF 实际架构、CAT 签名、DLL 依赖与目标系统安装结果，不按 ZIP 的 x86 字样推断操作系统架构。
- 可二开：本轮不修改驱动及 INF，避免破坏签名；用自建原生辅助程序调用 SetupAPI/NewDev 和 DisplayConfig 管理设备与扩展路径。
- 值得学习：设备状态检测、独立的配置目录和虚拟显示模式。官方源码硬件 ID 为 Root\\MttVDD，存在全局配置和管道，不能未经所有权判断接管已安装实例。
- 风险：发布签名不等于本机安装验收通过；安装需要管理员授权，可能要求重启。现有实体扩展屏及重叠验证不能替代 IDD + WGC + 大漠的联合验证。
- 发布资产：VirtualDisplayDriver-x86.Driver.Only.zip，132118 字节；发布页 SHA256 e24210692b442b39af763536330ce78b423f19342b7a7792c26de3944e418b3a。
- 本机核验（2026-10-09）：ZIP SHA256 匹配；INF 为 NTamd64、DriverVer=12/24/2024,11.30.4.434、UMDF 2.25 / IddCx0102；DLL PE Machine=0x8664。CAT/DLL Authenticode 均 Valid，签名人为 SignPath Foundation；signtool verify /pa /c 对 INF 验证通过。尚未安装，不能据此声称设备已运行。

### Microsoft Indirect Display Driver Sample

- URL: https://github.com/microsoft/Windows-driver-samples/tree/main/video/IndirectDisplay
- License: MS-PL，https://github.com/microsoft/Windows-driver-samples/blob/main/LICENSE
- 解决什么：展示 IddCx 监视器连接、模式与帧处理。
- 可直接复用：不作为可交付驱动，样例源码不等于已签名生产安装包。
- 可二开：需要额外驱动维护、签名与兼容验证，不适合本轮快速集成。
- 值得学习：IDD 生命周期；设备安装使用 Windows 官方 API，自建安装辅助程序，不分发 WDK DevCon 二进制。
- 风险：自编译驱动的签名交付问题超出本轮范围。

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

decision: extract
reason: 复用 VirtualDrivers 25.7.23 原始签名驱动，构建轻量安装、显示拓扑和窗口恢复适配层；不引入完整控制面板。既有 REQ-GWM-001 的 WGC/GPU 捕获继续保留。签名或单窗口联合验证不通过时，记录阻碍，不以离屏坐标、最小化或截图降级替代。

## API evidence

- https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/pnputil-examples — 驱动安装需要管理员权限；单纯添加 INF 不保证创建根枚举设备。
- https://learn.microsoft.com/en-us/windows-hardware/drivers/display/indirect-display-driver-model-overview — IDD 为 UMDF 模型，负责虚拟适配器、监视器和显示模式。
- https://github.com/VirtualDrivers/Virtual-Display-Driver/wiki/Registry-Overrides — VDDPATH 可指定配置目录；全局配置要求先核验所有权。

- https://learn.microsoft.com/en-us/windows/win32/api/windows.graphics.capture.interop/nf-windows-graphics-capture-interop-igraphicscaptureiteminterop-createforwindow — HWND接口最低Windows 10 1903。
- https://github.com/microsoft/Windows.UI.Composition-Win32-Samples/blob/master/cpp/ScreenCaptureforHWND/README.md — HWND捕获示例与最小化限制。
- https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getclientrect — 客户区坐标和半开边界。
- https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setforegroundwindow — 前台激活受系统限制。
- https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-seticon — 窗口大小图标。

本次已阅读项目说明、许可证、接口文档以及 Win32CaptureSample 的 SimpleCapture.cpp/.h 和 CropAndLock 的 ThumbnailCropAndLockWindow.cpp。2026-09-11 核验 HEAD：Win32CaptureSample `49fefe79fd9b11025f0b5eb91783a98888516070`；PowerToys `bb656da414e09808f6d25c98dca4313e7e37f828`。仅参考 API 与生命周期设计，未复制项目代码、未引入这些项目的二进制或源代码依赖。

实施选择：自建线程和槽位管理，采用自由线程帧池轮询及有界 GPU 纹理交接；不继承样例的应用框架或 PowerToys 的 reparent 行为。
