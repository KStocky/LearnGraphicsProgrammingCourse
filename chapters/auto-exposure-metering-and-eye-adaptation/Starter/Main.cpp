#include "Renderer.hpp"

#include <cstdio>

int wmain(int argc, wchar_t **argv)
{
    std::puts("Ch31 Starter: a pre-exposed analytic scene under a declared exposure, and no measurement at all.");
    std::puts("The pre-exposure is removed once, the declared exposure is applied once, then tone map and encode.");
    std::puts("Checkpoint: display=320x180 exposure=manual +1.5 stops pre-exposure=fixed 2.0 histogram=none.");
    std::puts("Keys: V view, S scene, C camera cut, U UI, R reset.");
    ch31::auto_exposure::starter::Renderer renderer{};
    lgp::framework::ApplicationConfiguration configuration{};
    configuration.title = L"Ch31 Auto Exposure Starter (declared exposure baseline)";
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
