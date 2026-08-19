#pragma once

#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <lgp/framework/barriers.hpp>

namespace ch08::frame_graph
{

struct TextureResourceId final
{
    std::uint32_t value{(std::numeric_limits<std::uint32_t>::max)()};

    [[nodiscard]] constexpr bool operator==(TextureResourceId const &) const noexcept = default;
};

struct PassId final
{
    std::uint32_t value{(std::numeric_limits<std::uint32_t>::max)()};

    [[nodiscard]] constexpr bool operator==(PassId const &) const noexcept = default;
};

enum class TextureUsageKind : std::uint8_t
{
    Read = 0U,
    Write,
};

enum class DependencyKind : std::uint8_t
{
    ReadAfterWrite = 0U,
    WriteAfterRead,
    WriteAfterWrite,
    Explicit,
};

enum class PassGraphDiagnosticKind : std::uint8_t
{
    InvalidPassId = 0U,
    InvalidTextureResourceId,
    DuplicateSamePassTextureUsage,
    ConflictingSamePassTextureUsage,
    ReadBeforeProduce,
    DuplicateExplicitDependency,
    SelfDependency,
    Cycle,
};

struct TextureResource final
{
    TextureResourceId id{};
    std::string name{};
    bool imported{};
    lgp::framework::TextureBarrierState initialState{};
};

struct CompiledTextureUsage final
{
    TextureResourceId resourceId{};
    TextureUsageKind kind{};
    lgp::framework::TextureBarrierState state{};
};

struct TextureBarrierRecord final
{
    TextureResourceId resourceId{};
    lgp::framework::TextureBarrierState before{};
    lgp::framework::TextureBarrierState after{};
};

struct TextureBarrierGroup final
{
    std::vector<TextureBarrierRecord> records{};
};

struct CompiledPass final
{
    PassId id{};
    std::string name{};
    std::uint32_t executionIndex{};
    std::vector<CompiledTextureUsage> textureUsages{};
    std::vector<TextureBarrierGroup> barrierGroups{};
};

struct DependencyEdge final
{
    PassId beforePassId{};
    PassId afterPassId{};
    std::optional<TextureResourceId> textureResourceId{};
    DependencyKind kind{DependencyKind::Explicit};
};

struct ResourceLifetime final
{
    TextureResourceId resourceId{};
    std::optional<std::uint32_t> firstExecutionIndex{};
    std::optional<std::uint32_t> lastExecutionIndex{};
};

struct CompiledPassGraph final
{
    std::vector<TextureResource> textureResources{};
    std::vector<CompiledPass> scheduledPasses{};
    std::vector<DependencyEdge> dependencyEdges{};
    std::vector<ResourceLifetime> resourceLifetimes{};
};

struct PassGraphDiagnostic final
{
    PassGraphDiagnosticKind kind{};
    std::string message{};
    std::optional<PassId> passId{};
    std::optional<PassId> relatedPassId{};
    std::optional<TextureResourceId> textureResourceId{};
    std::vector<PassId> cyclePassIds{};
    std::vector<std::string> cyclePassNames{};
};

using PassGraphCompileResult = std::expected<CompiledPassGraph, std::vector<PassGraphDiagnostic>>;

[[nodiscard]] lgp::framework::TextureBarrierState TransientTextureInitialState() noexcept;

class PassGraph final
{
  public:
    [[nodiscard]] TextureResourceId AddImportedTexture(std::string_view name,
                                                       lgp::framework::TextureBarrierState initialState);
    [[nodiscard]] TextureResourceId AddTransientTexture(std::string_view name);
    [[nodiscard]] PassId AddPass(std::string_view name);

    void DeclareTextureRead(PassId passId, TextureResourceId resourceId, lgp::framework::TextureBarrierState state);
    void DeclareTextureWrite(PassId passId, TextureResourceId resourceId, lgp::framework::TextureBarrierState state);
    void AddDependency(PassId beforePassId, PassId afterPassId);

    [[nodiscard]] PassGraphCompileResult Compile() const;

  private:
    struct TextureUsageDeclaration final
    {
        PassId passId{};
        TextureResourceId resourceId{};
        TextureUsageKind kind{TextureUsageKind::Read};
        lgp::framework::TextureBarrierState state{};
    };

    struct ExplicitDependencyDeclaration final
    {
        PassId beforePassId{};
        PassId afterPassId{};
    };

    std::vector<TextureResource> textureResources_{};
    std::vector<std::string> passNames_{};
    std::vector<TextureUsageDeclaration> textureUsageDeclarations_{};
    std::vector<ExplicitDependencyDeclaration> explicitDependencyDeclarations_{};
};

} // namespace ch08::frame_graph
