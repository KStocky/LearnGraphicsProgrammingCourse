#include "Renderer.hpp"

#include <cstdio>

int wmain(int argc, wchar_t **argv)
{
    std::puts("Ch29 Starter: deferred baseline with a distant environment and no screen-space transport.");
    std::puts("Keys: 1 baseline, 2 linear depth, 3 normals and roughness, D depth convention, E environment mode.");
    std::puts("Solution-only keys are ignored here. Checkpoint: the emissive slab casts no reflection on the floor.");
    ch29::screen_space_reflections::starter::Renderer renderer{};
    lgp::framework::ApplicationConfiguration configuration{};
    configuration.title = L"Ch29 Screen-Space Reflections Starter (environment-only baseline)";
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
