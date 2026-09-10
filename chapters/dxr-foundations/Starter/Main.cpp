#include "Renderer.hpp"

#include <cstdio>

int wmain(int argc, wchar_t **argv)
{
    std::puts("Ch33 Starter: the same scene, traced without a raytracing pipeline. A compute shader transforms each");
    std::puts("ray into every instance's object space, intersects two triangles with Moller-Trumbore, applies the");
    std::puts("instance mask, the strict TMin < t < TMax interval and the object-space facing rule, and computes the");
    std::puts("hit-group record index itself. It builds no acceleration structure, creates no state object and");
    std::puts("uploads no shader table, and the stage ledger says exactly that.");
    std::puts("Checkpoint: grid=96x64 scene=paired instances=3 mask=0xFF miss=0 view=final.");
    std::puts("Keys: V view, S scene, M miss record, R ray contribution, 1/2/3 instance mask, F face culling,");
    std::puts("      T ray interval.");
    ch33::dxr::starter::Renderer renderer{};
    lgp::framework::ApplicationConfiguration configuration{};
    configuration.title = L"Ch33 DXR Foundations Starter (analytic ray/triangle baseline)";
    configuration.width = 384U;
    configuration.height = 256U;
    auto result = lgp::framework::RunApplication(configuration, argc, argv, renderer);
    if (!result)
    {
        std::fprintf(stderr, "%s\n", lgp::framework::FormatError(result.error()).c_str());
        return 1;
    }
    return result.value();
}
