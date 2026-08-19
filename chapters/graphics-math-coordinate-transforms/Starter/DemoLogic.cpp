#include "DemoLogic.hpp"

#include "Scene.hpp"

#include <filesystem>

namespace ch01::graphics_math::starter
{
namespace
{

class StarterLogic final : public IChapterLogic
{
  public:
    std::wstring_view VariantName() const noexcept override
    {
        return L"Starter";
    }

    bool IsStarterVariant() const noexcept override
    {
        return true;
    }

    void InitializeControls(ChapterControls &controls) const noexcept override
    {
        controls = ChapterControls{};
        controls.showObjectAxes = true;
        controls.showWorldAxes = true;
        controls.showCameraAxes = false;
        controls.showFrustum = false;
        controls.showWrongOrder = false;
        controls.showComparisonOrientation = false;
        controls.showNormalVectors = false;
        controls.demonstrateIncorrectNormal = false;
        controls.omitPerspectiveDivide = false;
    }

    std::filesystem::path ShaderPath() const override
    {
        return std::filesystem::path{__FILE__}.replace_filename(L"Visualizer.hlsl");
    }

    lgp::framework::Result<ChapterDerivedScene> BuildScene(ChapterControls const &controls,
                                                           lgp::framework::Extent2D viewport,
                                                           double elapsedSeconds) const override
    {
        return starter::BuildScene(controls, viewport, elapsedSeconds);
    }
};

} // namespace

std::unique_ptr<IChapterLogic> CreateLogic()
{
    return std::make_unique<StarterLogic>();
}

} // namespace ch01::graphics_math::starter
