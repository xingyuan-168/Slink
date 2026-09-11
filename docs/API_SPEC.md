# API Spec

## 接口

状态：实施规格，尚无可加载 DLL。requirement_id: REQ-GWM-001。

所有函数 `extern "C" __stdcall`，使用 .def 固定未修饰 WM_ 导出名。返回 int32_t 状态码（0成功），输出值使用指针；HWND作为uint32_t位模式传递。索引均从1开始，无C++对象、STL或bool跨边界。

| 名称 | 参数/输出 | 语义 |
|---|---|---|
| WM_StartMapping | hwnds指针, count, rows, cols, canvasX,Y,W,H, srcX,Y,W,H | 复制数组、创建画布，初始客户区尺寸，启动不等待首帧 |
| WM_ReplaceWindows | hwnds指针,count | 原子替换；同编号ROI/标题/底图/强制状态保留，新编号用默认配置 |
| WM_SetWindow | slot,hwnd | 替换或重新绑定，保留该槽位其他配置；hwnd=0表示清空 |
| WM_RemoveWindow | slot | 清空HWND而不缩短数组 |
| WM_AddWindow | hwnd,outSlot | 末尾追加，默认配置，不填前面的空槽位 |
| WM_SetRegion | slot,x,y,w,h | 客户区物理像素，x/y非负、w/h正；越界区域填背景 |
| WM_SetLayout | rows,cols | 仅改行列；容量不得小于count，不重排 |
| WM_SetTitleA | slot,bytes,byteCount | CP936，不含终止符，空文本恢复自动标题 |
| WM_SetTitleW | slot,utf16,charCount | UTF-16，数量为16位代码单元数，不含终止符 |
| WM_SetBackground | slot,bytes,size | BMP/PNG，slot=0设置全局默认；size=0清除，恢复默认底图/纯色 |
| WM_SetForceBackground | slot,enabled | enabled为0或1 |
| WM_SetFrameRate | fps | 仅15/30/60，默认30 |
| WM_GetCanvasWindow | outHwnd | 输出画布HWND；未运行时返回未启动 |
| WM_GetCanvasStatus | outState,outCount,outRows,outCols | 固定32位输出 |
| WM_GetSlotStatus | slot,outHwnd,outState,outError,outFrameAgeMs | 帧龄使用32位毫秒饱和值，未收帧为0xFFFFFFFF；状态按槽位查询 |
| WM_GetLastErrorTextW | buffer,capacity,outRequired | 当前调用线程最近接口错误；容量含终止符，先查询长度再读取 |
| WM_Stop | 无 | 阻止新工作、撤销捕获并释放资源；重复调用成功，返回后允许卸载 |

## 数据与并发

所有普通整数为32位；字节集为指针+字节数。输入仅需在调用期间有效。数组数量可为0，rows/cols和画布尺寸必须正；空数组创建全空网格。安全检查乘法/坐标溢出，拒绝布局容量不足。

接口不得在持有调用方锁的情况下同步回调易语言，首版只提供轮询。异步捕获异常通过状态查询反馈；状态读取为一致快照。一个接口成功不表示每个HWND都捕获成功。

配置错误时事务不生效。单个失效HWND允许保留槽位并显示失效状态；0表示空槽位。重复HWND合法。

槽位状态：EMPTY、WAITING、LIVE、MINIMIZED、INVALID、CAPTURE_ERROR、FORCED_BACKGROUND、RECOVERING。长期无新帧不自动改变LIVE，帧龄可供调用方判断。画布状态：STOPPED、STARTING、RUNNING、STOPPING、ERROR。

## 错误

错误码：0 OK；1 INVALID_ARGUMENT；2 NOT_STARTED；3 ALREADY_STARTED；4 CAPACITY_EXCEEDED；5 SLOT_OUT_OF_RANGE；6 UNSUPPORTED_SYSTEM；7 GRAPHICS_FAILURE；8 IMAGE_DECODE_FAILED；9 STOPPING；10 BUFFER_TOO_SMALL；11 ACTIVATION_DENIED；12 INTERNAL_ERROR。

未使用自定义底图时返回默认纯色。图像解码失败不丢弃已有底图。错误文字为诊断信息，不可作为程序判断依据。

## 兼容

x86 DLL只能由x86宿主加载；捕获源可为x86/x64顶层窗口。CP936无法表示的标题使用W接口。无旧DLL二进制兼容承诺。

DLL不会修改宿主EXE资源；进程图标由易语言编译设置，画布标题栏图标由DLL资源设置。各导出函数的易语言声明在实现后与dumpbin结果一起验证，不能将此表当作已完成联调证据。
