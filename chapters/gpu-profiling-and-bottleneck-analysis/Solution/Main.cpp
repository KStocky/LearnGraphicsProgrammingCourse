#include "Renderer.hpp"

#include <cstdio>

// The windowed Solution renders one stable instrumented variant at a time: 1 selects the constant-buffer
// baseline and 2 selects the structured-buffer candidate. The program never alternates automatically, so each
// PIX capture has one unambiguous workload. Headless validation drives the same variants programmatically.
int wmain(int argc, wchar_t **argv)
{
    ch19::gpu_profiling::solution::Renderer renderer{};

    lgp::framework::ApplicationConfiguration configuration{};
    configuration.title = L"Ch19 GPU Profiling Solution";
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
