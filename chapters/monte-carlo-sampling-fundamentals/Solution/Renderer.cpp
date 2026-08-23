#include "Renderer.hpp"

namespace ch16::monte_carlo::solution
{

Renderer::Renderer() : RendererCore(std::filesystem::path{__FILE__}.parent_path() / "MonteCarloLab.hlsl", true) {}

} // namespace ch16::monte_carlo::solution
