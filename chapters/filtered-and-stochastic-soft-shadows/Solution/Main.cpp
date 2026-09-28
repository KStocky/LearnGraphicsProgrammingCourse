#include "Renderer.hpp"
#include <lgp/framework/error.hpp>

#include <cstdio>

int wmain(int argc, wchar_t **argv)
{
    ch38::soft_shadows::solution::Renderer renderer{};
    lgp::framework::ApplicationConfiguration config{};
    config.title = L"Soft Shadows - 1 Hard 2 PCF 3 PCSS 4 Area [ ] Radius";
    auto result = lgp::framework::RunApplication(config, argc, argv, renderer);
    if (!result)
    {
        std::fprintf(stderr, "%s\n", lgp::framework::FormatError(result.error()).c_str());
        return 1;
    }
    return *result;
}
