#include "Renderer.hpp"
#include <lgp/framework/error.hpp>

#include <cstdio>

auto wmain(int argc, wchar_t **argv) -> int
{
    ch37::projection::solution::Renderer renderer{};
    lgp::framework::ApplicationConfiguration config{};
    config.title = L"Ch37 GPU Pressure Projection Solution";
    config.enableDebugLayer = false;
    auto result = lgp::framework::RunApplication(config, argc, argv, renderer);
    if (!result)
    {
        std::fprintf(stderr, "%s\n", lgp::framework::FormatError(result.error()).c_str());
        return 1;
    }
    return *result;
}
