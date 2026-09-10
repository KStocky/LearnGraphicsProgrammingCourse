#pragma once
#include "../Common/GpuLabSupport.hpp"

namespace ch32::transparency::starter
{
using HeadlessTestConfiguration = gpu::LabConfiguration;
class Renderer final : public gpu::RendererCore
{
  public:
    Renderer();
};
} // namespace ch32::transparency::starter
