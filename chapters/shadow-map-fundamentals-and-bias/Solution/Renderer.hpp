#pragma once

#include "../Common/ShadowContracts.hpp"

#include <lgp/framework/application.hpp>

#include <cstdint>
#include <memory>

namespace ch07::shadows::solution
{

enum class ShadowVisualization : std::uint32_t
{
    Final = 0U,
    Visibility,
    ShadowDepth,
    ReceiverDepth,
    StoredMinusReceiver,
    ConfiguredBiasEstimate,
    ShadowUv,
    FrustumCoverage,
    Slope,
    FiniteValidation,
    ObjectId,
    PointSampleAgreement,
    ComparisonRelation,
    WorldTexelSize,
    FrustumBoundaryProbe,
};

struct HeadlessTestConfiguration final
{
    Float3 directionToLight{-0.45F, 0.82F, -0.35F};
    float illuminanceLux{1200.0F};
    float exposure{-4.0F};
    std::uint32_t shadowResolution{1024U};
    OrthographicExtents orthographicExtents{-10.0F, 10.0F, -8.0F, 8.0F};
    Float3 lightTarget{0.0F, 0.0F, 0.0F};
    float lightDistance{18.0F};
    DepthRange depthRange{0.1F, 40.0F};
    std::int32_t rasterConstantBias{1200};
    float rasterSlopeBias{1.5F};
    float rasterBiasClamp{0.01F};
    float receiverDepthBias{0.00035F};
    float receiverNormalOffsetWorld{0.02F};
    bool shadowEnabled{true};
    ShadowVisualization visualization{ShadowVisualization::Final};
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

} // namespace ch07::shadows::solution
