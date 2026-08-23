#include "Renderer.hpp"

namespace ch18::shader_occupancy::starter
{

Renderer::Renderer()
    : RendererCore(std::filesystem::path{__FILE__}.parent_path() / "ShaderOccupancyLab.hlsl", gpu::LabEdition::Starter)
{
}

} // namespace ch18::shader_occupancy::starter
