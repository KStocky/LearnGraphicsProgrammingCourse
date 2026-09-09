// Chapter 30 Starter: the honest output path and nothing else.
//
// The scene pass publishes a scene-linear, temporally resolved display-extent baseline together with the linear
// view depth and the Chapter 11 motion convention that the camera effects would need. The output pass then does
// exactly one job: remove the pre-exposure, apply the camera exposure once, tone map, encode for the display, and
// composite the UI in the domain the policy chose. There is no bloom, no motion blur, and no depth of field here,
// and none of the helpers this file calls performs one.
#include "../Common/PostProcessingShared.hlsli"

[numthreads(8, 8, 1)] void SceneCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= DisplayWidth || dispatchThreadId.y >= DisplayHeight)
    {
        return;
    }
    WriteSceneRecord(dispatchThreadId.xy);
}

[numthreads(8, 8, 1)] void ComposeCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= DisplayWidth || dispatchThreadId.y >= DisplayHeight)
    {
        return;
    }
    uint2 pixel = dispatchThreadId.xy;
    uint index = TexelIndex(uint2(DisplayWidth, DisplayHeight), pixel);
    float3 base = Chain[SourceOffset + index].rgb;
    // The Starter owns no bloom pyramid, so it contributes no bloom radiance rather than approximating one.
    FinishFrame(pixel, base, float3(0.0f, 0.0f, 0.0f));
}
