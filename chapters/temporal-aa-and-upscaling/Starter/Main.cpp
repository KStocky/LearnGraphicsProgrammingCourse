#include "Renderer.hpp"

#include <cstdio>

int wmain(int argc, wchar_t **argv)
{
    std::puts("Ch28 Starter: current-frame bilinear reconstruction from a jittered reduced-resolution analytic scene.");
    std::puts(
        "No temporal history is read. Views compare the native current frame with reduced spatial reconstruction.");
    std::puts("Checkpoint: display=321x181 render=203x115 scale=0.63 history=disabled jitter=Halton8.");
    ch28::temporal_aa::starter::Renderer renderer{};
    lgp::framework::ApplicationConfiguration configuration{};
    configuration.title = L"Ch28 Temporal AA Starter (spatial-only baseline)";
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
