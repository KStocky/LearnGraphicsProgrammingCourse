#pragma once

#include "../Common/ChapterTypes.hpp"

#include <lgp/framework/application.hpp>

#include <memory>

namespace ch04::color_pipeline::solution
{

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
    [[nodiscard]] lgp::framework::Status BuildUi(lgp::framework::UpdateContext const &context) override;
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &frameContext) override;
    void Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept override;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ch04::color_pipeline::solution
