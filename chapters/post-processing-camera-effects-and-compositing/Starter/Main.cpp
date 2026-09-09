#include "Renderer.hpp"

#include <cstdio>

int wmain(int argc, wchar_t **argv)
{
    std::puts("Ch30 Starter: a scene-linear, temporally resolved baseline and the honest output path only.");
    std::puts("Exposure is applied exactly once, then a tone curve, the display encoding, and the UI composite.");
    std::puts("Checkpoint: display=320x180 bloom=off depth-of-field=off motion-blur=off transfer=sRGB ui=after.");
    ch30::post_processing::starter::Renderer renderer{};
    lgp::framework::ApplicationConfiguration configuration{};
    configuration.title = L"Ch30 Post-Processing Starter (output path baseline)";
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
