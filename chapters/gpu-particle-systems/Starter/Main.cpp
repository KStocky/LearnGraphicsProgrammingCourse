#include "Renderer.hpp"

#include <cstdio>

int wmain(int argc, wchar_t **argv)
{
    std::puts("Ch34 Starter: an honest particle baseline. The CPU reference advances emission, fixed-step");
    std::puts("simulation, lifetime and compaction, and the host uploads the compacted live set for a direct");
    std::puts("instanced draw. No GPU emission, simulation, compaction or indirect arguments run here; the stage");
    std::puts("ledger reports every one of them as skipped, and the Solution replaces the upload with the");
    std::puts("equivalent on-GPU passes that produce the same picture.");

    ch34::particles::starter::Renderer renderer{};
    lgp::framework::ApplicationConfiguration configuration{};
    configuration.title = L"Ch34 GPU Particle Systems Starter (CPU-reference baseline)";
    configuration.width = 1280U;
    configuration.height = 720U;
    configuration.enableDebugLayer = false;

    auto result = lgp::framework::RunApplication(configuration, argc, argv, renderer);
    if (!result)
    {
        std::fprintf(stderr, "%s\n", lgp::framework::FormatError(result.error()).c_str());
        return 1;
    }
    return result.value();
}
