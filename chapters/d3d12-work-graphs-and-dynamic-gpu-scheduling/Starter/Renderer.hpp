#pragma once

#include "../Common/GpuLabSupport.hpp"

namespace ch20::work_graphs::starter
{

using HeadlessTestConfiguration = gpu::LabConfiguration;

class Renderer final : public gpu::RendererCore
{
  public:
    Renderer();
};

} // namespace ch20::work_graphs::starter
