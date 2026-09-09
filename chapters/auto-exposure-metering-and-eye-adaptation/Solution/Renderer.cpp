#include "Renderer.hpp"

namespace ch31::auto_exposure::solution
{
Renderer::Renderer()
    : RendererCore(std::filesystem::path{__FILE__}.parent_path() / "AutoExposureLab.hlsl", gpu::LabVariant::Solution)
{
}
} // namespace ch31::auto_exposure::solution
