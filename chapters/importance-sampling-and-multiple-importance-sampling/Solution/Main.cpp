#include "Renderer.hpp"

#include <cstdio>

int wmain(int argc, wchar_t **argv)
{
    ch17::importance_sampling::solution::Renderer renderer{};

    lgp::framework::ApplicationConfiguration configuration{};
    configuration.title = L"Ch17 Importance Sampling Solution (equal-cost estimator comparison)";
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
