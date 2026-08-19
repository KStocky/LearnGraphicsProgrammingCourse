#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <expected>
#include <string>
#include <string_view>

#include <Windows.h>

namespace lgp::framework
{

enum class ErrorDomain
{
    Generic,
    HResult,
    Win32,
    InvalidArgument,
    Dxc,
};

struct Error final
{
    ErrorDomain domain{ErrorDomain::Generic};
    std::string operation;
    std::string message;
    HRESULT hresult{E_FAIL};
    DWORD nativeCode{0};
};

template <typename T> using Result = std::expected<T, Error>;

using Status = std::expected<void, Error>;

[[nodiscard]] Error MakeError(std::string operation, std::string message);
[[nodiscard]] Error MakeHResultError(std::string operation, HRESULT hresult, std::string_view context = {});
[[nodiscard]] Error MakeWin32Error(std::string operation, DWORD win32Error, std::string_view context = {});
[[nodiscard]] Error MakeLastError(std::string operation, std::string_view context = {});

[[nodiscard]] std::string DescribeHRESULT(HRESULT hresult);
[[nodiscard]] std::string FormatError(Error const &error);

[[nodiscard]] Result<std::string> WideToUtf8(std::wstring_view text);
[[nodiscard]] Result<std::wstring> Utf8ToWide(std::string_view text);

[[nodiscard]] Status CheckHResult(std::string operation, HRESULT hresult, std::string_view context = {});

} // namespace lgp::framework
