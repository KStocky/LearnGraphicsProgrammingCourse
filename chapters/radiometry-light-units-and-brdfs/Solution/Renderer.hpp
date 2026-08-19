#pragma once

#include "../Common/ChapterTypes.hpp"

#include <lgp/framework/application.hpp>

#include <cstdint>
#include <memory>

namespace ch05::lighting::solution
{

enum class HeadlessLightType : std::uint32_t
{
    Directional = 0U,
    Point = 1U,
};

enum class HeadlessVisualization : std::uint32_t
{
    Final = 0U,
    Irradiance = 1U,
    Diffuse = 3U,
    Specular = 4U,
};

struct HeadlessTestConfiguration final
{
    MaterialParameters material{{0.72F, 0.18F, 0.08F}, 0.45F, 0.0F};
    HeadlessLightType lightType{HeadlessLightType::Directional};
    HeadlessVisualization visualization{HeadlessVisualization::Final};
    Float3 directionToLight{-0.45F, 0.82F, -0.35F};
    Float3 pointLightPosition{0.0F, 7.0F, 0.5F};
    LinearRgb lightColor{1.0F, 1.0F, 1.0F};
    float intensity{1200.0F};
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
    [[nodiscard]] lgp::framework::Status BuildUi(lgp::framework::UpdateContext const &context) override;
    [[nodiscard]] lgp::framework::Status Render(lgp::framework::FrameContext const &frameContext) override;
    void Shutdown(lgp::framework::DeviceResources &deviceResources) noexcept override;
    void ConfigureHeadlessTest(HeadlessTestConfiguration const &configuration) noexcept;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ch05::lighting::solution
