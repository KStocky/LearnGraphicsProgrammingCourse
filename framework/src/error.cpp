#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <lgp/framework/error.hpp>

#include <cstdint>
#include <cwctype>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace lgp::framework
{
namespace
{

[[nodiscard]] std::string AppendContext(std::string_view context, std::string message)
{
    if (context.empty())
    {
        return message;
    }

    if (message.empty())
    {
        return std::string{context};
    }

    return std::string{context} + ": " + message;
}

[[nodiscard]] std::wstring TrimTrailingWhitespace(std::wstring text)
{
    while (!text.empty() && std::iswspace(text.back()) != 0)
    {
        text.pop_back();
    }

    return text;
}

[[nodiscard]] std::string HResultToHexString(HRESULT hresult)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
           << static_cast<std::uint32_t>(hresult);
    return stream.str();
}

} // namespace

Error MakeError(std::string operation, std::string message)
{
    return {
        ErrorDomain::Generic, std::move(operation), std::move(message), E_FAIL, 0,
    };
}

Error MakeHResultError(std::string operation, HRESULT hresult, std::string_view context)
{
    return {
        ErrorDomain::HResult,        std::move(operation), AppendContext(context, DescribeHRESULT(hresult)), hresult,
        static_cast<DWORD>(hresult),
    };
}

Error MakeWin32Error(std::string operation, DWORD win32Error, std::string_view context)
{
    const HRESULT hresult = win32Error == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32(win32Error);

    return {
        ErrorDomain::Win32, std::move(operation), AppendContext(context, DescribeHRESULT(hresult)), hresult, win32Error,
    };
}

Error MakeLastError(std::string operation, std::string_view context)
{
    DWORD const win32Error = ::GetLastError();
    return MakeWin32Error(std::move(operation), win32Error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : win32Error, context);
}

std::string DescribeHRESULT(HRESULT hresult)
{
    LPWSTR messageBuffer = nullptr;
    DWORD const flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD const messageLength = ::FormatMessageW(flags, nullptr, static_cast<DWORD>(hresult), 0,
                                                 reinterpret_cast<LPWSTR>(&messageBuffer), 0, nullptr);

    if (messageLength != 0 && messageBuffer != nullptr)
    {
        std::wstring const message = TrimTrailingWhitespace(std::wstring{messageBuffer, messageLength});
        ::LocalFree(messageBuffer);

        auto const utf8Message = WideToUtf8(message);
        if (utf8Message)
        {
            return utf8Message.value();
        }
    }

    return "HRESULT " + HResultToHexString(hresult);
}

std::string FormatError(Error const &error)
{
    std::ostringstream stream;
    if (!error.operation.empty())
    {
        stream << error.operation;
        if (!error.message.empty())
        {
            stream << ": ";
        }
    }

    if (!error.message.empty())
    {
        stream << error.message;
    }

    stream << " [" << HResultToHexString(error.hresult) << "]";

    if (error.domain == ErrorDomain::Win32 && error.nativeCode != 0)
    {
        stream << " (win32=" << error.nativeCode << ")";
    }

    if (error.domain == ErrorDomain::Dxc)
    {
        stream << " (dxc)";
    }

    return stream.str();
}

Result<std::string> WideToUtf8(std::wstring_view text)
{
    if (text.empty())
    {
        return std::string{};
    }

    int const requiredSize = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                                   static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);

    if (requiredSize <= 0)
    {
        return std::unexpected(MakeLastError("WideCharToMultiByte", "Failed to size a UTF-8 conversion."));
    }

    std::string result(static_cast<std::size_t>(requiredSize), '\0');
    int const convertedSize =
        ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(),
                              requiredSize, nullptr, nullptr);

    if (convertedSize != requiredSize)
    {
        return std::unexpected(MakeLastError("WideCharToMultiByte", "Failed to convert UTF-16 text to UTF-8."));
    }

    return result;
}

Result<std::wstring> Utf8ToWide(std::string_view text)
{
    if (text.empty())
    {
        return std::wstring{};
    }

    int const requiredSize =
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);

    if (requiredSize <= 0)
    {
        return std::unexpected(MakeLastError("MultiByteToWideChar", "Failed to size a UTF-16 conversion."));
    }

    std::wstring result(static_cast<std::size_t>(requiredSize), L'\0');
    int const convertedSize = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                                    static_cast<int>(text.size()), result.data(), requiredSize);

    if (convertedSize != requiredSize)
    {
        return std::unexpected(MakeLastError("MultiByteToWideChar", "Failed to convert UTF-8 text to UTF-16."));
    }

    return result;
}

Status CheckHResult(std::string operation, HRESULT hresult, std::string_view context)
{
    if (FAILED(hresult))
    {
        return std::unexpected(MakeHResultError(std::move(operation), hresult, context));
    }

    return {};
}

} // namespace lgp::framework
