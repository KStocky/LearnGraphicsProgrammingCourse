#include "Renderer.hpp"

namespace ch28::temporal_aa::solution
{
Renderer::Renderer()
    : RendererCore(std::filesystem::path{__FILE__}.parent_path() / "TemporalAaLab.hlsl", gpu::LabVariant::Solution)
{
}
} // namespace ch28::temporal_aa::solution
