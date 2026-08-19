#include "ChapterTypes.hpp"

namespace ch01::graphics_math
{

std::string_view InspectionSpaceName(InspectionSpace space) noexcept
{
    switch (space)
    {
    case InspectionSpace::Object:
        return "Object";
    case InspectionSpace::World:
        return "World";
    case InspectionSpace::View:
        return "View";
    case InspectionSpace::Clip:
        return "Clip";
    case InspectionSpace::Ndc:
        return "NDC";
    case InspectionSpace::Screen:
        return "Screen";
    }

    return "Unknown";
}

std::string_view ProjectionModeName(ProjectionMode mode) noexcept
{
    switch (mode)
    {
    case ProjectionMode::Perspective:
        return "Perspective";
    case ProjectionMode::Orthographic:
        return "Orthographic";
    }

    return "Unknown";
}

std::string_view OrientationModeName(OrientationMode mode) noexcept
{
    switch (mode)
    {
    case OrientationMode::Euler:
        return "Euler";
    case OrientationMode::Quaternion:
        return "Quaternion";
    case OrientationMode::Slerp:
        return "Slerp";
    }

    return "Unknown";
}

} // namespace ch01::graphics_math
