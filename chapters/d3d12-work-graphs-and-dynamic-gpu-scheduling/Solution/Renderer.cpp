#include "Renderer.hpp"

namespace ch20::work_graphs::solution
{

Renderer::Renderer()
    : RendererCore(std::filesystem::path{__FILE__}.parent_path() / "WorkGraphsLab.hlsl", gpu::LabEdition::Solution)
{
}

} // namespace ch20::work_graphs::solution
