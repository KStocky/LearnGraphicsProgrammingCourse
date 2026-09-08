#pragma once
#include "../Common/GpuLabSupport.hpp"

namespace ch28::temporal_aa::solution
{
using HeadlessTestConfiguration = gpu::LabConfiguration;
class Renderer final : public gpu::RendererCore
{
  public:
    Renderer();
};
} // namespace ch28::temporal_aa::solution
