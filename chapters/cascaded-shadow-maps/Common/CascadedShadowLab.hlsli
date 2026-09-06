#ifndef CH24_CASCADED_SHADOW_LAB_HLSLI
#define CH24_CASCADED_SHADOW_LAB_HLSLI

// Chapter 24 - Cascaded Shadow Maps (shared HLSL core).
//
// This include holds only the pieces that are *not* the cascaded-shadow
// algorithm itself: the constant-buffer layout (which the C++ side asserts
// offset-for-offset), the vertex transforms, and the generic surface shading,
// cascade tint palette, and tone mapping. The algorithm the chapter teaches -
// building the depth array, selecting a cascade by view depth, the upper-edge
// blend band, per-cascade bias scaling, and comparison sampling - lives in the
// Starter and Solution pixel shaders so the lesson can read it directly.
//
// Conventions match Common/CascadedShadowContracts.hpp: row-major matrices with
// row-vector points (p' = p * M), a left-handed camera and light both looking
// along +Z, D3D clip/NDC with z in [0, 1], and shadow UV that flips Y
// (u = x*0.5 + 0.5, v = 0.5 - y*0.5). View depth is the positive distance along
// the camera forward axis, which is exactly what the split boundaries partition.

static const uint DebugViewShaded = 0u;        // Normal shaded scene with shadows.
static const uint DebugViewCascadeColor = 1u;  // Flat per-cascade tint (blended when blend is on).
static const uint DebugViewCascadeShaded = 2u; // Shaded scene tinted by the selected cascade.
static const uint DebugViewShadowFactor = 3u;  // Grayscale shadow visibility only.

static const uint MaxCascades = 4u;

// Field order and 16-byte packing match ch24::cascaded_shadows::gpu::LabConstants
// exactly; the C++ side static_asserts every offset so the layouts never drift.
struct LabConstants
{
    row_major float4x4 cameraViewProjection;
    row_major float4x4 cascadeViewProjection[MaxCascades];

    float4 cascadeSplitViewDepth;      // Far view-depth boundary of each cascade.
    float4 cascadeBlendStartViewDepth; // View depth where each cascade's upper blend band starts.
    float4 cascadeTexelBiasScale;      // Per-cascade bias scale = texel world size / cascade 0 texel world size.

    float3 cameraPosition;
    float exposure;

    float3 cameraForward; // Unit camera +Z axis; view depth = dot(worldPos - cameraPosition, cameraForward).
    float receiverDepthBias;

    float3 directionToLight;
    float normalOffsetWorld;

    float3 lightColor;
    float lightIntensity;

    uint cascadeCount;
    uint debugView;
    uint blendEnabled;
    uint shadowsEnabled;
};

ConstantBuffer<LabConstants> gLab : register(b0);

// Depth-only pass constants: one cascade's light view-projection per draw. Bound
// on register b1 so this shared include can declare it alongside gLab without a
// binding conflict; ShadowVS is the only entry point that references it.
struct ShadowPassConstants
{
    row_major float4x4 lightViewProjection;
};

ConstantBuffer<ShadowPassConstants> gShadowPass : register(b1);

struct MeshVertex
{
    float3 position : POSITION;
    float3 normal : NORMAL;
};

struct ShadowVertex
{
    float4 clipPosition : SV_Position;
};

struct SurfaceVertex
{
    float4 clipPosition : SV_Position;
    float3 worldPosition : POSITION0;
    float3 worldNormal : NORMAL0;
    float viewDepth : POSITION1;
};

static const float ShadowPi = 3.14159265358979323846f;

// Depth-only vertex shader for the shadow passes. Geometry is already in world
// space, so this only applies the cascade's light view-projection.
ShadowVertex ShadowVS(MeshVertex input)
{
    ShadowVertex output;
    output.clipPosition = mul(float4(input.position, 1.0f), gShadowPass.lightViewProjection);
    return output;
}

// Camera vertex shader shared by the Starter and Solution lighting passes.
SurfaceVertex SceneVS(MeshVertex input)
{
    SurfaceVertex output;
    float4 worldPosition = float4(input.position, 1.0f);
    output.clipPosition = mul(worldPosition, gLab.cameraViewProjection);
    output.worldPosition = worldPosition.xyz;
    output.worldNormal = input.normal;
    output.viewDepth = dot(worldPosition.xyz - gLab.cameraPosition, gLab.cameraForward);
    return output;
}

// Deterministic surface albedo: a checkerboard floor (up-facing, near y = 0) and
// a solid tan for every occluder. The checker gives the tone-mapped frame many
// distinct colors so the WARP tests can prove the scene is actually drawn.
float3 SurfaceAlbedo(float3 worldPosition, float3 worldNormal)
{
    bool isFloor = worldNormal.y > 0.9f && abs(worldPosition.y) < 0.05f;
    if (isFloor)
    {
        float checker = fmod(floor(worldPosition.x) + floor(worldPosition.z), 2.0f);
        return checker == 0.0f ? float3(0.72f, 0.72f, 0.74f) : float3(0.30f, 0.31f, 0.34f);
    }
    return float3(0.82f, 0.66f, 0.44f);
}

// Lambertian direct term plus a small ambient so shadowed regions stay readable.
// visibility multiplies only the direct term, which is what a shadow occludes.
float3 ShadeSurface(float3 albedo, float3 worldNormal, float visibility)
{
    float3 normal = normalize(worldNormal);
    float3 lightDirection = normalize(gLab.directionToLight);
    float nDotL = saturate(dot(normal, lightDirection));
    float3 direct = gLab.lightColor * gLab.lightIntensity * nDotL * visibility;
    float3 ambient = 0.12f.xxx;
    return albedo * (ambient + (direct * (1.0f / ShadowPi)));
}

// Fixed cascade tint palette shared by the debug views.
float3 CascadeTint(uint cascade)
{
    if (cascade == 0u)
    {
        return float3(0.90f, 0.22f, 0.20f);
    }
    if (cascade == 1u)
    {
        return float3(0.24f, 0.80f, 0.32f);
    }
    if (cascade == 2u)
    {
        return float3(0.26f, 0.46f, 0.95f);
    }
    return float3(0.92f, 0.86f, 0.22f);
}

float3 AcesFit(float3 color)
{
    float3 numerator = color * ((2.51f * color) + 0.03f);
    float3 denominator = (color * ((2.43f * color) + 0.59f)) + 0.14f;
    return saturate(numerator / denominator);
}

float SrgbEncode(float value)
{
    return value <= 0.0031308f ? value * 12.92f : (1.055f * pow(value, 1.0f / 2.4f)) - 0.055f;
}

float3 SrgbEncodeRgb(float3 color)
{
    return float3(SrgbEncode(color.r), SrgbEncode(color.g), SrgbEncode(color.b));
}

// Tone maps a linear radiance for display. Debug tints are already display-ready
// [0, 1] colors, so callers pass those straight to SrgbEncodeRgb instead.
float3 ToneMapForDisplay(float3 linearColor)
{
    float3 mapped = AcesFit(max(linearColor * exp2(gLab.exposure), 0.0f));
    return saturate(SrgbEncodeRgb(mapped));
}

#endif // CH24_CASCADED_SHADOW_LAB_HLSLI
