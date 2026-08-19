#pragma once

#include "../Common/ShadowContracts.hpp"

#include <lgp/framework/application.hpp>

#include <cstdint>
#include <memory>

namespace ch07::shadows::starter
{

struct HeadlessTestConfiguration final
{
    Float3 directionToLight{-0.45F, 0.82F, -0.35F};
    float illuminanceLux{1200.0F};
    float exposure{-4.0F};
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

    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context) override;
    [[nodiscard]] lgp::framework::Status OnResize(lgp::framework::DeviceResources &deviceResources,
                                                  lgp::framework::Extent2D drawableSize) override;
    [[nodiscard]] lgp::framework::Status Update(lgp::framework::UpdateContext const &context) override;
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &frameContext) override;
    void Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept override;
    void ConfigureHeadlessTest(HeadlessTestConfiguration const &configuration) noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ch07::shadows::starter
