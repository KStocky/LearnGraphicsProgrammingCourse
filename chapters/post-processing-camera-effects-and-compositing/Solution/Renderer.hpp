#pragma once
#include "../Common/GpuLabSupport.hpp"

namespace ch30::post_processing::solution
{
using HeadlessTestConfiguration = gpu::LabConfiguration;
class Renderer final : public gpu::RendererCore
{
  public:
    Renderer();
};
} // namespace ch30::post_processing::solution
