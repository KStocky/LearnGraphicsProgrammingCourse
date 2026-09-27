#include "Renderer.hpp"
#include <cstdio>
#include <lgp/framework/error.hpp>

auto wmain(int argc, wchar_t **argv) -> int
{
    std::puts("Ch36 Solution: GPU MAC-face velocity transport and force-driven scalar advection.");
    ch36::fluid::solution::Renderer renderer{};
    lgp::framework::ApplicationConfiguration config{};
    config.title = L"Ch36 Eulerian Fluid Advection Solution";
    config.enableDebugLayer = false;
    auto result = lgp::framework::RunApplication(config, argc, argv, renderer);
    if (!result)
    {
        std::fprintf(stderr, "%s\n", lgp::framework::FormatError(result.error()).c_str());
        return 1;
    }
    return *result;
}
