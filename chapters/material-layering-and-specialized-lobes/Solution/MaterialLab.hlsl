#include "../Common/MaterialLab.hlsli"

// Solution shader: the inspectable material-lobe lab.
//   * SphereVS draws the tangent-framed sphere; ProbeVS fills the viewport with an
//     analytic probe surface whose shading inputs come from the probe constants.
//   * LightingPS shades the Baseline preset through the same ShadeBaselineRadiance
//     as the Starter (byte-identical baseline) and every other preset through the
//     anisotropic layered path that mirrors the CPU contract term for term.
//   * The Final view is exactly base + coat + emission, so the readback tests can
//     prove the contribution split sums to the final before display mapping.

SurfaceVertex SphereVS(MeshVertex input)
{
    return MaterialLabVertex(input);
}

SurfaceVertex ProbeVS(uint vertexId : SV_VertexID)
{
    return ProbeCardVertex(vertexId);
}

float4 LightingPS(SurfaceVertex input) : SV_Target
{
    float3 normal = normalize(input.worldNormal);
    float3 tangentHint = input.worldTangent.xyz;

    float3 viewDirection;
    float3 lightDirection;
    if (gLab.sceneGeometry == SceneGeometryProbe)
    {
        // The probe surface is analytic: shading inputs come from the constants so
        // CPU/GPU parity does not depend on rasterization position.
        viewDirection = normalize(gLab.probeViewDirection);
        lightDirection = normalize(gLab.probeLightDirection);
    }
    else
    {
        viewDirection = normalize(gLab.cameraPosition - input.worldPosition);
        lightDirection = normalize(gLab.directionToLight);
    }

    if (gLab.preset == PresetBaseline)
    {
        float3 baseline = ShadeBaselineRadiance(normal, tangentHint, viewDirection, lightDirection);
        float3 color = baseline;
        if (gLab.outputView == OutputViewCoat || gLab.outputView == OutputViewEmission)
        {
            color = float3(0.0f, 0.0f, 0.0f);
        }
        else if (gLab.outputView == OutputViewAttenuation)
        {
            color = float3(1.0f, 1.0f, 1.0f);
        }
        return float4(color, 1.0f);
    }

    ShadingBasis basis = MakeShadingBasis(normal, tangentHint);
    LayeredResponse response = EvaluateLayered(basis, viewDirection, lightDirection);
    float nDotL = saturate(dot(basis.normal, lightDirection));
    float3 color = SelectLayeredView(response, nDotL, gLab.outputView);
    return float4(color, 1.0f);
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
