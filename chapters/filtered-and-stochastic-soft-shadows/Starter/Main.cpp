#include "Renderer.hpp"
#include <lgp/framework/error.hpp>

#include <cstdio>

int wmain(int argc, wchar_t **argv)
{
    ch38::soft_shadows::starter::Renderer renderer{};
    lgp::framework::ApplicationConfiguration config{};
    config.title = L"Filtered and Stochastic Soft Shadows - Starter";
    auto result = lgp::framework::RunApplication(config, argc, argv, renderer);
    if (!result)
    {
        std::fprintf(stderr, "%s\n", lgp::framework::FormatError(result.error()).c_str());
        return 1;
    }
    return *result;
}
