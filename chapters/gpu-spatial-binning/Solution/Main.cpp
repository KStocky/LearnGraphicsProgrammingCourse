#include "Renderer.hpp"

#include <lgp/framework/error.hpp>

#include <cstdio>

auto wmain(int argc, wchar_t **argv) -> int
{
    std::puts("Ch35 Solution: GPU builds bounded keys, bitonic-sorts (key, identity), constructs exact cell");
    std::puts("ranges and enumerates clamped neighbor stencils before drawing the GPU-sorted records.");
    ch35::spatial::solution::Renderer renderer{};
    lgp::framework::ApplicationConfiguration configuration{};
    configuration.title = L"Ch35 GPU Spatial Binning Solution";
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
