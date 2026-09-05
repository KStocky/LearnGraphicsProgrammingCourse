#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace ch22::meshlets
{

// Errors are split so that callers can distinguish malformed meshlet payloads
// (structural violations that no valid builder should ever emit) from a builder
// that produced structurally valid data whose reconstruction disagrees with the
// original index stream.
enum class MeshError : std::uint8_t
{
    // Mesh input validation.
    EmptyPositions = 0U,
    EmptyIndices,
    NonFinitePosition,
    IndexCountNotTriangleList,
    IndexOutOfRange,
    DegenerateTriangle,
    // Limit validation.
    MaxVerticesTooSmall,
    MaxVerticesTooLarge,
    MaxPrimitivesTooSmall,
    MaxPrimitivesTooLarge,
    // Structural meshlet validation (malformed data).
    EmptyMeshletSet,
    EmptyMeshlet,
    VertexLimitExceeded,
    PrimitiveLimitExceeded,
    LocalIndexOutOfRange,
    GlobalIndexOutOfRange,
    DuplicateGlobalVertex,
    StatisticsMismatch,
    // Reconstruction validation (structurally valid but semantically wrong).
    ReconstructionCountMismatch,
    ReconstructionIndexMismatch,
    // Derived-quantity computation.
    ArithmeticOverflow,
};

// D3D12 mesh-shader hard limits. A meshlet may address at most 256 vertices and
// emit at most 256 primitives. A 16-bit local index carries generous headroom
// over the largest legal local index (255) and keeps later format experiments
// from coupling the in-memory contract to an exactly-full 8-bit range.
inline constexpr std::uint32_t kMaxMeshletVertices = 256U;
inline constexpr std::uint32_t kMaxMeshletPrimitives = 256U;

using GlobalIndex = std::uint32_t;
using LocalIndex = std::uint16_t;

struct Float3 final
{
    float x{};
    float y{};
    float z{};

    [[nodiscard]] bool operator==(Float3 const &) const noexcept = default;
};

struct MeshletLimits final
{
    std::uint32_t maxVertices{};
    std::uint32_t maxPrimitives{};

    [[nodiscard]] bool operator==(MeshletLimits const &) const noexcept = default;
};

// A single triangle expressed with meshlet-local vertex indices. Each component
// indexes the owning meshlet's vertexRemap table.
struct PrimitiveTriangle final
{
    LocalIndex a{};
    LocalIndex b{};
    LocalIndex c{};

    [[nodiscard]] bool operator==(PrimitiveTriangle const &) const noexcept = default;
};

// vertexRemap maps a meshlet-local vertex slot to the source mesh's global vertex
// index; primitives reference those local slots. First-use ordering is preserved:
// vertexRemap[k] is the (k+1)-th distinct global vertex encountered while walking
// the source triangles that belong to this meshlet.
struct Meshlet final
{
    std::vector<GlobalIndex> vertexRemap{};
    std::vector<PrimitiveTriangle> primitives{};

    [[nodiscard]] bool operator==(Meshlet const &) const noexcept = default;
};

struct BuildStatistics final
{
    std::uint32_t meshletCount{};
    std::uint32_t primitiveCount{};
    // Total vertexRemap entries summed across meshlets. Vertices are unique within
    // a meshlet but a shared vertex is re-listed once per meshlet that touches it.
    std::uint32_t vertexReferenceCount{};
    // Distinct source vertices referenced by at least one meshlet.
    std::uint32_t referencedVertexCount{};
    // vertexReferenceCount - referencedVertexCount: the storage overhead paid for
    // duplicating boundary vertices across meshlets, useful when comparing the
    // classic index-buffer pipeline against a mesh-shader pipeline.
    std::uint32_t duplicatedVertexReferences{};

    [[nodiscard]] bool operator==(BuildStatistics const &) const noexcept = default;
};

struct MeshletBuild final
{
    std::vector<Meshlet> meshlets{};
    BuildStatistics statistics{};

    [[nodiscard]] bool operator==(MeshletBuild const &) const noexcept = default;
};

// A conservative bounding volume; not claimed to be the minimal enclosing sphere.
struct BoundingSphere final
{
    Float3 center{};
    float radius{};

    [[nodiscard]] bool operator==(BoundingSphere const &) const noexcept = default;
};

// A conservative backface-culling cone. When valid is false the cone is disabled
// and must not be used to cull: the meshlet's face normals span at least a
// hemisphere (or otherwise cannot yield a safe axis) so no orientation is safe to
// reject. cosHalfAngle is the cosine of the cone half-angle; for a valid cone it
// is strictly positive and every source face normal lies within the cone after
// normalizing the stored float axis.
struct NormalCone final
{
    Float3 axis{};
    float cosHalfAngle{};
    bool valid{};

    [[nodiscard]] bool operator==(NormalCone const &) const noexcept = default;
};

// Validates that positions are non-empty and finite, that indices form a non-empty
// triangle list whose entries are in range, and that no triangle is degenerate.
[[nodiscard]] std::expected<void, MeshError> ValidateMesh(std::span<Float3 const> positions,
                                                          std::span<GlobalIndex const> indices) noexcept;

// Validates useful lower bounds (a triangle needs three vertices and one primitive)
// and the D3D12 upper bounds.
[[nodiscard]] std::expected<void, MeshError> ValidateLimits(MeshletLimits limits) noexcept;

// Deterministic greedy partition in source-triangle order. A triangle joins the
// current meshlet iff its newly required distinct vertices fit within maxVertices
// and one more primitive fits within maxPrimitives; otherwise the current meshlet
// is finished and a fresh one begins. Ordering is never changed for optimization.
[[nodiscard]] std::expected<MeshletBuild, MeshError> BuildMeshlets(std::span<Float3 const> positions,
                                                                   std::span<GlobalIndex const> indices,
                                                                   MeshletLimits limits);

// Reconstructs the global triangle-list index stream from a meshlet build by
// expanding each local primitive through its meshlet's vertexRemap table.
[[nodiscard]] std::expected<std::vector<GlobalIndex>, MeshError> ReconstructIndices(MeshletBuild const &build);

// Proves the build is structurally sound and that reconstruction reproduces the
// original index stream exactly. Structural failures are reported before any
// reconstruction comparison so malformed data is never misreported as a mismatch.
[[nodiscard]] std::expected<void, MeshError> ValidateMeshletBuild(MeshletBuild const &build,
                                                                  std::span<Float3 const> positions,
                                                                  std::span<GlobalIndex const> indices,
                                                                  MeshletLimits limits);

// Conservative centroid-of-referenced-vertices sphere: the center is the mean of
// the meshlet's referenced positions and the radius is the maximum distance from
// that center to any referenced position.
[[nodiscard]] std::expected<BoundingSphere, MeshError> ComputeCentroidBoundingSphere(Meshlet const &meshlet,
                                                                                     std::span<Float3 const> positions);

// Conservative backface-culling cone from the meshlet's source face normals.
// Returns a disabled cone whenever a safe cone cannot be guaranteed.
[[nodiscard]] std::expected<NormalCone, MeshError> ComputeNormalCone(Meshlet const &meshlet,
                                                                     std::span<Float3 const> positions);

} // namespace ch22::meshlets
