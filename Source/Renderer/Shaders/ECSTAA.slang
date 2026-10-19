import ICommon;

Texture2D<float4> currentDiffuse;
Texture2D<float4> currentSpecular;
Texture2D<float4> historyDiffuse;
Texture2D<float4> historySpecular;
RWTexture2D<float4> outputDiffuse;
RWTexture2D<float4> outputSpecular;

struct TAAPushConstants
{
    float historyWeight;
    uint historyValid;
    uint neighborhoodClamp;
    uint padding;
};

[[vk::push_constant]] TAAPushConstants pc;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 tid : SV_DispatchThreadID)
{
    uint width, height;
    currentDiffuse.GetDimensions(width, height);
    uint2 extent = uint2(width, height);
    if (tid.x >= extent.x || tid.y >= extent.y)
        return;

    float4 diffuse = currentDiffuse.Load(int3(tid, 0));
    float4 specular = currentSpecular.Load(int3(tid, 0));
    if (pc.historyValid == 0u)
    {
        outputDiffuse[tid] = float4(diffuse.rgb, 1.0f);
        outputSpecular[tid] = float4(specular.rgb, 1.0f);
        return;
    }

    float4 previousDiffuse = historyDiffuse.Load(int3(tid, 0));
    float4 previousSpecular = historySpecular.Load(int3(tid, 0));
    if (pc.neighborhoodClamp != 0u)
    {
        float3 diffuseMin = diffuse.rgb;
        float3 diffuseMax = diffuse.rgb;
        float3 specularMin = specular.rgb;
        float3 specularMax = specular.rgb;
        for (int y = -1; y <= 1; ++y)
        {
            for (int x = -1; x <= 1; ++x)
            {
                int2 samplePixel = clamp(int2(tid) + int2(x, y), int2(0), int2(extent) - 1);
                float3 sampleDiffuse = currentDiffuse.Load(int3(samplePixel, 0)).rgb;
                float3 sampleSpecular = currentSpecular.Load(int3(samplePixel, 0)).rgb;
                diffuseMin = min(diffuseMin, sampleDiffuse);
                diffuseMax = max(diffuseMax, sampleDiffuse);
                specularMin = min(specularMin, sampleSpecular);
                specularMax = max(specularMax, sampleSpecular);
            }
        }
        previousDiffuse.rgb = clamp(previousDiffuse.rgb, diffuseMin, diffuseMax);
        previousSpecular.rgb = clamp(previousSpecular.rgb, specularMin, specularMax);
    }

    float previousSamples = max(previousDiffuse.a, 1.0f);
    float runningAverageWeight = previousSamples / (previousSamples + 1.0f);
    float historyWeight = min(saturate(pc.historyWeight), runningAverageWeight);
    float nextSamples = min(previousSamples + 1.0f, 1024.0f);
    outputDiffuse[tid] = float4(lerp(diffuse.rgb, previousDiffuse.rgb, historyWeight), nextSamples);
    outputSpecular[tid] = float4(lerp(specular.rgb, previousSpecular.rgb, historyWeight), nextSamples);
}
