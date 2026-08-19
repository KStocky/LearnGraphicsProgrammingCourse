#include "Renderer.hpp"

#include <cstdio>

int wmain(int argc, wchar_t **argv)
{
    ch12::gbuffer::solution::Renderer renderer{};

    lgp::framework::ApplicationConfiguration configuration{};
    configuration.title = L"Ch12 G-buffer and Deferred Shading Solution";
    configuration.width = 1600U;
    configuration.height = 900U;
    configuration.enableDebugLayer = false;
    configuration.rtvDescriptorCount = 64U;

    auto result = lgp::framework::RunApplication(configuration, argc, argv, renderer);
    if (!result)
    {
        std::fprintf(stderr, "%s\n", lgp::framework::FormatError(result.error()).c_str());
        return 1;
    }
    return result.value();
}
