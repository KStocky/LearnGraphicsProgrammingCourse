#include <lgp/framework/command_line.hpp>

#include <limits>
#include <utility>
#include <vector>

namespace lgp::framework
{
namespace
{

[[nodiscard]] Result<std::uint64_t> ParsePositiveInteger(std::wstring_view text)
{
    if (text.empty())
    {
        return std::unexpected(MakeError("ParseCommandLine", "--frames requires a positive integer value."));
    }

    std::uint64_t value = 0;
    for (wchar_t const character : text)
    {
        if (character < L'0' || character > L'9')
        {
            return std::unexpected(MakeError("ParseCommandLine", "--frames requires a positive integer value."));
        }

        std::uint64_t const digit = static_cast<std::uint64_t>(character - L'0');
        if (value > ((std::numeric_limits<std::uint64_t>::max() - digit) / 10U))
        {
            return std::unexpected(MakeError("ParseCommandLine", "--frames value is too large."));
        }

        value = (value * 10U) + digit;
    }

    if (value == 0U)
    {
        return std::unexpected(MakeError("ParseCommandLine", "--frames must be greater than zero."));
    }

    return value;
}

[[nodiscard]] Error MakeArgumentError(std::wstring_view argument, std::string_view message)
{
    auto const narrowArgument = WideToUtf8(argument);
    if (narrowArgument && !narrowArgument->empty())
    {
        return MakeError("ParseCommandLine", std::string{message} + " Offending argument: " + narrowArgument.value());
    }

    return MakeError("ParseCommandLine", std::string{message});
}

} // namespace

Result<CommandLineOptions> ParseCommandLine(std::span<const std::wstring_view> arguments)
{
    CommandLineOptions options;

    for (std::size_t argumentIndex = 1; argumentIndex < arguments.size(); ++argumentIndex)
    {
        std::wstring_view const argument = arguments[argumentIndex];

        if (argument == L"--warp")
        {
            options.useWarpAdapter = true;
            continue;
        }

        if (argument == L"--headless")
        {
            options.headless = true;
            continue;
        }

        if (argument == L"--help" || argument == L"-h" || argument == L"/?")
        {
            options.showHelp = true;
            continue;
        }

        if (argument == L"--frames")
        {
            if (options.maxFrameCount.has_value())
            {
                return std::unexpected(MakeError("ParseCommandLine", "--frames was specified more than once."));
            }

            if (argumentIndex + 1 >= arguments.size())
            {
                return std::unexpected(MakeError("ParseCommandLine", "--frames requires a following value."));
            }

            auto const frameCount = ParsePositiveInteger(arguments[++argumentIndex]);
            if (!frameCount)
            {
                return std::unexpected(std::move(frameCount.error()));
            }

            options.maxFrameCount = frameCount.value();
            continue;
        }

        if (argument.starts_with(L"--frames="))
        {
            if (options.maxFrameCount.has_value())
            {
                return std::unexpected(MakeError("ParseCommandLine", "--frames was specified more than once."));
            }

            auto const frameCount = ParsePositiveInteger(argument.substr(9));
            if (!frameCount)
            {
                return std::unexpected(std::move(frameCount.error()));
            }

            options.maxFrameCount = frameCount.value();
            continue;
        }

        return std::unexpected(MakeArgumentError(argument, "Unknown command line option."));
    }

    return options;
}

Result<CommandLineOptions> ParseCommandLine(int argc, wchar_t const *const *argv)
{
    if (argc < 0)
    {
        return std::unexpected(MakeError("ParseCommandLine", "argc must not be negative."));
    }

    if (argc > 0 && argv == nullptr)
    {
        return std::unexpected(MakeError("ParseCommandLine", "argv must not be null when argc is non-zero."));
    }

    std::vector<std::wstring_view> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int argumentIndex = 0; argumentIndex < argc; ++argumentIndex)
    {
        arguments.emplace_back(argv[argumentIndex] == nullptr ? L"" : argv[argumentIndex]);
    }

    return ParseCommandLine(arguments);
}

std::wstring UsageText()
{
    return L"Options:\n"
           L"  --warp              Use the D3D12 WARP adapter.\n"
           L"  --headless          Run without creating a Win32 window or swap chain.\n"
           L"  --frames <count>    Run a deterministic fixed-step frame count and exit.\n"
           L"  --frames=<count>    Equivalent inline frame-count form.\n"
           L"  --help              Print this help text.\n";
}

} // namespace lgp::framework
