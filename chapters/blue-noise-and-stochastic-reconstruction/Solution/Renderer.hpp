#pragma once

#include "../Common/GpuLabSupport.hpp"

namespace ch25::blue_noise::solution
{

using HeadlessTestConfiguration = gpu::LabConfiguration;

class Renderer final : public gpu::RendererCore
{
  public:
    Renderer();
};

} // namespace ch25::blue_noise::solution
