#include "Renderer.hpp"
#include <cstdio>
#include <lgp/framework/error.hpp>
int wmain(int argc, wchar_t **argv)
{
    ch39::fog::starter::Renderer renderer{};
    lgp::framework::ApplicationConfiguration config{};
    config.title = L"Synthetic bounded fog - homogeneous unshadowed Starter";
    auto result = lgp::framework::RunApplication(config, argc, argv, renderer);
    if (!result)
    {
        std::fprintf(stderr, "%s\n", lgp::framework::FormatError(result.error()).c_str());
        return 1;
    }
    return *result;
}
