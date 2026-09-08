#include "Renderer.hpp"

#include <cstdio>

int wmain(int argc, wchar_t **argv)
{
    std::puts(
        "Ch28 Solution: display-resolution temporal history, rejection, YCoCg clipping, feedback, and sharpening.");
    std::puts("Native TAA and temporal upscaling are distinct modes; default upscale scale=0.63, Halton period=8.");
    std::puts("Checkpoint: display=321x181 render=203x115 history=ping-pong metadata feedback=[0.05,0.95].");
    ch28::temporal_aa::solution::Renderer renderer{};
    lgp::framework::ApplicationConfiguration configuration{};
    configuration.title = L"Ch28 Temporal AA and Upscaling Solution";
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
