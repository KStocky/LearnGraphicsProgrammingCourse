#pragma once

#include "../Common/GpuLabSupport.hpp"

namespace ch26::spherical_harmonics::solution
{

using HeadlessTestConfiguration = gpu::LabConfiguration;

class Renderer final : public gpu::RendererCore
{
  public:
    Renderer();
};

} // namespace ch26::spherical_harmonics::solution
