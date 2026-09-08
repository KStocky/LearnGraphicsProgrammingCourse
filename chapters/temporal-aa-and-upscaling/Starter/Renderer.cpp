#include "Renderer.hpp"

namespace ch28::temporal_aa::starter
{
Renderer::Renderer()
    : RendererCore(std::filesystem::path{__FILE__}.parent_path() / "TemporalAaLab.hlsl", gpu::LabVariant::Starter)
{
}
} // namespace ch28::temporal_aa::starter
