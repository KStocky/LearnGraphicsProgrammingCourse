#pragma once

#include "../Common/ChapterTypes.hpp"

#include <lgp/framework/error.hpp>

namespace ch01::graphics_math::starter
{

[[nodiscard]] lgp::framework::Result<ChapterDerivedScene> BuildScene(ChapterControls const &controls,
                                                                     lgp::framework::Extent2D viewport,
                                                                     double elapsedSeconds);

} // namespace ch01::graphics_math::starter
