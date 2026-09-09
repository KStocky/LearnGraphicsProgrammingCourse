#include "Renderer.hpp"

#include <cstdio>

int wmain(int argc, wchar_t **argv)
{
    std::puts("Ch30 Solution: a bounded post chain submitted from a validated pipeline plan.");
    std::puts("Depth of field with a signed circle of confusion, a centred gather shutter, a soft-knee bloom");
    std::puts("pyramid, scene-linear composition, one exposure, a tone curve, display encoding, and the UI.");
    std::puts("Keys: V view, O camera order, P bloom source, I UI placement, D/M/B stages, U UI, R reset.");
    ch30::post_processing::solution::Renderer renderer{};
    lgp::framework::ApplicationConfiguration configuration{};
    configuration.title = L"Ch30 Post-Processing Camera Effects and Compositing Solution";
    configuration.width = 320U;
    configuration.height = 180U;
    auto result = lgp::framework::RunApplication(configuration, argc, argv, renderer);
    if (!result)
    {
        std::fprintf(stderr, "%s\n", lgp::framework::FormatError(result.error()).c_str());
        return 1;
    }
    return result.value();
}
