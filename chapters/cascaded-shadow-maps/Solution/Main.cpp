#include "Renderer.hpp"

#include <cstdio>

// Ch24 Cascaded Shadow Maps Solution - interactive controls (windowed only;
// headless runs are deterministic and ignore input):
//   V  Cycle the view: Shaded -> Cascade color -> Cascade-tinted shaded ->
//      Shadow factor -> Shaded.
//   N  Jump to the normal shaded view.       C  Jump to the cascade-color view.
//   S  Toggle stabilization (texel snapping) on/off. Watch the far cascade edges
//      shimmer as the camera moves once it is off.
//   B  Toggle the cascade-transition blend band on/off. Off shows the hard seam
//      between cascades; on cross-fades it.
//   H  Toggle shadows on/off.
// The current view and toggles are printed to stdout whenever they change.

int wmain(int argc, wchar_t **argv)
{
    ch24::cascaded_shadows::solution::Renderer renderer{};

    lgp::framework::ApplicationConfiguration configuration{};
    configuration.title = L"Ch24 Cascaded Shadow Maps Solution";
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
