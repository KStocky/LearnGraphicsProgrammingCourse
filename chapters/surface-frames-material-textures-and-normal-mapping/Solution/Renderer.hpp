#pragma once

#include <lgp/framework/application.hpp>

#include <cstdint>
#include <memory>

namespace ch06::surface_frames::solution
{

enum class HeadlessVisualization : std::uint32_t
{
    Final = 0U,
    Uv = 1U,
    GeometricNormal = 2U,
    InterpolatedNormal = 3U,
    MappedNormal = 4U,
    Tangent = 5U,
    Bitangent = 6U,
    Handedness = 7U,
    BaseColor = 8U,
    Roughness = 9U,
    Metalness = 10U,
    Diffuse = 11U,
    Specular = 12U,
    ObjectId = 13U,
    FiniteValidation = 14U,
};

enum class HeadlessScene : std::uint32_t
{
    Full = 0U,
    GroundOnly = 1U,
    CenterSphereOnly = 2U,
};

struct HeadlessTestConfiguration final
{
    HeadlessVisualization visualization{HeadlessVisualization::Final};
    HeadlessScene scene{HeadlessScene::Full};
    bool useBaseColorTexture{true};
    bool usePackedMaterialTexture{true};
    bool useNormalTexture{true};
    bool invertNormalGreen{false};
    bool sampleBaseColorAsLinear{false};
    bool overrideMaterial{false};
    bool overrideNormalSample{false};
    float normalStrength{1.0F};
    float normalSampleR{0.5F};
    float normalSampleG{0.5F};
    float normalSampleB{1.0F};
    float baseColorR{0.72F};
    float baseColorG{0.18F};
    float baseColorB{0.08F};
    float roughness{0.45F};
    float metalness{};
    float directionToLightX{-0.45F};
    float directionToLightY{0.82F};
    float directionToLightZ{-0.35F};
    float lightIntensity{1200.0F};
    float cameraAzimuth{};
    float cameraElevation{0.28F};
    float cameraDistance{15.5F};
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

} // namespace ch06::surface_frames::solution
