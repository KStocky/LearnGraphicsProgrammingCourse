#include "Renderer.hpp"

#include <cstdio>

int wmain(int argc, wchar_t **argv)
{
    std::puts("Ch27 Solution: irradiance E, V=N GGX NDF prefilter, exact-Smith split sum, and row-local lat-long mip "
              "diagnostics.");
    std::puts(
        "Defaults: roughness=0.55 N.V=0.65; views 1-9 (8=BRDF LUT, 9=paired comparison), Q/E roughness, A/D N.V.");
    ch27::environment_lighting::solution::Renderer renderer{};
    lgp::framework::ApplicationConfiguration configuration{};
    configuration.title = L"Ch27 Environment Lighting Solution (diffuse + split-sum IBL)";
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
