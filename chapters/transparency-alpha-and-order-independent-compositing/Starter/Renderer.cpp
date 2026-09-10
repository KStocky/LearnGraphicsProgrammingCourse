#include "Renderer.hpp"

namespace ch32::transparency::starter
{
Renderer::Renderer()
    : RendererCore(std::filesystem::path{__FILE__}.parent_path() / "TransparencyLab.hlsl", gpu::LabVariant::Starter)
{
}
} // namespace ch32::transparency::starter
