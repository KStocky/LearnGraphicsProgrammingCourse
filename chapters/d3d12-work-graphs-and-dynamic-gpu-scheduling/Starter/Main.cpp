#include "Renderer.hpp"

#include <cstdio>

int wmain(int argc, wchar_t **argv)
{
    ch20::work_graphs::starter::Renderer renderer{};

    lgp::framework::ApplicationConfiguration configuration{};
    configuration.title = L"Ch20 Work Graphs Starter (explicit fixed Dispatch stages)";
    configuration.width = 640U;
    configuration.height = 360U;
    configuration.enableDebugLayer = false;

    auto result = lgp::framework::RunApplication(configuration, argc, argv, renderer);
    if (!result)
    {
        std::fprintf(stderr, "%s\n", lgp::framework::FormatError(result.error()).c_str());
        return 1;
    }
    return result.value();
}
