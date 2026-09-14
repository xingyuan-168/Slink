# API Spec — 单入口版

requirement_id: REQ-GWM-001。2026-09-14 按用户确认收敛接口；本规格替代原 18 命令设计。

唯一导出 `WM_StartMapping`，C ABI、x86 `__stdcall`、.def 固定无修饰名称，返回 32 位错误码。参数依次为：

```cpp
int32_t WM_StartMapping(
  const uint32_t* hwnds, int32_t count, int32_t rows, int32_t cols,
  int32_t canvasX, int32_t canvasY, int32_t canvasWidth, int32_t canvasHeight,
  int32_t srcX, int32_t srcY, int32_t srcWidth, int32_t srcHeight,
  const uint8_t* configuration, int32_t configurationBytes);
```

必须使用 14 参数新声明，不兼容旧 12 参数 stdcall 栈布局。配置为字节集+长度，空配置使用默认值；无跨边界结构体指针、C++对象、回调和运行时设置/查询/停止命令。公共头文件位于 include/GameWindowMapper.h，仅供仓库开发。

## 启动配置

基础参数直接传递。可选配置是小端连续字节，16 字节头为版本1、FPS、全局图片字节数、记录数，随后全局图片字节及槽位记录。每条记录头32字节依次为槽位、ROI四整数、标题字节数、图片字节数、标志，随后标题和图片字节；标志位1覆盖ROI、2强制底图、4使用UTF-16LE（否则CP936）。同编号不能重复记录，未记录槽位继承默认。全部格式、限制、返回码及易语言示例以 [使用说明](E_LANGUAGE.md) 为准。

输入按边界校验、解码、复制完成后启动；配置错误整次拒绝。调用成功表示画布和渲染初始化完成，不等待首帧。异步捕获异常显示在格子内。失效 HWND 保留槽位，重复 HWND 独立配置，不内部排序。

## 并发和生命周期

同一进程只允许一个活动映射，串行化启动检查。已有画布或正在回收时返回3，绝不更新当前配置。关闭之后重新调用才能改变参数。移除其余17个导出，实际二进制导出表必须只有一个函数。

WM_CLOSE 触发捕获/渲染停止，UI 退出并由回收线程 join；销毁映射对象后才解除活动标记。回收线程使用临时模块引用保护执行代码，最后通过 FreeLibraryAndExitThread 释放引用并退出；不永久 pin DLL。DllMain 不等待或初始化图形。调用方不用补调 Stop，也不必通过下一次启动清理。

源窗口和宿主不受关闭影响。调用返回后所有输入可以释放。调用方应遵循自身 LoadLibrary 引用配对；不能在 DllMain 中启动。DLL 内嵌图标用于画布，不修改宿主 EXE。
