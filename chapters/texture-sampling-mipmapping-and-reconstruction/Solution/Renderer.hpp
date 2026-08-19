#pragma once

#include <lgp/framework/application.hpp>

#include <cstdint>
#include <memory>

namespace ch03::texture::solution
{

enum class SamplerMode : std::uint32_t
{
    Point = 0,
    Bilinear,
    Trilinear,
    Anisotropic,
};

class Renderer final : public lgp::framework::IChapterRenderer
{
  public:
    Renderer();
    ~Renderer() override;

    Renderer(Renderer &&) noexcept;
    Renderer &operator=(Renderer &&) noexcept;
    Renderer(Renderer const &) = delete;
    Renderer &operator=(Renderer const &) = delete;

    void SetSamplerMode(SamplerMode mode) noexcept;
    void SetMipVisualization(bool enabled) noexcept;
    void SetLodBias(float bias) noexcept;

    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context) override;
    [[nodiscard]] lgp::framework::Status OnResize(lgp::framework::DeviceResources &deviceResources,
                                                  lgp::framework::Extent2D drawableSize) override;
    [[nodiscard]] lgp::framework::Status Update(lgp::framework::UpdateContext const &context) override;
    [[nodiscard]] lgp::framework::Status BuildUi(lgp::framework::UpdateContext const &context) override;
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &frameContext) override;
    void Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept override;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ch03::texture::solution
