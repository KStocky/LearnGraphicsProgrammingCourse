#include "Renderer.hpp"

namespace ch20::work_graphs::starter
{

Renderer::Renderer()
    : RendererCore(std::filesystem::path{__FILE__}.parent_path() / "WorkGraphsLab.hlsl", gpu::LabEdition::Starter)
{
}

} // namespace ch20::work_graphs::starter
