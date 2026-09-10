#include "Renderer.hpp"

namespace ch32::transparency::solution
{
Renderer::Renderer()
    : RendererCore(std::filesystem::path{__FILE__}.parent_path() / "TransparencyLab.hlsl", gpu::LabVariant::Solution)
{
}
} // namespace ch32::transparency::solution
