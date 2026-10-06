#include "Renderer.hpp"

#include <cstdio>

int wmain(int argc, wchar_t **argv)
{
    std::puts("Chapter 40: bounded SINGLE STEP, restarted from the same scene each frame.");
    std::puts("S solver, I iterations, T dt, C compliance, M mass ratio, P root pin, V diagnostic.");
    std::puts("Settings report the trusted host stage; FreeMotion is prediction, not a projected solution.");
    std::puts("Headless GPU numerical diagnostics read submitted frame 0 before frame 1 (requires --frames>=2).");
    ch40::pbd::LGP_PBD_VARIANT::Renderer renderer{};
    lgp::framework::ApplicationConfiguration configuration{};
    configuration.title = L"Chapter 40 - bounded GPU single step";
    configuration.width = 960U;
    configuration.height = 640U;
    auto result = lgp::framework::RunApplication(configuration, argc, argv, renderer);
    if (!result)
    {
        std::fprintf(stderr, "%s\n", lgp::framework::FormatError(result.error()).c_str());
        return 1;
    }
    return *result;
}
