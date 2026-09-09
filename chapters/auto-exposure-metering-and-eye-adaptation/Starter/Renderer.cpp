#include "Renderer.hpp"

namespace ch31::auto_exposure::starter
{
Renderer::Renderer()
    : RendererCore(std::filesystem::path{__FILE__}.parent_path() / "AutoExposureLab.hlsl", gpu::LabVariant::Starter)
{
}
} // namespace ch31::auto_exposure::starter
