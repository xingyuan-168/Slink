#pragma once
#include <windows.h>
#include <string>

namespace gwm::vd {
// Extracts the embedded, individually SHA256-pinned runtime files into the
// current user's restricted LocalAppData\GameWindowMapper\Runtime\v1 directory.
// Returns gwm_recovery.exe; its original driver files are in the driver sibling
// directory. Reuses files only when their bytes match. Does not launch/install.
// Throws std::system_error for a Windows failure or invalid/tampered payload.
std::wstring PrepareRuntimePayload(HINSTANCE module);
}
