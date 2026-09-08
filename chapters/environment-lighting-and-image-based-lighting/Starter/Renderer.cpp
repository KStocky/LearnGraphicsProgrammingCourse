#include "Renderer.hpp"

namespace ch27::environment_lighting::starter
{

Renderer::Renderer()
    : RendererCore(std::filesystem::path{__FILE__}.parent_path() / "EnvironmentLightingLab.hlsl",
                   gpu::LabVariant::Starter)
{
}

} // namespace ch27::environment_lighting::starter
