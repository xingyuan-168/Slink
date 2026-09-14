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

/* One x86 stdcall entry. All input is copied before return; closing the canvas
   releases mapping resources without another call. Empty configuration uses defaults.
   Optional config is packed little-endian with no pointers or padding:
   Header: int32 version(1), fps(0/15/30/60), defaultImageBytes, recordCount;
   then default PNG/BMP bytes, then recordCount records.
   Record: int32 slot(1-based), x,y,w,h, titleBytes,imageBytes,flags;
   then title bytes and PNG/BMP bytes. Each slot may occur once.
   flags: 1=override ROI, 2=force background, 4=UTF-16LE title (else CP936).
   Empty title uses source caption; empty image inherits global background.
   See docs/E_LANGUAGE.md for limits and E-language examples.
   ABI CHANGED: replace old 12-argument declarations with all 14 arguments. */
GWM_API int32_t GWM_CALL WM_StartMapping(const uint32_t* hwnds, int32_t count,
    int32_t rows, int32_t cols, int32_t canvasX, int32_t canvasY,
    int32_t canvasWidth, int32_t canvasHeight, int32_t srcX, int32_t srcY,
    int32_t srcWidth, int32_t srcHeight,
    const uint8_t* configuration, int32_t configurationBytes);
#ifdef __cplusplus
}
#endif
