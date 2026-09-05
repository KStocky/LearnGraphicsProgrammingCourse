#include "Renderer.hpp"

#include <cstdio>

int wmain(int argc, wchar_t **argv)
{
    ch19::gpu_profiling::starter::Renderer renderer{};

    lgp::framework::ApplicationConfiguration configuration{};
    configuration.title = L"Ch19 GPU Profiling Starter";
    configuration.width = 1280U;
    configuration.height = 720U;
    configuration.enableDebugLayer = false;

    auto result = lgp::framework::RunApplication(configuration, argc, argv, renderer);
    if (!result)
    {
        std::fprintf(stderr, "%s\n", lgp::framework::FormatError(result.error()).c_str());
        return 1;
    }
    return result.value();
}
