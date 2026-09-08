#pragma once
#include "../Common/GpuLabSupport.hpp"

namespace ch29::screen_space_reflections::solution
{
using HeadlessTestConfiguration = gpu::LabConfiguration;
class Renderer final : public gpu::RendererCore
{
  public:
    Renderer();
};
} // namespace ch29::screen_space_reflections::solution
