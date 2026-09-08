#include "Renderer.hpp"

namespace ch27::environment_lighting::solution
{

Renderer::Renderer()
    : RendererCore(std::filesystem::path{__FILE__}.parent_path() / "EnvironmentLightingLab.hlsl",
                   gpu::LabVariant::Solution)
{
}

} // namespace ch27::environment_lighting::solution
