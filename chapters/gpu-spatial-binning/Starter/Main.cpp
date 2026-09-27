#include "Renderer.hpp"

#include <lgp/framework/error.hpp>

#include <cstdio>

auto wmain(int argc, wchar_t **argv) -> int
{
    std::puts("Ch35 Starter: CPU spatial contract builds the sorted particle list; GPU draws uploaded points.");
    std::puts("No GPU binning, sorting, ranges, or neighbor search runs in this baseline.");
    ch35::spatial::starter::Renderer renderer{};
    lgp::framework::ApplicationConfiguration configuration{};
    configuration.title = L"Ch35 GPU Spatial Binning Starter";
    configuration.width = 1280U;
    configuration.height = 720U;
    configuration.enableDebugLayer = false;
    auto result = lgp::framework::RunApplication(configuration, argc, argv, renderer);
    if (!result)
    {
        std::fprintf(stderr, "%s\n", lgp::framework::FormatError(result.error()).c_str());
        return 1;
    }
    return *result;
}
