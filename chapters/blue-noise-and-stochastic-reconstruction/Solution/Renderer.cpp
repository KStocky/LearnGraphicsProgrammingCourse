#include "Renderer.hpp"

namespace ch25::blue_noise::solution
{

Renderer::Renderer()
    : RendererCore(std::filesystem::path{__FILE__}.parent_path() / "BlueNoiseLab.hlsl", gpu::LabVariant::Solution)
{
}

} // namespace ch25::blue_noise::solution
