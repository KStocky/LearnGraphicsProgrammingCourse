#pragma once

#include "../Common/GpuLabSupport.hpp"

namespace ch27::environment_lighting::solution
{

using HeadlessTestConfiguration = gpu::LabConfiguration;

class Renderer final : public gpu::RendererCore
{
  public:
    Renderer();
};

} // namespace ch27::environment_lighting::solution
