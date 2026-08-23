#pragma once

#include "../Common/GpuLabSupport.hpp"

namespace ch18::shader_occupancy::starter
{

using HeadlessTestConfiguration = gpu::LabConfiguration;

class Renderer final : public gpu::RendererCore
{
  public:
    Renderer();
};

} // namespace ch18::shader_occupancy::starter
