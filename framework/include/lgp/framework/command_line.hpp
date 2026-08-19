#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <lgp/framework/error.hpp>

namespace lgp::framework
{

struct CommandLineOptions final
{
    bool useWarpAdapter{false};
    bool headless{false};
    std::optional<std::uint64_t> maxFrameCount{};
    bool showHelp{false};
};

[[nodiscard]] Result<CommandLineOptions> ParseCommandLine(std::span<const std::wstring_view> arguments);
[[nodiscard]] Result<CommandLineOptions> ParseCommandLine(int argc, wchar_t const *const *argv);
[[nodiscard]] std::wstring UsageText();

} // namespace lgp::framework
