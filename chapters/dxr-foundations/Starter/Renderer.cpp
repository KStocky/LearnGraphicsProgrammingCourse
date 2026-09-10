#include "Renderer.hpp"

namespace ch33::dxr::starter
{
Renderer::Renderer()
    : RendererCore(std::filesystem::path{__FILE__}.parent_path() / "DxrLab.hlsl", gpu::LabVariant::Starter)
{
}
} // namespace ch33::dxr::starter
