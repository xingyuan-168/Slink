#pragma once
#include <windows.h>
namespace gwm::vd::ui {
// Private native menu/control IDs, never exported through the E-language ABI.
inline constexpr UINT Hide = 0x6a01;
inline constexpr UINT Show = 0x6a02;
inline constexpr UINT Retry = 0x6a03;
inline constexpr UINT RetryRestore = 0x6a04;
}
