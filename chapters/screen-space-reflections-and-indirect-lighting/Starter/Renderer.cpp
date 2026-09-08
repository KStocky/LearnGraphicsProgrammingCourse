#include "Renderer.hpp"

namespace ch29::screen_space_reflections::starter
{
Renderer::Renderer()
    : RendererCore(std::filesystem::path{__FILE__}.parent_path() / "ScreenSpaceReflectionLab.hlsl",
                   gpu::LabVariant::Starter)
{
}
} // namespace ch29::screen_space_reflections::starter
