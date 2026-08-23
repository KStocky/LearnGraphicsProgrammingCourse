#include "Renderer.hpp"

namespace ch16::monte_carlo::starter
{

Renderer::Renderer() : RendererCore(std::filesystem::path{__FILE__}.parent_path() / "MonteCarloLab.hlsl", false) {}

} // namespace ch16::monte_carlo::starter
