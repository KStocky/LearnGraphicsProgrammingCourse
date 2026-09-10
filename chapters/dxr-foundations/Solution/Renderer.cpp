#include "Renderer.hpp"

namespace ch33::dxr::solution
{
Renderer::Renderer()
    : RendererCore(std::filesystem::path{__FILE__}.parent_path() / "DxrLab.hlsl", gpu::LabVariant::Solution)
{
}
} // namespace ch33::dxr::solution
