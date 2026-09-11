# CQ_SLink 多窗口画面映射 DLL — 设计与 Codex 开发规格 V3

## 1. 项目目标

开发一个 Windows C++ DLL，供易语言调用。易语言传入多个游戏窗口 HWND，DLL 创建一个独立的“画布窗口”，将这些源窗口按用户指定的行数、列数映射到画布中的网格小窗口。

核心需求：

- 易语言传入窗口句柄数组。
- 支持指定统一的源窗口映射范围，例如源窗口 1920×1080，只显示客户区 `(0,0,800,600)`。
- 支持后续对任意单个窗口单独覆盖源区域。
- 用户指定网格行数、列数。
- 用户指定画布首次打开的位置和大小。
- 画布使用标准 Windows 顶层窗口，可最大化、还原、拖拽自由缩放。
- 画布大小变化后，每个映射区域自动重新布局并缩放。
- 每个映射区域顶部有标题栏，自动显示对应 HWND 的窗口标题。
- 易语言传入一个“字节集”作为无画面/空槽位底图；DLL 内部立即复制字节，调用返回后绝不能继续持有易语言传入内存指针。
- 空句柄、失效句柄、映射失败时显示底图；底图为空或解码失败时显示默认深色背景。
- 正常映射状态下 DLL 不移动、不缩放、不激活源游戏窗口；只有用户通过 Tile 右键菜单执行“后台隐藏/显示并激活”时，才允许按本规格控制对应源窗口。
- 每个映射 Tile 支持鼠标右键菜单：`隐藏窗口`、`显示窗口`。隐藏必须采用“只改变窗口位置、不改变窗口大小/客户区大小/窗口样式/可见状态/最小化状态”的后台停放策略。隐藏前后，画面映射、图色识别/识字链路以及用户现有基于 HWND 的 DX/GDI 后台截图必须保持可用，且返回图像尺寸、客户区坐标系和裁剪语义不得改变；显示时恢复原位置/状态并尝试激活。任何目标游戏若无法满足这些硬约束，隐藏功能必须判定为兼容性失败并回滚，不能用 SW_HIDE、最小化、透明、Cloak、改样式等方式“凑效果”。
- DLL 自己维护 UI 线程与消息循环，避免易语言多线程直接操作窗口/DWM 对象。

## 2. 首选技术路线

### 2.1 V1：DWM Thumbnail 作为主后端

V1 优先使用 Windows Desktop Window Manager Thumbnail API：

- `DwmRegisterThumbnail`
- `DwmUpdateThumbnailProperties`
- `DwmUnregisterThumbnail`
- `DWM_THUMBNAIL_PROPERTIES::rcSource`
- `DWM_THUMBNAIL_PROPERTIES::rcDestination`

理由：本项目第一目标只是“把多个窗口画面实时显示到另一个画布中”，并不要求 DLL 获得像素数据。DWM Thumbnail 可以由系统合成器完成源窗口到目标窗口的缩放和区域映射，代码量、CPU/GPU 拷贝和维护复杂度都显著低于自行抓帧后再用 D3D11 合成。

DLL 对外统一把源坐标定义为“源窗口客户区坐标”。DWM 显示时默认设置 `fSourceClientAreaOnly = TRUE`。例如 `(0,0,800,600)` 就表示游戏客户区左上角 800×600。

### 2.2 必须保留后端抽象

内部定义 `IMirrorBackend`，V1 实现 `DwmThumbnailBackend`。不要把 DWM API 散落到窗口布局/UI 代码中。

建议接口：

```cpp
class IMirrorBackend {
public:
    virtual ~IMirrorBackend() = default;
    virtual bool Attach(HWND canvas, HWND source) = 0;
    virtual void Detach() = 0;
    virtual bool Update(const RECT& src, const RECT& dst, bool visible) = 0;
    virtual bool IsAttached() const = 0;
};
```

如果目标游戏在 DWM 下黑屏、冻结或兼容性不足，V2 可以增加 `WindowsGraphicsCaptureBackend`，外部 DLL ABI 不变。

### 2.3 WGC 备用方案

Windows Graphics Capture 使用 `IGraphicsCaptureItemInterop::CreateForWindow`，最低 Windows 10 1903。若启用该后端，需要 D3D11 设备、Direct3D11CaptureFramePool、GraphicsCaptureSession、纹理裁剪与合成。

不要一开始就实现 WGC。先用目标游戏做 DWM 单窗口技术验证，DWM 可用再完成正式 DLL；只有 DWM 验证失败才切换 WGC。

## 3. UI 技术栈

- C++20
- Win32 API
- DWM API：实时窗口缩略图
- Direct2D：画布底色、tile 背景、边框、标题栏绘制
- DirectWrite：Unicode 窗口标题
- WIC：从易语言字节集解码 PNG/JPEG/BMP 底图
- CMake
- Visual Studio 2022 / MSVC
- 纯 Windows 系统组件，V1 不引入第三方库

链接库建议：

```text
user32.lib
gdi32.lib
dwmapi.lib
d2d1.lib
dwrite.lib
windowscodecs.lib
ole32.lib
```

如果易语言主程序是 32 位，必须提供 x86 DLL。可同时提供 x86/x64 两套产物。不要假定源游戏与 DLL 必须同位数；关键是 DLL 必须与加载它的易语言进程位数一致。

## 4. 线程模型

禁止在 `DllMain` 中创建窗口、启动复杂线程、初始化 COM 或执行 DWM 注册。

`SL_CreateCanvas` 第一次调用时创建一个专属 UI 线程：

```text
易语言任意工作线程
        │
        ▼
C ABI 导出层
        │
        ▼
SessionManager
        │  投递命令
        ▼
Canvas UI Thread
        ├─ Win32 Message Loop
        ├─ CanvasWindow
        ├─ TileManager
        ├─ DwmThumbnailBackend × N
        └─ Direct2D/DirectWrite/WIC
```

所有 Win32 窗口创建、DWM Thumbnail 注册/更新/注销、Direct2D 绘制都在 Canvas UI Thread 完成。

易语言可以从多个线程调用更新函数，但 DLL 内部必须转换成线程安全命令并 `PostMessage`/队列投递到 UI 线程。绝不能让多个易语言线程直接并发操作 `HTHUMBNAIL`、画布 HWND 或渲染资源。

## 5. 数据模型

```cpp
struct SourceRect {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
};

struct TileState {
    uint64_t sourceHwndValue;
    SourceRect sourceRect;
    bool hasCustomSourceRect;
    bool validSource;
    std::wstring title;
    HTHUMBNAIL thumbnail;

    // 源窗口可见性控制
    bool parkedHidden;
    bool hasSavedPlacement;
    WINDOWPLACEMENT savedPlacement;
    RECT savedWindowRect;
};

struct CanvasOptions {
    int32_t rows;
    int32_t cols;
    int32_t initialX;
    int32_t initialY;
    int32_t initialWidth;
    int32_t initialHeight;
    int32_t titleHeightDip;
    int32_t gapDip;
    int32_t scaleMode;
};
```

外部 ABI 不直接暴露这些 C++ 结构；内部可以自由使用。

## 6. 对易语言友好的 DLL ABI

导出函数全部使用：

```cpp
extern "C" __declspec(dllexport)
```

x86 构建使用 `__stdcall`；x64 下 Windows ABI 会统一处理调用约定。

为了最大限度降低易语言结构体对齐、C++ ABI、生命周期问题，主接口只使用整数、指针、长度。

### 6.1 创建画布

```cpp
int64_t __stdcall SL_CreateCanvas(
    const uint64_t* hwndArray,
    int32_t hwndCount,
    int32_t rows,
    int32_t cols,
    int32_t sourceX,
    int32_t sourceY,
    int32_t sourceWidth,
    int32_t sourceHeight,
    int32_t canvasX,
    int32_t canvasY,
    int32_t canvasWidth,
    int32_t canvasHeight,
    const uint8_t* placeholderBytes,
    int32_t placeholderByteCount,
    int32_t scaleMode,
    uint32_t flags
);
```

返回：

- `> 0`：Session ID
- `0`：失败，可调用 `SL_GetLastErrorCode/SL_GetLastErrorTextUtf8`

重要：`hwndArray` 和 `placeholderBytes` 必须在函数返回前完成深拷贝；禁止保存易语言传入的原始地址。

推荐易语言调用语义：

```text
开始映射(
    窗口数组,
    取数组成员数(窗口数组),
    3,
    4,
    0,
    0,
    800,
    600,
    0,
    0,
    1920,
    1080,
    底图字节集,
    取字节集长度(底图字节集),
    1,
    0
)
```

这里表示：3 行 4 列；每个窗口默认只映射客户区 `(0,0,800,600)`；画布初始大小 1920×1080。

### 6.2 获取画布 HWND

```cpp
uint64_t __stdcall SL_GetCanvasHwnd(int64_t sessionId);
```

仅用于易语言需要查询/定位画布时使用。正常情况下无需直接操作。

### 6.3 动态更新窗口数组

```cpp
int32_t __stdcall SL_SetWindows(
    int64_t sessionId,
    const uint64_t* hwndArray,
    int32_t hwndCount
);
```

DLL 必须重新绑定新增/变化的 HWND，并释放已经删除窗口对应的 DWM Thumbnail。

### 6.4 修改统一源区域

```cpp
int32_t __stdcall SL_SetCommonSourceRect(
    int64_t sessionId,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height
);
```

### 6.5 单窗口覆盖源区域

```cpp
int32_t __stdcall SL_SetTileSourceRect(
    int64_t sessionId,
    int32_t tileIndex,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height
);
```

`tileIndex` 从 0 开始。这样当前项目可以使用统一的 0,0,800,600；未来少数窗口需要不同裁剪时也无需改变 ABI。

### 6.6 清除单窗口覆盖

```cpp
int32_t __stdcall SL_ClearTileSourceRectOverride(
    int64_t sessionId,
    int32_t tileIndex
);
```

### 6.7 修改网格

```cpp
int32_t __stdcall SL_SetLayout(
    int64_t sessionId,
    int32_t rows,
    int32_t cols
);
```

如果 `hwndCount > rows * cols`，返回错误，不允许静默丢窗口。

### 6.8 修改底图

```cpp
int32_t __stdcall SL_SetPlaceholderImage(
    int64_t sessionId,
    const uint8_t* imageBytes,
    int32_t imageByteCount
);
```

同样必须立即深拷贝。

### 6.9 修改缩放模式

```cpp
int32_t __stdcall SL_SetScaleMode(
    int64_t sessionId,
    int32_t scaleMode
);
```

推荐定义：

```text
0 = STRETCH  拉伸填满，不保持比例
1 = FIT      保持比例，完整显示，允许留边（默认）
2 = FILL     保持比例，铺满目标区域，允许裁掉边缘
```

V1 至少实现 STRETCH 和 FIT。DWM 原生目标矩形会拉伸；FIT 由 DLL 自己计算一个等比 `rcDestination`。

### 6.10 窗口控制

```cpp
int32_t __stdcall SL_ShowCanvas(int64_t sessionId);
int32_t __stdcall SL_HideCanvas(int64_t sessionId);
int32_t __stdcall SL_SetCanvasRect(
    int64_t sessionId,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height
);
```

用户仍可以直接拖拽/最大化，`WM_SIZE` 时自动重新布局。

### 6.11 状态查询

```cpp
int32_t __stdcall SL_GetTileState(
    int64_t sessionId,
    int32_t tileIndex
);
```

建议：

```text
0 = EMPTY
1 = OK
2 = INVALID_HWND
3 = DWM_REGISTER_FAILED
4 = SOURCE_CLOAKED
```

V1 不做跨线程回调给易语言，避免回调线程与易语言运行时兼容问题；状态需要时由易语言轮询。

### 6.12 销毁

```cpp
int32_t __stdcall SL_DestroyCanvas(int64_t sessionId);
```

销毁顺序：停止接收新命令 → 隐藏/注销全部 Thumbnail → 释放 UI/D2D/WIC 资源 → DestroyWindow → 退出消息循环 → join UI thread → 删除 Session。

### 6.13 错误信息

```cpp
int32_t __stdcall SL_GetLastErrorCode();
int32_t __stdcall SL_GetLastErrorTextUtf8(char* buffer, int32_t bufferBytes);
```

错误信息使用 UTF-8，便于易语言按字节集获取。

### 6.14 源游戏窗口显示/后台隐藏控制

右键菜单操作必须同时提供 DLL API，便于易语言脚本直接控制：

```cpp
int32_t __stdcall SL_HideSourceWindow(
    int64_t sessionId,
    int32_t tileIndex
);

int32_t __stdcall SL_ShowSourceWindow(
    int64_t sessionId,
    int32_t tileIndex,
    int32_t activate
);

int32_t __stdcall SL_GetSourceWindowVisibilityState(
    int64_t sessionId,
    int32_t tileIndex
);
```

建议状态：

```text
0 = UNKNOWN / INVALID
1 = NORMAL_VISIBLE
2 = PARKED_HIDDEN
3 = OS_HIDDEN
4 = MINIMIZED
```

这里的“隐藏窗口”默认不是 `ShowWindow(SW_HIDE)`，而是本项目定义的 **Park-Offscreen 后台隐藏**。原因是用户明确要求隐藏后不得改变窗口大小，并且画布映射、图色识别/识字以及既有 DX/GDI 后台截图链路都不能因为隐藏操作而失效。真正 `SW_HIDE` 或最小化经常导致游戏停止提交画面、DWM Thumbnail/WGC 失去实时内容或后台截图变黑，因此不能作为默认行为。

右键菜单与这些 API 必须走同一套内部命令，不允许维护两套逻辑。

## 7. 画布与 Tile 布局

画布使用标准：

```text
WS_OVERLAPPEDWINDOW
```

因此天然支持最大化、还原、拖拽改变大小。

不要在 DLL 内修改宿主进程的全局 DPI 模式。UI 线程创建画布窗口前优先使用线程级 DPI awareness，例如 `SetThreadDpiAwarenessContext`，避免影响易语言整个程序。

每个 Tile：

```text
┌──────────────────────┐
│ 标题栏：窗口标题       │  ← 约 24~28 DIP
├──────────────────────┤
│                      │
│  DWM 映射画面 / 底图   │
│                      │
└──────────────────────┘
```

布局参数建议：

- outer padding：4 DIP
- tile gap：4 DIP
- title height：26 DIP
- 标题超长：单行省略号
- 空槽位仍绘制标题区和底图，可标题显示“空”或空字符串

`WM_SIZE`、`SL_SetLayout`、窗口数量变化时统一调用 `RecalculateLayout()`。

## 8. 源区域与目标区域算法

源区域默认使用用户指定的统一 `(x,y,w,h)`。

若 `width <= 0` 或 `height <= 0`，约定为整个源客户区。

每次注册/更新前：

```text
GetClientRect(sourceHwnd)
→ 得到客户区大小
→ 将 source rect clamp 到客户区
→ 若最终宽高 <= 0，认为当前源区域非法，隐藏 thumbnail 并显示底图
```

目标区域：

```text
canvas client rect
→ 扣除 outer padding
→ 按 rows/cols 等分
→ 每个 cell 顶部切 titleHeight
→ 剩余部分为 contentRect
→ 根据 scaleMode 计算 thumbnail destination rect
```

FIT 算法：

```text
scale = min(contentW/srcW, contentH/srcH)
dstW = srcW * scale
dstH = srcH * scale
dstX = contentX + (contentW-dstW)/2
dstY = contentY + (contentH-dstH)/2
```

STRETCH：`dst = contentRect`。

## 9. DWM Thumbnail 生命周期

对每个有效 HWND：

```text
DwmRegisterThumbnail(canvasHwnd, sourceHwnd, &thumbnail)
```

然后：

```cpp
DWM_THUMBNAIL_PROPERTIES p{};
p.dwFlags =
    DWM_TNP_RECTDESTINATION |
    DWM_TNP_RECTSOURCE |
    DWM_TNP_VISIBLE |
    DWM_TNP_OPACITY |
    DWM_TNP_SOURCECLIENTAREAONLY;
p.rcSource = ...;
p.rcDestination = ...;
p.opacity = 255;
p.fVisible = TRUE;
p.fSourceClientAreaOnly = TRUE;
DwmUpdateThumbnailProperties(thumbnail, &p);
```

切换句柄或销毁 Tile 时必须 `DwmUnregisterThumbnail`。

不能把无效 HWND 直接注册；先 `IsWindow`。可选检查 `DWMWA_CLOAKED`，对 cloaked 窗口给出状态。

## 10. 标题栏

标题默认自动取源顶层窗口标题。

建议每 500ms～1000ms 刷新一次标题，而不是每帧读取。

标题读取失败或为空时可显示：

```text
HWND: 0x00123456
```

不要创建 N 个真实子窗口作为标题栏；直接在画布上绘制即可，降低 HWND 数量和消息复杂度。

## 11. 底图字节集

输入设计为“压缩图片原始字节”：PNG/JPEG/BMP。

流程：

```text
易语言字节集
→ SL_CreateCanvas / SL_SetPlaceholderImage
→ DLL 深拷贝 std::vector<uint8_t>
→ WIC 从内存流解码
→ 缓存为 D2D Bitmap
→ 空槽位/映射失败时绘制
```

图片默认使用 FIT 居中显示。解码失败不导致画布创建失败，降级为默认深色背景，同时写错误日志/状态。

V1 使用一张全局底图。若未来需要每个 Tile 不同底图，再新增 `SL_SetTilePlaceholderImage`，不要一开始增加复杂度。

## 12. Tile 右键菜单与源窗口显示/隐藏

### 12.1 交互要求

用户在任意有有效 HWND 的 Tile 内容区或标题栏上点击鼠标右键，弹出 Win32 原生上下文菜单：

```text
┌────────────────┐
│ 显示窗口        │
│ 隐藏窗口        │
└────────────────┘
```

行为规则：

- 当前处于正常显示状态：`隐藏窗口` 可用，`显示窗口` 可禁用。
- 当前由 DLL 执行过后台隐藏：`显示窗口` 可用，`隐藏窗口` 可禁用。
- HWND 无效：两项均禁用。
- 不把右键点击转发给游戏；右键只操作画布自己的菜单。
- Tile 命中测试通过画布客户区坐标计算 `tileIndex`，不要给每个 Tile 创建真实子窗口。
- 菜单命令最终调用和导出 API 相同的内部 `HideSourceWindow/ShowSourceWindow` command。

可在标题栏增加很轻量的状态文字，例如 `【后台隐藏】`，但不是硬性要求。

### 12.2 “隐藏窗口”的硬约束：Zero-Impact Park-Offscreen

这里的“隐藏”不是 Windows 语义上的 `SW_HIDE`，而是本项目定义的 **Zero-Impact Park-Offscreen**。它的第一原则不是“让窗口消失”，而是 **不能破坏源窗口的渲染条件与现有自动化截图链路**。

隐藏操作必须满足以下硬约束：

1. **禁止改变窗口大小**：外框宽高和客户区宽高必须保持不变。
2. **禁止最小化**：不得调用 `SW_MINIMIZE` / `SW_SHOWMINIMIZED`。
3. **禁止真正隐藏**：不得调用 `SW_HIDE`。
4. **禁止改窗口样式**：不得修改 `GWL_STYLE/GWL_EXSTYLE` 来实现隐藏。
5. **禁止透明化**：不得通过 `WS_EX_LAYERED`、`SetLayeredWindowAttributes`、Alpha=0 等手段隐藏。
6. **禁止 DWM Cloak**：不得使用 `DWMWA_CLOAK` 一类方式让系统停止正常合成该窗口。
7. **禁止改父窗口/重建窗口**：不得 Reparent，不得替换 HWND。
8. **禁止改变游戏客户区坐标系**：例如原客户区为 1920×1080，隐藏后仍必须是 1920×1080；原识图区域 `(0,0,800,600)` 隐藏后语义完全不变。
9. **画布映射必须继续实时更新**：不能因为隐藏导致 Tile 黑屏、冻结或停帧。
10. **既有基于 HWND 的 DX/GDI 后台截图必须继续可用**：截图尺寸、像素格式、客户区相对坐标不得因隐藏操作而改变。
11. **图色识别/OCR 不应感知到隐藏状态**：只要识图/识字逻辑是对同一 HWND、同一客户区坐标执行，隐藏前后不需要修改脚本参数。

默认算法：

```text
1. IsWindow(hwnd) 校验
2. GetWindowPlacement / GetWindowRect / GetClientRect 保存并记录基线
3. 记录 GWL_STYLE / GWL_EXSTYLE / GetDpiForWindow，仅用于校验，禁止修改
4. 记录隐藏前：
   - windowWidth / windowHeight
   - clientWidth / clientHeight
   - showCmd
   - sourceRect
5. 计算整个虚拟桌面之外的停放坐标
6. 只改变 X/Y：SetWindowPos + SWP_NOSIZE
7. 不改变 Z-order、不激活、不最小化、不隐藏
8. 再次读取 WindowRect / ClientRect / Style / ExStyle / DPI
9. 若任意“不可变化项”发生变化：立即恢复原位置并返回兼容性失败
10. 验证映射后端仍持续产出新帧
11. 通过后才将 tile.parkedHidden = true
```

虚拟桌面范围使用：

```cpp
GetSystemMetrics(SM_XVIRTUALSCREEN);
GetSystemMetrics(SM_YVIRTUALSCREEN);
GetSystemMetrics(SM_CXVIRTUALSCREEN);
GetSystemMetrics(SM_CYVIRTUALSCREEN);
```

移动调用必须是“只移动、不缩放”：

```cpp
SetWindowPos(
    hwnd,
    nullptr,
    parkX,
    parkY,
    0,
    0,
    SWP_NOSIZE |
    SWP_NOZORDER |
    SWP_NOACTIVATE |
    SWP_NOOWNERZORDER |
    SWP_NOSENDCHANGING
);
```

这里 `SWP_NOSIZE` 是硬要求。禁止把原宽高重新传给 `SetWindowPos` 作为“看起来没变”的替代实现，因为某些窗口在 DPI、边框或消息处理上可能因此触发尺寸重算。

### 12.3 截图兼容性的边界必须说清楚

“DX/GDI 截图不受影响”可以作为 **基于 HWND 的后台截图链路** 的硬验收目标，例如：

- 自有 DX 后台截图接口以目标 HWND 为输入；
- `GetDC(hwnd)` / `GetWindowDC(hwnd)` 类窗口 DC 截图；
- `PrintWindow(hwnd, ...)`（前提是目标游戏本身支持）；
- 其他以目标 HWND/客户区为基准的后台取图方式。

但是如果现有 GDI 脚本是：

```text
GetDC(NULL) / GetDC(桌面)
+ BitBlt
+ 屏幕绝对坐标
```

或者使用 Desktop Duplication 抓“物理桌面某个固定屏幕坐标”，那么 **窗口被移出可见桌面后，不可能同时保持原屏幕绝对坐标截图结果不变**。这是两个要求在定义上互相冲突，不是 DLL 实现技巧可以完全消除的问题。

因此本项目把“零影响”正式定义为：

> 对同一 HWND、同一客户区坐标的后台 DX/GDI 取图与图色/OCR流程，隐藏前后无需改变窗口尺寸、截图尺寸、识别坐标和调用参数，并持续得到有效新帧。

如果用户现有脚本使用的是“桌面绝对坐标截图”，Codex 必须明确标记为 `SCREEN_COORD_CAPTURE_INCOMPATIBLE_WITH_OFFSCREEN_HIDE`，不能伪装成已满足。

### 12.3.1 映射后端不能和隐藏状态强耦合

V2 方案中 DWM Thumbnail 是首选，但在现在的硬约束下，选择后端的标准必须调整为：

```text
不是“DWM 能显示就用 DWM”
而是
“隐藏前后都能持续实时映射，才允许使用该后端”
```

因此 PoC 必须分别验证：

```text
DWM Thumbnail + Zero-Impact Park-Offscreen
```

如果目标游戏移出屏幕后 DWM Thumbnail 冻结/黑屏，而游戏本身仍在正常 Present、DX/GDI 后台截图仍正常，则正式 DLL 的映射后端必须切换到：

```text
Windows Graphics Capture + D3D11 自行合成
```

外部 ABI 不变。

如果连 WGC 在目标游戏的离屏状态下也无法持续得到新帧，则该目标游戏不满足“离屏隐藏但完全保持映射”的前提，隐藏功能必须报告不兼容，不能回退到 `SW_HIDE`、最小化、透明化等会破坏截图链路的实现。

### 12.3.2 隐藏操作采用 Fail-Closed

隐藏必须“失败关闭（Fail-Closed）”：

```text
尝试移动
  ↓
尺寸/客户区/样式校验
  ↓
映射实时性校验
  ↓
全部通过 → 标记隐藏成功

任意失败
  ↓
立即恢复原位置
  ↓
返回错误码
  ↓
不改变 tile 状态
```

建议新增错误码：

```text
SL_ERR_HIDE_RESIZED_SOURCE
SL_ERR_HIDE_CLIENT_SIZE_CHANGED
SL_ERR_HIDE_STYLE_CHANGED
SL_ERR_HIDE_MAPPING_STALLED
SL_ERR_HIDE_CAPTURE_INCOMPATIBLE
SL_ERR_HIDE_ROLLBACK_FAILED
```

### 12.4 显示窗口并激活

`SL_ShowSourceWindow(sessionId, tileIndex, 1)` 或菜单“显示窗口”应：

```text
1. 校验 HWND
2. 若 parkedHidden=true，恢复保存的 WINDOWPLACEMENT/RECT
3. 必要时 ShowWindowAsync(hwnd, SW_RESTORE / SW_SHOWNORMAL)
4. SetWindowPos 恢复位置和尺寸
5. BringWindowToTop
6. SetForegroundWindow
7. parkedHidden=false
```

如果原窗口在隐藏前是最大化状态，应尽量恢复原 `WINDOWPLACEMENT`，而不是强制普通窗口。

Windows 对跨进程前台激活存在系统限制，因此 `SetForegroundWindow` 可能返回失败。实现应把“显示恢复成功”和“前台激活成功”分开记录：窗口已经恢复显示但 OS 拒绝抢前台时，不应把整个操作判定为失败。

必要时可在受控范围内尝试：

```text
GetWindowThreadProcessId
GetForegroundWindow
AttachThreadInput
SetForegroundWindow
BringWindowToTop
AttachThreadInput(FALSE)
```

但必须保证异常路径也会解除 `AttachThreadInput`，不能永久绑定线程输入队列。不要使用模拟 Alt 键等侵入式手段作为默认方案。

### 12.5 外部状态变化

用户可能在 DLL 外部手工移动、最小化、关闭游戏窗口。DLL 不能假定保存状态永远有效。

- HWND 已销毁：清除 `parkedHidden` 和保存的 placement。
- 后台隐藏期间 HWND 被外部重新定位：仍以 DLL 最后保存的“隐藏前位置”为恢复目标。
- HWND 被替换：`SL_SetWindows` 必须把旧 HWND 的隐藏状态清理掉，新 HWND 视为正常状态。
- `SL_DestroyCanvas` 默认应恢复所有仍由 DLL `parkedHidden` 的有效源窗口，避免关闭画布后游戏永久留在屏幕外。

### 12.6 必须新增的 PoC 验证

正式实现这个功能前，在 `dwm_poc` 中增加严格的隐藏兼容性门禁：

```text
A. 隐藏前记录 WindowRect / ClientRect / Style / ExStyle / DPI
B. 游戏正常显示，映射 Tile 持续更新
C. 用现有 DX 后台截图连续取图，记录返回尺寸/格式/帧更新情况
D. 用现有 GDI 后台截图连续取图，记录返回尺寸/格式/帧更新情况
E. 执行仅改变 X/Y 的 Park-Offscreen（SWP_NOSIZE）
F. 再次读取 WindowRect / ClientRect / Style / ExStyle / DPI
G. 断言：窗口宽高完全一致
H. 断言：客户区宽高完全一致
I. 断言：Style / ExStyle 未变化
J. 断言：映射 Tile 继续收到新帧，无黑屏/冻结
K. 断言：DX 后台截图继续收到新帧，尺寸/坐标语义不变
L. 断言：GDI 后台截图继续收到新帧，尺寸/坐标语义不变
M. 在固定客户区 ROI 上运行一次图色识别与 OCR，确认脚本参数无需改变
N. 连续隐藏/显示 100 次，尺寸零漂移、位置可完整恢复
O. 双屏/多屏环境验证 park 坐标位于整个虚拟桌面之外
P. 普通窗口/最大化窗口分别验证
```

上述任何一项失败，都视为该目标游戏/该截图链路对当前隐藏方案“不兼容”。Codex 必须先回滚窗口，再报告失败原因；不得自行改用 `SW_HIDE`、最小化、透明、Cloak、改样式等方案绕过验收。

## 13. 窗口失效与动态恢复

游戏可能关闭或重新创建 HWND。

V1 行为：

```text
IsWindow(hwnd) == false
→ 注销旧 thumbnail
→ tile 状态 = INVALID_HWND
→ 显示底图
```

易语言获得新 HWND 后调用 `SL_SetWindows` 即可重新绑定。

不要在 DLL 内通过窗口标题“猜”新的 HWND，句柄来源仍由易语言负责。

## 14. 日志

建议支持可选日志文件，但默认关闭。

调试构建日志至少记录：

```text
Session create/destroy
Canvas HWND
Source HWND bind/unbind
DwmRegisterThumbnail HRESULT
DwmUpdateThumbnailProperties HRESULT
Source rect clamp 结果
Placeholder decode 结果
WM_SIZE 后布局尺寸
```

不要每次 WM_PAINT/每次布局都高频刷日志导致 IO 抖动。

## 15. 错误码建议

```text
0      SL_OK
1001   SL_E_INVALID_ARGUMENT
1002   SL_E_INVALID_SESSION
1003   SL_E_LAYOUT_CAPACITY
1004   SL_E_INVALID_HWND
1005   SL_E_DWM_REGISTER_FAILED
1006   SL_E_DWM_UPDATE_FAILED
1007   SL_E_IMAGE_DECODE_FAILED
1008   SL_E_UI_THREAD_START_FAILED
1009   SL_E_CANVAS_CREATE_FAILED
1010   SL_E_SOURCE_RECT_INVALID
1011   SL_E_SOURCE_WINDOW_CONTROL_FAILED
1012   SL_E_SOURCE_WINDOW_RESTORE_FAILED
1013   SL_E_SOURCE_WINDOW_ACTIVATE_FAILED
```

## 16. 工程目录

```text
slink/
├─ CMakeLists.txt
├─ README.md
├─ include/
│  └─ slink.h
├─ src/
│  ├─ dllmain.cpp
│  ├─ exports.cpp
│  ├─ session_manager.h/.cpp
│  ├─ canvas_session.h/.cpp
│  ├─ canvas_window.h/.cpp
│  ├─ tile_manager.h/.cpp
│  ├─ tile.h/.cpp
│  ├─ layout_engine.h/.cpp
│  ├─ image_decoder.h/.cpp
│  ├─ error.h/.cpp
│  └─ backends/
│     ├─ mirror_backend.h
│     └─ dwm_thumbnail_backend.h/.cpp
├─ tests/
│  ├─ layout_engine_tests.cpp
│  └─ source_rect_tests.cpp
├─ samples/
│  ├─ cpp_demo/
│  └─ easy_language/
│     └─ README.md
└─ docs/
   ├─ API.md
   └─ EASY_LANGUAGE.md
```

不要为了“架构漂亮”拆过多层。上述文件可以在实现过程中按实际代码量适当合并。

## 17. 构建要求

CMake 至少提供：

```text
slink_x86 Release
slink_x64 Release
```

优先考虑 `/MT` 静态链接 MSVC CRT，减少易语言部署时缺 VC Runtime 的问题。

产物：

```text
slink.dll
slink.lib
slink.h
README.md
API.md
EASY_LANGUAGE.md
cpp_demo.exe
```

如果只实际使用 32 位易语言，可先交付 x86，通过后再补 x64。

## 18. 最关键的兼容性验证

在正式开发前先做一个极小 PoC，只做以下事情：

1. 创建一个 Win32 顶层窗口。
2. 用户手工/命令行传入一个目标游戏 HWND。
3. `DwmRegisterThumbnail`。
4. `rcSource = (0,0,800,600)`。
5. 将结果显示到 PoC 窗口。
6. 拖拽调整 PoC 大小并实时更新 `rcDestination`。
7. 把源游戏窗口放在其他窗口后方，确认映射是否继续正常更新。
8. 再测试最小化、窗口化、无边框窗口模式，并记录行为。

只有该游戏的 DWM 映射验证成功后再完成正式 DLL。

如果 DWM 对目标游戏表现异常，停止扩展 DWM 版本，转 WGC 后端验证。不要用 BitBlt/PrintWindow 反复打补丁。

## 19. DWM / WGC / BitBlt 选择结论

| 方案 | 本项目推荐度 | 优点 | 主要问题 |
|---|---:|---|---|
| DWM Thumbnail | V1 首选 | 零/低拷贝、实现简单、系统负责缩放、支持源/目标矩形 | 最小化、独占全屏、某些游戏/保护内容兼容性不保证；拿不到像素 |
| Windows Graphics Capture | V2 备用 | 可直接获得 GPU 帧、可自由合成、后续可接 CV | 代码复杂度高，需要 D3D11/WinRT；资源管理复杂 |
| BitBlt | 不推荐主方案 | 简单 | 遮挡、硬件加速窗口、游戏画面兼容性差 |
| PrintWindow | 不推荐主方案 | 可请求目标窗口绘制 | 很多游戏/DirectX 窗口返回黑屏或不完整，性能不稳定 |

## 20. V1 明确不做

- 不做把画布鼠标点击转发给游戏；但 Tile 自身的右键上下文菜单属于 V1 必做。
- 不做键盘转发。
- 不做 OCR/YOLO。
- 不做屏幕录制。
- 不做每个 Tile 不同底图。
- 不做自动寻找/重连窗口句柄。
- 不做 WGC，除非 DWM PoC 验证失败。
- 不做跨进程注入、Hook、游戏内注入。

这些全部保持在 V1 范围之外，避免项目膨胀。

## 21. 验收标准

必须通过以下场景：

1. 12 个有效 HWND，3 行 4 列，统一源区域 `(0,0,800,600)`，初始画布 `1920×1080`。
2. 每个 Tile 上方显示对应窗口标题。
3. 拖动画布边框时所有 Tile 实时自适应。
4. 最大化、还原正常。
5. 一个 HWND 失效后只有该 Tile 切换为底图，其他 Tile 不受影响。
6. `SL_SetWindows` 可在运行时替换窗口数组。
7. `SL_SetCommonSourceRect` 可实时改变所有窗口裁剪区域。
8. `SL_SetTileSourceRect` 只改变目标 Tile。
9. 行列数修改后即时重新布局。
10. `hwndCount > rows*cols` 必须返回明确错误。
11. 易语言传入数组和字节集后即使原变量改变/释放，DLL 仍稳定，因为内部已深拷贝。
12. 连续 Create/Destroy 100 次不崩溃、不残留窗口、不残留 DWM thumbnail。
13. 无底图、错误底图、空句柄、重复 HWND、极小窗口尺寸均不崩溃。
14. 100%、125%、150% DPI 下标题和布局可用。
15. 易语言多个工作线程同时调用更新 API 不发生数据竞争或 UI 线程崩溃。
16. 任意有效 Tile 右键可弹出“显示窗口/隐藏窗口”菜单。
17. “隐藏窗口”采用 Zero-Impact Park-Offscreen，只允许改变窗口 X/Y；禁止改变外框宽高、客户区宽高、Style/ExStyle、可见状态、最小化状态。
18. Park-Offscreen 后映射 Tile 必须持续实时更新；若 DWM 不满足则切换 WGC 技术验证，不能以黑屏/冻结作为可接受结果。
19. Park-Offscreen 后用户现有基于 HWND 的 DX 与 GDI 后台截图链路必须继续取得有效新帧，截图尺寸与客户区坐标语义不变；图色识别/OCR 参数无需改变。
20. 若检测到尺寸变化、客户区变化、样式变化、映射停帧或截图链路不兼容，隐藏必须立即回滚并返回明确错误。
21. “显示窗口”可恢复隐藏前的位置、尺寸和窗口状态，并尽力置前激活。
22. 连续隐藏/显示同一窗口 100 次位置不漂移、窗口/客户区尺寸零漂移、不崩溃。
23. 销毁 Canvas 时，所有由 DLL 后台停放的有效窗口必须自动恢复，避免遗留到屏幕外。
24. 双屏/多屏环境下隐藏坐标必须位于整个虚拟桌面之外，而不是只移出主屏幕。

## 22. 给 Codex 的执行要求

请按以下顺序开发，不要直接一次性写完整工程：

1. 先建立最小 `dwm_poc`，验证目标游戏 HWND 能否稳定通过 DWM Thumbnail 映射，并实现 `(0,0,800,600)` 裁剪和 resize 自适应。
2. PoC 成功后再创建正式 `slink.dll` 工程与稳定 C ABI。
3. 先完成单 Session + 多 Tile + 行列布局 + 标题栏。
4. 再加入底图字节集/WIC 解码。
5. 再加入动态 `SL_SetWindows`、源区域修改、布局修改。
6. 在 PoC 中先执行“零影响隐藏门禁”：`只移动X/Y且SWP_NOSIZE → 窗口/客户区尺寸零变化 → 映射持续新帧 → 现有DX后台截图持续新帧 → 现有GDI后台截图持续新帧 → 图色/OCR坐标语义不变 → 恢复并激活`。全部通过后才实现 Tile 右键菜单与 `SL_HideSourceWindow/SL_ShowSourceWindow`。
7. 最后完成线程安全、错误处理、日志、x86/x64 构建和测试。
8. 每完成一个阶段先运行 demo 验证，不要在未验证的基础上继续堆功能。
9. 不得在 `DllMain` 做复杂初始化。
10. 不得保存易语言数组/字节集的调用方指针。
11. 不得为了赶进度改用 `BitBlt/PrintWindow` 作为主后端；若 DWM 不兼容目标游戏，报告验证结果并切换到 WGC 技术验证。

## 23. Codex 需要最终汇报的内容

完成后输出：

```text
1. 实现状态与未完成项
2. DWM 对目标游戏的验证结论
3. 编译方式
4. x86/x64 产物路径
5. 所有导出 API
6. 易语言参数类型对应表
7. 最小易语言调用示例
8. 已执行测试及结果
9. 已知限制
10. 如果 DWM 不兼容，WGC 后端切换建议
```

## 24. 易语言侧参数类型建议

最终需要 Codex 在 `docs/EASY_LANGUAGE.md` 给出精确声明。原则如下：

| C 类型 | 易语言建议 |
|---|---|
| `int32_t` | 整数型 |
| `uint32_t` | 整数型（按位解释） |
| `int64_t` Session ID | 长整数型 |
| `uint64_t` HWND 值 | 长整数型 |
| `const uint64_t*` | 长整数型数组首地址/数组地址 |
| `const uint8_t*` | 字节集数据地址 |
| byte count | `取字节集长度()` |

Codex 必须根据实际易语言 DLL 调用方式补一个可运行的易语言调用示例，不能只给 C++ demo。

## 25. 设计结论

这个项目第一版不应该做成“多路截图 + CPU 拷贝 + OpenCV 缩放 + GDI 绘制”。它本质上是 Windows 桌面合成问题。

最合理的第一版是：

```text
易语言窗口句柄数组
→ C ABI DLL
→ 独立 UI Thread
→ Win32 Canvas Window
→ Grid Layout
→ DWM Thumbnail × N
→ rcSource 裁剪源客户区
→ rcDestination 自适应画布
→ Direct2D/DirectWrite 标题与背景
→ WIC 解码底图字节集
→ Tile 右键菜单
→ Park-Offscreen 后台隐藏 / 恢复显示并激活
```

先用 DWM PoC 验证目标游戏，再决定是否需要 WGC，是本项目最重要的技术决策。
