#pragma once

#include "ChapterLogic.hpp"

#include <memory>

#include <lgp/framework/application.hpp>

namespace ch01::graphics_math
{

class ChapterRenderer final : public lgp::framework::IChapterRenderer
{
  public:
    explicit ChapterRenderer(std::unique_ptr<IChapterLogic> logic);
    ~ChapterRenderer() override;

    ChapterRenderer(ChapterRenderer &&) noexcept;
    ChapterRenderer &operator=(ChapterRenderer &&) noexcept;
    ChapterRenderer(ChapterRenderer const &) = delete;
    ChapterRenderer &operator=(ChapterRenderer const &) = delete;

    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context) override;
    [[nodiscard]] lgp::framework::Status OnResize(lgp::framework::DeviceResources &deviceResources,
                                                  lgp::framework::Extent2D drawableSize) override;
    [[nodiscard]] lgp::framework::Status Update(lgp::framework::UpdateContext const &context) override;
    [[nodiscard]] lgp::framework::Status BuildUi(lgp::framework::UpdateContext const &context) override;
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &frameContext) override;
    void Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept override;

  private:
    class Impl;

    std::unique_ptr<Impl> impl_{};
};

} // namespace ch01::graphics_math
