#include "Renderer.hpp"

#include <cstdio>

int wmain(int argc, wchar_t **argv)
{
    std::puts(
        "Ch27 Starter: analytic lat-long lookup; diffuse and sharp-specular baselines are intentionally incomplete.");
    std::puts("Defaults: roughness=0.55 N.V=0.65; views 1-3, Q/E roughness, A/D N.V.");
    ch27::environment_lighting::starter::Renderer renderer{};
    lgp::framework::ApplicationConfiguration configuration{};
    configuration.title = L"Ch27 Environment Lighting Starter (bounded direct-lookup baseline)";
    configuration.width = 320U;
    configuration.height = 180U;
    configuration.enableDebugLayer = false;
    auto result = lgp::framework::RunApplication(configuration, argc, argv, renderer);
    if (!result)
    {
        std::fprintf(stderr, "%s\n", lgp::framework::FormatError(result.error()).c_str());
        return 1;
    }
    return result.value();
}
