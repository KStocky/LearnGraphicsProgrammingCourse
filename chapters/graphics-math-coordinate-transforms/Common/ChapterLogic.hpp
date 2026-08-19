#pragma once

#include "ChapterTypes.hpp"

#include <filesystem>
#include <memory>

#include <lgp/framework/error.hpp>

namespace ch01::graphics_math
{

class IChapterLogic
{
  public:
    virtual ~IChapterLogic() = default;

    virtual std::wstring_view VariantName() const noexcept = 0;
    virtual bool IsStarterVariant() const noexcept = 0;
    virtual void InitializeControls(ChapterControls &controls) const noexcept = 0;
    [[nodiscard]] virtual std::filesystem::path ShaderPath() const = 0;
    [[nodiscard]] virtual lgp::framework::Result<ChapterDerivedScene> BuildScene(ChapterControls const &controls,
                                                                                 lgp::framework::Extent2D viewport,
                                                                                 double elapsedSeconds) const = 0;
};

} // namespace ch01::graphics_math
