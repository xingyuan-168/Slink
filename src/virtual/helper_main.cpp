#include "broker.h"
#include <windows.h>
#include <string_view>
#include <system_error>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return ERROR_INVALID_PARAMETER;
    try {
        const std::wstring_view command(argv[1]);
        if (command == L"--broker") return static_cast<int>(gwm::vd::RunRecoveryBroker());
        if (command == L"--install") return static_cast<int>(gwm::vd::RunDriverInstaller());
        return ERROR_INVALID_PARAMETER;
    } catch (const std::system_error& error) {
        return error.code().value() ? error.code().value() : ERROR_GEN_FAILURE;
    } catch (...) { return ERROR_UNHANDLED_EXCEPTION; }
}
