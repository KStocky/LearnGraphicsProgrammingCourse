#pragma once

#include "../Common/GpuLabSupport.hpp"

namespace ch27::environment_lighting::starter
{

using HeadlessTestConfiguration = gpu::LabConfiguration;

class Renderer final : public gpu::RendererCore
{
  public:
    Renderer();
};

} // namespace ch27::environment_lighting::starter
