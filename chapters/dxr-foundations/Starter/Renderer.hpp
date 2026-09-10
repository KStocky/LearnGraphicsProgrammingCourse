#pragma once
#include "../Common/GpuLabSupport.hpp"

namespace ch33::dxr::starter
{
using HeadlessTestConfiguration = gpu::LabConfiguration;
class Renderer final : public gpu::RendererCore
{
  public:
    Renderer();
};
} // namespace ch33::dxr::starter
