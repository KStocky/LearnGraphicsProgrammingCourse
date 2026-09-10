#pragma once
#include "../Common/GpuLabSupport.hpp"

namespace ch32::transparency::solution
{
using HeadlessTestConfiguration = gpu::LabConfiguration;
class Renderer final : public gpu::RendererCore
{
  public:
    Renderer();
};
} // namespace ch32::transparency::solution
