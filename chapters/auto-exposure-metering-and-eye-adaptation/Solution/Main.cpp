#include "Renderer.hpp"

#include <cstdio>

int wmain(int argc, wchar_t **argv)
{
    std::puts("Ch31 Solution: an auto-exposure loop written out as five explicit stages.");
    std::puts("A fixed-point log2 histogram with integer atomics, a fixed-order reduction of the exact sufficient");
    std::puts("statistics, a metering policy, a target from middle grey, and adaptation in log exposure. The frame");
    std::puts("displays what the previous frame committed; this frame's measurement reaches the next frame only.");
    std::puts("Keys: V view, S scene, P policy, W centre weighting, K mask, M mode, C camera cut, U UI, R reset.");
    ch31::auto_exposure::solution::Renderer renderer{};
    lgp::framework::ApplicationConfiguration configuration{};
    configuration.title = L"Ch31 Auto Exposure Metering and Eye Adaptation Solution";
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
