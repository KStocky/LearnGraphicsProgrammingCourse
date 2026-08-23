#include "Renderer.hpp"

namespace ch17::importance_sampling::starter
{

Renderer::Renderer()
    : RendererCore(std::filesystem::path{__FILE__}.parent_path() / "ImportanceSamplingLab.hlsl",
                   gpu::LabVariant::Starter)
{
}

} // namespace ch17::importance_sampling::starter
