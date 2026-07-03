import ICommon;
import IMath;

uniform UBO globalParams;
Texture2D<float> halfAO;
Texture2D<float> depth;
RWTexture2D<float> aoOutput;

float WeightForDepth(float centerDepth, float sampleDepth)
{
    if (sampleDepth <= EPS)
        return 0.0f;
    return 1.0f / (1.0f + abs(centerDepth - sampleDepth) * 96.0f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 tid : SV_DispatchThreadID)
{
    uint2 fullExtent = uint2(uint(globalParams.fbWidth), uint(globalParams.fbHeight));
    if (tid.x >= fullExtent.x || tid.y >= fullExtent.y)
        return;

    uint2 halfExtent = (fullExtent + 1u) / 2u;
    uint2 basePixel = tid / 2u;
    float centerDepth = depth.Load(uint3(tid, 0));
    if (centerDepth <= EPS)
    {
        aoOutput[tid] = 1.0f;
        return;
    }

    float sumAO = 0.0f;
    float sumWeight = 0.0f;
    for (int y = -1; y <= 1; y++)
    {
        for (int x = -1; x <= 1; x++)
        {
            int2 halfPixel = clamp(int2(basePixel) + int2(x, y), int2(0), int2(halfExtent) - 1);
            uint2 fullSample = min(uint2(halfPixel) * 2u, fullExtent - 1u);
            float sampleDepth = depth.Load(uint3(fullSample, 0));
            float weight = WeightForDepth(centerDepth, sampleDepth);
            sumAO += halfAO.Load(uint3(uint2(halfPixel), 0)) * weight;
            sumWeight += weight;
        }
    }

    aoOutput[tid] = sumWeight > 0.0f ? saturate(sumAO / sumWeight) : 1.0f;
}
