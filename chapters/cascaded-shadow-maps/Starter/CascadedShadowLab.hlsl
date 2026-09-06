#include "../Common/CascadedShadowLab.hlsli"

// Chapter 24 Starter shader: a single directional shadow map.
//
// This is the baseline the cascaded Solution improves on. One orthographic light
// view covers the entire camera frustum and writes a single depth texture; the
// lighting pass projects each receiver into that one map and does a hardware
// comparison lookup. Because one map must span near and far at once, its texels
// are coarse - the resolution problem that cascades exist to solve. There is no
// cascade selection, no blend band, and no per-cascade bias here; those arrive in
// the Solution. gLab.cascadeViewProjection[0] holds the single light matrix and
// gLab.cascadeTexelBiasScale[0] is 1.

Texture2D<float> gShadowMap : register(t0);
SamplerComparisonState gShadowCompare : register(s0);

// Projects a receiver into the single shadow map and returns hardware-filtered
// visibility in [0, 1]. Receivers outside the map are fully lit.
float SampleShadowVisibility(float3 worldPosition, float3 worldNormal)
{
    float3 normal = normalize(worldNormal);
    float biasScale = gLab.cascadeTexelBiasScale[0];
    float3 offsetPosition = worldPosition + (normal * gLab.normalOffsetWorld * biasScale);

    float4 clip = mul(float4(offsetPosition, 1.0f), gLab.cascadeViewProjection[0]);
    if (clip.w <= 0.0f)
    {
        return 1.0f;
    }
    float3 ndc = clip.xyz / clip.w;
    float2 uv = float2((ndc.x * 0.5f) + 0.5f, 0.5f - (ndc.y * 0.5f));
    if (any(uv < 0.0f) || any(uv > 1.0f) || ndc.z < 0.0f || ndc.z > 1.0f)
    {
        return 1.0f;
    }
    float comparisonDepth = ndc.z - (gLab.receiverDepthBias * biasScale);
    return gShadowMap.SampleCmpLevelZero(gShadowCompare, uv, comparisonDepth);
}

float4 LightingPS(SurfaceVertex input) : SV_Target
{
    float3 albedo = SurfaceAlbedo(input.worldPosition, input.worldNormal);
    float visibility = gLab.shadowsEnabled != 0u ? SampleShadowVisibility(input.worldPosition, input.worldNormal) : 1.0f;

    if (gLab.debugView == DebugViewShadowFactor)
    {
        return float4(SrgbEncodeRgb(visibility.xxx), 1.0f);
    }

    float3 shaded = ShadeSurface(albedo, input.worldNormal, visibility);

    // The single map has one "cascade", so the cascade-tint debug views simply
    // tint the whole scene with cascade 0's color.
    if (gLab.debugView == DebugViewCascadeColor)
    {
        return float4(SrgbEncodeRgb(CascadeTint(0u)), 1.0f);
    }
    if (gLab.debugView == DebugViewCascadeShaded)
    {
        return float4(ToneMapForDisplay(shaded * CascadeTint(0u)), 1.0f);
    }
    return float4(ToneMapForDisplay(shaded), 1.0f);
}
