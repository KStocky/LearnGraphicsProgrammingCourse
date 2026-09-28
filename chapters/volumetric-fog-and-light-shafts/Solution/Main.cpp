#include "Renderer.hpp"
#include <cstdio>
#include <lgp/framework/error.hpp>
int wmain(int argc, wchar_t **argv)
{
    ch39::fog::solution::Renderer renderer{};
    lgp::framework::ApplicationConfiguration config{};
    config.title = L"Synthetic bounded fog and shadowed single-scattering shafts - Solution";
    auto result = lgp::framework::RunApplication(config, argc, argv, renderer);
    if (!result)
    {
        std::fprintf(stderr, "%s\n", lgp::framework::FormatError(result.error()).c_str());
        return 1;
    }
    return *result;
}
