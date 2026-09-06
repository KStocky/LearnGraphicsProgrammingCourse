#include "../Common/MaterialLab.hlsli"

// Starter shader: the isotropic metal/dielectric baseline. SphereVS/BaselinePS
// shade one directional light with the shared height-correlated base lobe, and
// the fullscreen pass tone maps the linear HDR target to the swap chain. The
// Solution's Baseline preset calls the same ShadeBaselineRadiance so the two
// produce byte-identical baseline pixels.

SurfaceVertex SphereVS(MeshVertex input)
{
    return MaterialLabVertex(input);
}

float4 BaselinePS(SurfaceVertex input) : SV_Target
{
    float3 normal = normalize(input.worldNormal);
    float3 viewDirection = normalize(gLab.cameraPosition - input.worldPosition);
    float3 lightDirection = normalize(gLab.directionToLight);
    float3 radiance = ShadeBaselineRadiance(normal, input.worldTangent.xyz, viewDirection, lightDirection);
    return float4(radiance, 1.0f);
}

FullscreenVertex FullscreenVS(uint vertexId : SV_VertexID)
{
    return FullscreenTriangleVertex(vertexId);
}

float4 DisplayPS(FullscreenVertex input) : SV_Target
{
    float3 hdr = gHdrInput.SampleLevel(gLinearClampSampler, input.uv, 0.0f).rgb;
    return float4(DisplayColor(hdr, gDisplay.exposure, gDisplay.outputView), 1.0f);
}
