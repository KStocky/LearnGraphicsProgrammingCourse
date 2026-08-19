#pragma once

#include "../Common/ChapterLogic.hpp"

#include <memory>

namespace ch01::graphics_math::starter
{

[[nodiscard]] std::unique_ptr<IChapterLogic> CreateLogic();

} // namespace ch01::graphics_math::starter
