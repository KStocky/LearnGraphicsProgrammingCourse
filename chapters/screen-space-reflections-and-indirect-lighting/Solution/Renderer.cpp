#include "Renderer.hpp"

namespace ch29::screen_space_reflections::solution
{
Renderer::Renderer()
    : RendererCore(std::filesystem::path{__FILE__}.parent_path() / "ScreenSpaceReflectionLab.hlsl",
                   gpu::LabVariant::Solution)
{
}
} // namespace ch29::screen_space_reflections::solution
