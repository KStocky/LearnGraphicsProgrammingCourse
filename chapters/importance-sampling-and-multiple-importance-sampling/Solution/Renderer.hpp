#pragma once

#include "../Common/GpuLabSupport.hpp"

namespace ch17::importance_sampling::solution
{

using HeadlessTestConfiguration = gpu::LabConfiguration;

class Renderer final : public gpu::RendererCore
{
  public:
    Renderer();
};

} // namespace ch17::importance_sampling::solution
