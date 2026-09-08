#include "Renderer.hpp"

#include <cstdio>

int wmain(int argc, wchar_t **argv)
{
    std::puts("Ch29 Solution: screen-space reflections, environment fallback, and one-bounce screen-space indirect.");
    std::puts("Views: 1 baseline, 2 depth, 3 normals, 4 hit/miss, 5 confidence factors, 6 combined confidence,");
    std::puts("       7 screen reflection, 8 environment fallback, 9 diffuse indirect, 0 final composition,");
    std::puts("       H temporal history acceptance.");
    std::puts("Keys: R reset history, D depth convention, E environment, S screen tracing, G indirect, T temporal,");
    std::puts("      B step length, M step budget. Traversal is linear marching; no hierarchical depth is running.");
    ch29::screen_space_reflections::solution::Renderer renderer{};
    lgp::framework::ApplicationConfiguration configuration{};
    configuration.title = L"Ch29 Screen-Space Reflections and Indirect Lighting Solution";
    configuration.width = 321U;
    configuration.height = 181U;
    auto result = lgp::framework::RunApplication(configuration, argc, argv, renderer);
    if (!result)
    {
        std::fprintf(stderr, "%s\n", lgp::framework::FormatError(result.error()).c_str());
        return 1;
    }
    return result.value();
}
