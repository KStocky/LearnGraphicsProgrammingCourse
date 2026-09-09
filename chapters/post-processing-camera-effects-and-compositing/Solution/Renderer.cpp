#include "Renderer.hpp"

namespace ch30::post_processing::solution
{
Renderer::Renderer()
    : RendererCore(std::filesystem::path{__FILE__}.parent_path() / "PostProcessingLab.hlsl", gpu::LabVariant::Solution)
{
}
} // namespace ch30::post_processing::solution
