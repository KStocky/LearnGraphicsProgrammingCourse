#include "Renderer.hpp"

namespace ch30::post_processing::starter
{
Renderer::Renderer()
    : RendererCore(std::filesystem::path{__FILE__}.parent_path() / "PostProcessingLab.hlsl", gpu::LabVariant::Starter)
{
}
} // namespace ch30::post_processing::starter
