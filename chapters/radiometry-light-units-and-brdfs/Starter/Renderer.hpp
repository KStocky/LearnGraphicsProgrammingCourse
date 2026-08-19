#pragma once

#include "../Common/ChapterGeometry.hpp"

#include <lgp/framework/application.hpp>

namespace ch05::lighting::starter
{

class Renderer final : public lgp::framework::IChapterRenderer
{
  public:
    [[nodiscard]] lgp::framework::Status Initialize(lgp::framework::ApplicationInitContext const &context) override;
    [[nodiscard]] lgp::framework::Status OnResize(lgp::framework::DeviceResources &deviceResources,
                                                  lgp::framework::Extent2D drawableSize) override;
    [[nodiscard]] lgp::framework::Status Update(lgp::framework::UpdateContext const &context) override;
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &frameContext) override;
    void Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept override;

  private:
    GeometryMesh sphere_;
    GeometryMesh plane_;
};

} // namespace ch05::lighting::starter
