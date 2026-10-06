#include "Renderer.hpp"
#include <cstdio>
int wmain(int argc, wchar_t **argv)
{
    std::puts("Chapter42 bounded 3D weakly-compressible SPH; particle diagnostic, NOT a water surface renderer.");
    std::puts("SPACE pause, R reset, V density/pressure/support/speed, G naive/grid, P clamped/signed pressure, "
              "H support .25/.5/.125m, T substeps4/1, C sound4/16m/s, M viscosity1/0, B walls. Physics/search controls "
              "RESET.");
    std::puts(
        "Persistent state. Fixed tick1/120s per submitted frame, default four substeps; no wall-clock accumulator.");
    std::puts("mu is dynamic viscosity kg/(m*s). Poly6 density includes self; spiky pressure and viscosity Laplacian "
              "are a mixed-kernel model. Coincident distinct pairs: pressure direction zero, counted; "
              "finite viscosity damps velocity differences without inventing a direction.");
    std::puts("Wall projection to [-1.99,1.99]m with restitution is NOT boundary-density completion or CCD.");
    std::puts("Sound and motion ratios >0.1 are HEURISTIC warnings, motion >0.5R is an explicit default failed step.");
    std::puts("GPU first error sticky until reset; failed substeps may be partially committed, NOT transactional.");
    std::puts(
        "Logical particle payload counts exclude sample/key/metric traffic; NOT DRAM bandwidth or warp divergence. "
        "WARP is correctness evidence, never a performance claim.");
    std::puts("Headless stdout measures FIRST submitted tick before frame1, not final --frames state. "
              "--frames1 has no measurement line. No ImGui initialization.");
    ch42::sph::LGP_SPH_VARIANT::Renderer renderer{};
    lgp::framework::ApplicationConfiguration configuration{};
    configuration.title = L"Chapter 42 - SPH Particle Fluid Simulation";
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
