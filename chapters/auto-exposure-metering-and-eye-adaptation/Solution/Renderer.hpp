#pragma once
#include "../Common/GpuLabSupport.hpp"

namespace ch31::auto_exposure::solution
{
using HeadlessTestConfiguration = gpu::LabConfiguration;
class Renderer final : public gpu::RendererCore
{
  public:
    Renderer();
};
} // namespace ch31::auto_exposure::solution
