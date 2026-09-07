#include "Renderer.hpp"

namespace ch25::blue_noise::starter
{

Renderer::Renderer()
    : RendererCore(std::filesystem::path{__FILE__}.parent_path() / "BlueNoiseLab.hlsl", gpu::LabVariant::Starter)
{
}

} // namespace ch25::blue_noise::starter
