#include "Renderer.hpp"

namespace ch17::importance_sampling::solution
{

Renderer::Renderer()
    : RendererCore(std::filesystem::path{__FILE__}.parent_path() / "ImportanceSamplingLab.hlsl",
                   gpu::LabVariant::Solution)
{
}

} // namespace ch17::importance_sampling::solution
