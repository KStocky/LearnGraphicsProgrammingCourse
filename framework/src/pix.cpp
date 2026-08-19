#include <lgp/framework/pix.hpp>

#include <pix3.h>

#include <utility>

namespace lgp::framework
{

PixEventScope::PixEventScope(ID3D12CommandQueue &queue, std::uint64_t color, wchar_t const *label) noexcept
    : queue_{&queue}
{
    PIXBeginEvent(queue_, color, label);
}

PixEventScope::PixEventScope(ID3D12GraphicsCommandList &commandList, std::uint64_t color, wchar_t const *label) noexcept
    : commandList_{&commandList}
{
    PIXBeginEvent(commandList_, color, label);
}

PixEventScope::PixEventScope(PixEventScope &&other) noexcept
    : queue_{std::exchange(other.queue_, nullptr)}, commandList_{std::exchange(other.commandList_, nullptr)}
{
}

PixEventScope &PixEventScope::operator=(PixEventScope &&other) noexcept
{
    if (this != &other)
    {
        End();
        queue_ = std::exchange(other.queue_, nullptr);
        commandList_ = std::exchange(other.commandList_, nullptr);
    }

    return *this;
}

PixEventScope::~PixEventScope()
{
    End();
}

void PixEventScope::End() noexcept
{
    if (queue_ != nullptr)
    {
        PIXEndEvent(queue_);
        queue_ = nullptr;
    }

    if (commandList_ != nullptr)
    {
        PIXEndEvent(commandList_);
        commandList_ = nullptr;
    }
}

void SetPixMarker(ID3D12CommandQueue &queue, std::uint64_t color, wchar_t const *label) noexcept
{
    PIXSetMarker(&queue, color, label);
}

void SetPixMarker(ID3D12GraphicsCommandList &commandList, std::uint64_t color, wchar_t const *label) noexcept
{
    PIXSetMarker(&commandList, color, label);
}

} // namespace lgp::framework
