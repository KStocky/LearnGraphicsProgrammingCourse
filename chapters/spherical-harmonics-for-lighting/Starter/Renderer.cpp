#include "Renderer.hpp"

namespace ch26::spherical_harmonics::starter
{

Renderer::Renderer()
    : RendererCore(std::filesystem::path{__FILE__}.parent_path() / "SphericalHarmonicsLab.hlsl",
                   gpu::LabVariant::Starter)
{
}

} // namespace ch26::spherical_harmonics::starter
