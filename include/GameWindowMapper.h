#pragma once
#include <stdint.h>
#include <wchar.h>

#if defined(_WIN32)
#define GWM_CALL __stdcall
#if defined(GWM_BUILD)
#define GWM_API
#else
#define GWM_API __declspec(dllimport)
#endif
#else
#define GWM_CALL
#define GWM_API
#endif
#ifdef __cplusplus
extern "C" {
#endif

enum GWM_Result {
    GWM_OK=0, GWM_INVALID_ARGUMENT=1, GWM_NOT_STARTED=2,
    GWM_ALREADY_STARTED=3, GWM_CAPACITY_EXCEEDED=4,
    GWM_SLOT_OUT_OF_RANGE=5, GWM_UNSUPPORTED_SYSTEM=6,
    GWM_GRAPHICS_FAILURE=7, GWM_IMAGE_DECODE_FAILED=8,
    GWM_STOPPING=9, GWM_BUFFER_TOO_SMALL=10,
    GWM_ACTIVATION_DENIED=11, GWM_INTERNAL_ERROR=12
};
enum GWM_CanvasState {
    GWM_STOPPED=0, GWM_STARTING=1, GWM_RUNNING=2,
    GWM_CANVAS_STOPPING=3, GWM_CANVAS_ERROR=4
};
enum GWM_SlotState {
    GWM_EMPTY=0, GWM_WAITING=1, GWM_LIVE=2, GWM_MINIMIZED=3,
    GWM_INVALID=4, GWM_CAPTURE_ERROR=5, GWM_FORCED_BACKGROUND=6,
    GWM_RECOVERING=7
};

/* All slots are 1-based. HWND values are uint32 bit patterns (x86 ABI).
   Input buffers must remain valid only for the duration of the call.
   Explicitly call WM_Stop before unloading this DLL. */
GWM_API int32_t GWM_CALL WM_StartMapping(const uint32_t* hwnds, int32_t count,
    int32_t rows, int32_t cols, int32_t canvasX, int32_t canvasY,
    int32_t canvasWidth, int32_t canvasHeight, int32_t srcX, int32_t srcY,
    int32_t srcWidth, int32_t srcHeight);
GWM_API int32_t GWM_CALL WM_ReplaceWindows(const uint32_t* hwnds,int32_t count);
GWM_API int32_t GWM_CALL WM_SetWindow(int32_t slot,uint32_t hwnd);
GWM_API int32_t GWM_CALL WM_RemoveWindow(int32_t slot);
GWM_API int32_t GWM_CALL WM_AddWindow(uint32_t hwnd,int32_t* outSlot);
GWM_API int32_t GWM_CALL WM_SetRegion(int32_t slot,int32_t x,int32_t y,int32_t w,int32_t h);
GWM_API int32_t GWM_CALL WM_SetLayout(int32_t rows,int32_t cols);
GWM_API int32_t GWM_CALL WM_SetTitleA(int32_t slot,const char* text,int32_t byteCount);
GWM_API int32_t GWM_CALL WM_SetTitleW(int32_t slot,const wchar_t* text,int32_t charCount);
/* slot=0 means global default background; bytes=null,size=0 clears. */
GWM_API int32_t GWM_CALL WM_SetBackground(int32_t slot,const uint8_t* bytes,int32_t size);
GWM_API int32_t GWM_CALL WM_SetForceBackground(int32_t slot,int32_t enabled);
GWM_API int32_t GWM_CALL WM_SetFrameRate(int32_t fps);
GWM_API int32_t GWM_CALL WM_GetCanvasWindow(uint32_t* outHwnd);
GWM_API int32_t GWM_CALL WM_GetCanvasStatus(int32_t* outState,int32_t* outCount,int32_t* outRows,int32_t* outCols);
GWM_API int32_t GWM_CALL WM_GetSlotStatus(int32_t slot,uint32_t* outHwnd,int32_t* outState,int32_t* outError,uint32_t* outFrameAgeMs);
GWM_API int32_t GWM_CALL WM_GetLastErrorTextW(wchar_t* buffer,int32_t capacity,int32_t* outRequired);
GWM_API int32_t GWM_CALL WM_Stop(void);

#ifdef __cplusplus
}
#endif
