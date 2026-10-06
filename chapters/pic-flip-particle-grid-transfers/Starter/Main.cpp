#include "Renderer.hpp"
#include <cstdio>
int wmain(int argc, wchar_t **argv)
{
    std::puts("Chapter43 bounded 3D staggered MAC PIC/FLIP transfers, NOT a liquid simulator.");
    std::puts("Fixed positions; one transfer round per submitted frame, NOT a physical timestep.");
    std::puts("SPACE pause, R reset, V x/y/z faces or particle delta, A alpha .95/0/1, "
              "U zero/opt-in uniform increment, F transferred/constant/affine manufactured grid. A/U/F RESET.");
    std::puts("Default grid change is ZERO: isolate transfer attenuation. U opts into (.25,-.125,.0625)m/s "
              "per round; repeated increments can exceed the explicit velocity bound and fail.");
    std::puts("Origin(-1,-1,-1)m, spacing .5m, 4^3 cells; each family has 80 faces. "
              "Family lumped mass is NOT cell density and three family sums are NOT total physical mass.");
    std::puts("Invalid geometric corners clipped then weights renormalized; empty faces explicitly invalid. "
              "Constants reproduce on supported boundaries; affine reproduction requires unbiased support.");
    std::puts("FLIP uses common before/after support, topology mismatch fails. "
              "GPU first error sticky until reset; failed rounds are NOT transactional.");
    std::puts("CAS attempts count attempted float additions including retries in the sampled round; "
              "not hardware throughput. WARP correctness only.");
    std::puts("Headless stdout samples FIRST submitted round before frame1, not final --frames state; "
              "--frames1 has no measurement line. No ImGui initialization.");
    ch43::transfers::LGP_TRANSFER_VARIANT::Renderer renderer{};
    lgp::framework::ApplicationConfiguration configuration{};
    configuration.title = L"Chapter 43 - PIC/FLIP Particle-Grid Transfers";
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
