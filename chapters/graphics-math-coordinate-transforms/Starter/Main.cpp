#include "../Common/ChapterRenderer.hpp"

#include "DemoLogic.hpp"

#include <cstdio>

int wmain(int argc, wchar_t **argv)
{
    ch01::graphics_math::ChapterRenderer renderer{ch01::graphics_math::starter::CreateLogic()};

    lgp::framework::ApplicationConfiguration configuration{};
    configuration.title = L"Ch01 Graphics Math Starter";
    configuration.width = 1600U;
    configuration.height = 900U;
    configuration.shaderVisibleDescriptorCount = 1024U;
    configuration.rtvDescriptorCount = 32U;

    auto result = lgp::framework::RunApplication(configuration, argc, argv, renderer);
    if (!result)
    {
        std::fprintf(stderr, "%s\n", lgp::framework::FormatError(result.error()).c_str());
        return 1;
    }

    return result.value();
}
