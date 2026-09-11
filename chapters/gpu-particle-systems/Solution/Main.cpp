#include "Renderer.hpp"

#include <cstdio>

int wmain(int argc, wchar_t **argv)
{
    std::puts("Ch34 Solution: the full particle frame on the GPU. Emission allocates free slots from a scanned dead");
    std::puts("list, issues monotone identities and seeds each particle; a fixed-step simulation integrates, resolves");
    std::puts("the ground plane, ages and retires; a stable scan-and-scatter compaction feeds a compute pass that");
    std::puts("writes the D3D12_DRAW_ARGUMENTS an ExecuteIndirect consumes across the UAV-to-indirect barrier.");

    ch34::particles::solution::Renderer renderer{};
    lgp::framework::ApplicationConfiguration configuration{};
    configuration.title = L"Ch34 GPU Particle Systems Solution (indirect GPU pipeline)";
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
