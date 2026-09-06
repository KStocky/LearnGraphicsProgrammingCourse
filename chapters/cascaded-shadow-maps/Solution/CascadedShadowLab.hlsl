#include "../Common/CascadedShadowLab.hlsli"

// Chapter 24 Solution shader: four cascaded shadow maps.
//
// The four depth-only passes each write one slice of a Texture2DArray. This
// lighting pass does the work the chapter teaches, all visible here:
//   * SelectCascade picks the cascade for a receiver from its view depth, using
//     the split boundaries the CPU contract computed. A blend band at the upper
//     edge of each cascade cross-fades into the next so the resolution seam is
//     hidden instead of popping.
//   * SampleCascadeVisibility projects into the chosen slice, applies the
//     per-cascade bias (scaled by that cascade's texel world size), and does a
//     hardware comparison lookup.
// Compare this with the Starter's single map: the near cascade now has far finer
// texels than one map spanning the whole frustum could afford.

Texture2DArray<float> gShadowArray : register(t0);
SamplerComparisonState gShadowCompare : register(s0);

struct CascadeSelection
{
    uint primary;
    uint secondary;
    float blendWeight; // Weight of the secondary cascade in [0, 1).
};

// Selects the cascade(s) for a positive view-space depth. The transition band
// sits at the upper end of each cascade interval only, so adjacent cascades never
// blend twice and never leave a gap - matching ch24::cascaded_shadows::SelectCascade.
CascadeSelection SelectCascade(float viewDepth)
{
    uint count = gLab.cascadeCount;
    uint primary = count - 1u;
    for (uint i = 0u; i < count; ++i)
    {
        if (viewDepth < gLab.cascadeSplitViewDepth[i])
        {
            primary = i;
            break;
        }
    }

    CascadeSelection selection;
    selection.primary = primary;
    selection.secondary = primary;
    selection.blendWeight = 0.0f;

    if (gLab.blendEnabled != 0u && (primary + 1u) < count)
    {
        float blendStart = gLab.cascadeBlendStartViewDepth[primary];
        float splitFar = gLab.cascadeSplitViewDepth[primary];
        if (viewDepth > blendStart && splitFar > blendStart)
        {
            selection.secondary = primary + 1u;
            selection.blendWeight = saturate((viewDepth - blendStart) / (splitFar - blendStart));
        }
    }
    return selection;
}

// Projects a receiver into one cascade slice and returns hardware-filtered
// visibility. Receivers outside the slice are fully lit.
float SampleCascadeVisibility(uint cascade, float3 worldPosition, float3 worldNormal)
{
    float3 normal = normalize(worldNormal);
    float biasScale = gLab.cascadeTexelBiasScale[cascade];
    float3 offsetPosition = worldPosition + (normal * gLab.normalOffsetWorld * biasScale);

    float4 clip = mul(float4(offsetPosition, 1.0f), gLab.cascadeViewProjection[cascade]);
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
    return gShadowArray.SampleCmpLevelZero(gShadowCompare, float3(uv, (float)cascade), comparisonDepth);
}

float ResolveVisibility(SurfaceVertex input, CascadeSelection selection)
{
    if (gLab.shadowsEnabled == 0u)
    {
        return 1.0f;
    }
    float primary = SampleCascadeVisibility(selection.primary, input.worldPosition, input.worldNormal);
    if (selection.blendWeight <= 0.0f)
    {
        return primary;
    }
    float secondary = SampleCascadeVisibility(selection.secondary, input.worldPosition, input.worldNormal);
    return lerp(primary, secondary, selection.blendWeight);
}

float4 LightingPS(SurfaceVertex input) : SV_Target
{
    CascadeSelection selection = SelectCascade(input.viewDepth);
    float visibility = ResolveVisibility(input, selection);

    if (gLab.debugView == DebugViewShadowFactor)
    {
        return float4(SrgbEncodeRgb(visibility.xxx), 1.0f);
    }

    float3 tint = CascadeTint(selection.primary);
    if (selection.blendWeight > 0.0f)
    {
        tint = lerp(tint, CascadeTint(selection.secondary), selection.blendWeight);
    }

    if (gLab.debugView == DebugViewCascadeColor)
    {
        return float4(SrgbEncodeRgb(tint), 1.0f);
    }

    float3 albedo = SurfaceAlbedo(input.worldPosition, input.worldNormal);
    float3 shaded = ShadeSurface(albedo, input.worldNormal, visibility);
    if (gLab.debugView == DebugViewCascadeShaded)
    {
        return float4(ToneMapForDisplay(shaded * tint), 1.0f);
    }
    return float4(ToneMapForDisplay(shaded), 1.0f);
}
