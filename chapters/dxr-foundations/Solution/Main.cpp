#include "Renderer.hpp"

#include <cstdio>

int wmain(int argc, wchar_t **argv)
{
    std::puts("Ch33 Solution: the DXR 1.0 frame as nine ordered stages, each publishing its own evidence. A one-");
    std::puts("geometry triangle bottom-level build, the barrier the top-level build needs, a top-level build or");
    std::puts("refit of three instances, the barrier the dispatch needs, a runtime lib_6_3 library resolved into a");
    std::puts("raytracing state object with a hit-group-local root signature, a shader table whose four hit-group");
    std::puts("records report the index the traversal selected, SetPipelineState1, DispatchRays, and the readback.");
    std::puts("Keys: V view, S scene, M miss record, R ray contribution, 1/2/3 instance mask, F face culling,");
    std::puts("      T ray interval, B rebuild/refit.");
    ch33::dxr::solution::Renderer renderer{};
    lgp::framework::ApplicationConfiguration configuration{};
    configuration.title = L"Ch33 DXR Foundations Solution (acceleration structures, state object, shader table)";
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
