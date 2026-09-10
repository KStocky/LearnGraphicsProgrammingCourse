#include "Renderer.hpp"

#include <cstdio>

int wmain(int argc, wchar_t **argv)
{
    std::puts("Ch32 Starter: an opaque scene, a CPU per-draw back-to-front sort, and premultiplied source-over.");
    std::puts("Every fragment is a blend, every fragment is tested against the opaque depth, and every fragment");
    std::puts("consumes the light list of its own depth slice. There is no per-pixel sort, no order-independent");
    std::puts("accumulator, no coverage decision, no refraction and no reactive mask.");
    std::puts("Checkpoint: display=192x120 scene=showcase composite=per-draw order capacity=8 fog=none.");
    std::puts("Keys: V view, S scene.");
    ch32::transparency::starter::Renderer renderer{};
    lgp::framework::ApplicationConfiguration configuration{};
    configuration.title = L"Ch32 Transparency Starter (CPU per-draw sorted source-over)";
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
