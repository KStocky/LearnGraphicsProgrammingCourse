#include "Renderer.hpp"

#include <cstdio>

int wmain(int argc, wchar_t **argv)
{
    std::puts("Ch32 Solution: transparency as eight explicit stages, none of which is nominated as the winner.");
    std::puts("A bounded per-pixel fragment store with a counted overflow, the declared back-to-front reference,");
    std::puts("the CPU per-draw order beside it with the error between them, alpha testing and stochastic coverage");
    std::puts("as coverage decisions rather than blends, a weighted blended accumulate and resolve with its own");
    std::puts("published error, a clustered light-list seam, a bounded screen-space refraction with a declared");
    std::puts("fallback, fog in the placement the policy asked for, and a temporal reactive mask.");
    std::puts("Keys: V view, S scene, M composite, O traversal, W weight, F fog placement, G fog density, K capacity.");
    ch32::transparency::solution::Renderer renderer{};
    lgp::framework::ApplicationConfiguration configuration{};
    configuration.title = L"Ch32 Transparency, Alpha and Order-Independent Compositing Solution";
    configuration.width = 192U;
    configuration.height = 120U;
    auto result = lgp::framework::RunApplication(configuration, argc, argv, renderer);
    if (!result)
    {
        std::fprintf(stderr, "%s\n", lgp::framework::FormatError(result.error()).c_str());
        return 1;
    }
    return result.value();
}
