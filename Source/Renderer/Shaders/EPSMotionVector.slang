// Fragment shader for the dedicated motion-vector pass. Converts current/previous clip
// positions (same camera) to UV and writes currentUV - previousUV. No material sampling.
#include "ICommon.slang"

public struct MotionV2F
{
    public float4 pos : SV_POSITION;
    public float4 currClip : CURRCLIP;
    public float4 prevClip : PREVCLIP;
};

struct Fragment
{
    float2 motion : SV_Target0;
};

[shader("fragment")]
Fragment main(MotionV2F v2f)
{
    float2 currNDC = v2f.currClip.xy / max(abs(v2f.currClip.w), 1e-8f);
    float2 prevNDC = v2f.prevClip.xy / max(abs(v2f.prevClip.w), 1e-8f);
    // Match GTAO / inverseViewProj convention: y flips between NDC and UV.
    float2 currUV = float2(currNDC.x * 0.5f + 0.5f, 0.5f - currNDC.y * 0.5f);
    float2 prevUV = float2(prevNDC.x * 0.5f + 0.5f, 0.5f - prevNDC.y * 0.5f);
    Fragment f;
    f.motion = currUV - prevUV;
    return f;
}
