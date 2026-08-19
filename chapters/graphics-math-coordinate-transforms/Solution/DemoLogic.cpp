#include "DemoLogic.hpp"

#include "Scene.hpp"

#include <filesystem>

namespace ch01::graphics_math::solution
{
namespace
{

class SolutionLogic final : public IChapterLogic
{
  public:
    std::wstring_view VariantName() const noexcept override
    {
        return L"Solution";
    }

    bool IsStarterVariant() const noexcept override
    {
        return false;
    }

    void InitializeControls(ChapterControls &controls) const noexcept override
    {
        controls = ChapterControls{};
        controls.orientationMode = OrientationMode::Euler;
        controls.showComparisonOrientation = true;
        controls.showWrongOrder = true;
        controls.showFrustum = true;
        controls.showNormalVectors = true;
        controls.demonstrateIncorrectNormal = true;
        controls.omitPerspectiveDivide = true;
    }

    std::filesystem::path ShaderPath() const override
    {
        return std::filesystem::path{__FILE__}.replace_filename(L"Visualizer.hlsl");
    }

    lgp::framework::Result<ChapterDerivedScene> BuildScene(ChapterControls const &controls,
                                                           lgp::framework::Extent2D viewport,
                                                           double elapsedSeconds) const override
    {
        return solution::BuildScene(controls, viewport, elapsedSeconds);
    }
};

} // namespace

std::unique_ptr<IChapterLogic> CreateLogic()
{
    return std::make_unique<SolutionLogic>();
}

} // namespace ch01::graphics_math::solution
