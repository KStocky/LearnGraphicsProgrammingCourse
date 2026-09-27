#include "Renderer.hpp"
#include <cstdio>
#include <lgp/framework/error.hpp>

auto wmain(int argc, wchar_t **argv) -> int
{
    std::puts("Ch36 Starter: CPU-derived initial tracer, no GPU advection.");
    ch36::fluid::starter::Renderer renderer{};
    lgp::framework::ApplicationConfiguration config{};
    config.title = L"Ch36 Eulerian Fluid Advection Starter";
    config.enableDebugLayer = false;
    auto result = lgp::framework::RunApplication(config, argc, argv, renderer);
    if (!result)
    {
        std::fprintf(stderr, "%s\n", lgp::framework::FormatError(result.error()).c_str());
        return 1;
    }
    return *result;
}
