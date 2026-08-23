#pragma once

#include "../Common/GpuLabSupport.hpp"

namespace ch16::monte_carlo::starter
{

using HeadlessTestConfiguration = gpu::LabConfiguration;

class Renderer final : public gpu::RendererCore
{
  public:
    Renderer();
};

} // namespace ch16::monte_carlo::starter
