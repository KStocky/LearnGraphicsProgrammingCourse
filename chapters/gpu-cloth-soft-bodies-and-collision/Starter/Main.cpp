#include "Renderer.hpp"
#include <cstdio>

int wmain(int argc, wchar_t **argv)
{
    std::puts("Chapter 41: persistent GPU cloth / bounded single-tetrahedron soft body.");
    std::puts("M model, I iterations (8/24/1), C DIMENSIONLESS compliance multiplier (0/1/100), F friction (0.4/0), "
              "S self-contact, T tearing, SPACE pause, R reset. Physical controls reset original rest state.");
    std::puts("Fixed tick 1/60 per submitted frame, four substeps; NOT a wall-clock accumulator or CCD.");
    std::puts("Kind-specific base compliance: distance/shear/bending 1e-5 s^2/kg, area 1e-6 m^2*s^2/kg, "
              "volume 1e-7 m^4*s^2/kg. Effective alpha=base*multiplier; default multiplier0 is rigid.");
    std::puts("Shape stages reject active current tetrahedra with signed volume<=1e-8 m^3 before projection "
              "or tearing and after projection/contact passes; never abs-volume recovery.");
    std::puts("Blue active faces/outline, red disabled faces/broken edges, yellow pins, green environment contacts.");
    std::puts("Top amber distance residual and magenta penetration bars: 0.01m full width; mixed compliant scalar "
              "is diagnostic only, not a cross-unit norm. No energy-conservation or performance claim.");
    std::puts("Headless maps submitted frame 0 once before frame 1, never an unsubmitted command list.");
    std::puts("Energy diagnostics in J: K plus U=-sum(m*dot(gravity,position)), zero potential at world origin; "
              "pins omitted, elastic/model work excluded. K alone is NOT energy drift. Default pinned/contact/"
              "compliant/tearing scenes are NOT conservation tests; controlled freefall tests report E-E_reference.");
    ch41::cloth::LGP_CLOTH_VARIANT::Renderer renderer{};
    lgp::framework::ApplicationConfiguration configuration{};
    configuration.title = L"Chapter 41 - GPU Cloth, Soft Bodies, and Collision";
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
