#include "Renderer.hpp"

namespace ch18::shader_occupancy::solution
{

Renderer::Renderer()
    : RendererCore(std::filesystem::path{__FILE__}.parent_path() / "ShaderOccupancyLab.hlsl", gpu::LabEdition::Solution)
{
}

} // namespace ch18::shader_occupancy::solution
