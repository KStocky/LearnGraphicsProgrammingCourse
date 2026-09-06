#include "Renderer.hpp"

#include <cstdio>

// Ch23 Material Layering Solution - interactive controls (windowed only; headless
// runs are fully deterministic and ignore input):
//   1 / 2 / 3 / 4  Select preset: Baseline (isotropic) / Anisotropic / Clearcoat /
//                  Emissive. Baseline is byte-identical to the Starter.
//   V              Cycle the diagnostic output view: Final -> Base (coat-attenuated)
//                  -> Coat reflection -> Emission -> Coat transmission (energy) ->
//                  Final. In the Final view, Base + Coat + Emission sum to Final.
//   G              Toggle geometry: material Sphere <-> analytic Probe card. The
//                  probe card shades one constant surface so anisotropy orientation
//                  and the CPU parity probes are easy to read.
//   [ / ]          Decrease / increase display exposure (EV).
//   Right-drag     Orbit the camera. Mouse wheel dollies in and out.
// The current preset/view/geometry is also printed to stdout whenever it changes.

int wmain(int argc, wchar_t **argv)
{
    ch23::material_layering::solution::Renderer renderer{};

    lgp::framework::ApplicationConfiguration configuration{};
    configuration.title = L"Ch23 Material Layering Solution";
    configuration.width = 1280U;
    configuration.height = 720U;

    lgp::framework::Result<int> const result = lgp::framework::RunApplication(configuration, argc, argv, renderer);
    if (!result)
    {
        std::fprintf(stderr, "%s\n", lgp::framework::FormatError(result.error()).c_str());
        return 1;
    }
    return *result;
}
