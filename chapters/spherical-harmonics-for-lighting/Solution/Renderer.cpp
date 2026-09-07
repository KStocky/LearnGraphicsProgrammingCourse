#include "Renderer.hpp"

namespace ch26::spherical_harmonics::solution
{

Renderer::Renderer()
    : RendererCore(std::filesystem::path{__FILE__}.parent_path() / "SphericalHarmonicsLab.hlsl",
                   gpu::LabVariant::Solution)
{
}

} // namespace ch26::spherical_harmonics::solution
