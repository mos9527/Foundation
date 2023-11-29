#include "Common.hlsli"
#define IBL_SAMPLER
#include "IBL.hlsli"
cbuffer Constants : register(b0, space0)
{
    uint u0;
    uint u1;
    uint u2;
    uint u3;
    uint u4;
    uint u5;
    uint u6;
}
SamplerState g_Sampler : register(s0, space0);
// see https://github.com/KhronosGroup/glTF-IBL-Sampler/blob/master/lib/source/shaders/filter.frag
[numthreads(RENDERER_FULLSCREEN_THREADS, RENDERER_FULLSCREEN_THREADS, 6)]
void main_pano2cube(uint3 DTid : SV_DispatchThreadID)
{
    uint dimension = u0, source = u1, cubemap = u2;
    if (any(DTid.xy > uint2(dimension, dimension)))
        return;
    Texture2D<float4> hdriMap = ResourceDescriptorHeap[source];
    RWTexture2DArray<float4> cubemapOut = ResourceDescriptorHeap[cubemap];
    float3 N = normalize(UV2XYZ(DTid.xy / float2(dimension, dimension), DTid.z));
    float2 uv = XYZToPanoUV(N);
    cubemapOut[DTid.xyz] = hdriMap.SampleLevel(g_Sampler, uv, 0);
}
// Ported from https://github.com/KhronosGroup/glTF-IBL-Sampler/blob/master/lib/source/shaders/filter.frag
#define SAMPLE_COUNT 1024
#define SAMPLE_BIAS 0.0f
[numthreads(RENDERER_FULLSCREEN_THREADS, RENDERER_FULLSCREEN_THREADS, 6)]
void main_prefilter(uint3 DTid : SV_DispatchThreadID)
{
    uint cubemapDimension = u0, destDimension = u1, source = u2, dest = u3, cubeIndex = u4, flags = u5, mipIndex = u6;    
    if (any(DTid.xy > uint2(destDimension, destDimension)))
        return;
    TextureCube cubemap = ResourceDescriptorHeap[source];
    RWTexture2DArray<float4> cubemapOut = ResourceDescriptorHeap[dest];    
    float roughness = mipIndex / (log2(cubemapDimension) - 1);

    uint distribution = 0;
    float3 N = normalize(UV2XYZ(DTid.xy / float2(destDimension, destDimension), DTid.z));
    if (flags & IBL_FILTER_FLAG_IRRADIANCE)
    {
        distribution = USE_LAMBERTIAN;
    }
    if (flags & IBL_FILTER_FLAG_RADIANCE)
    {
        if (cubeIndex == IBL_PROBE_SPECULAR_GGX) 
            distribution = USE_GGX;
        if (cubeIndex == IBL_PROBE_SPECULAR_SHEEN)
            distribution = USE_CHARLIE;
    }
    float3 color = splat3(0);
    float weight = 0.0f;

    for (int i = 0; i < SAMPLE_COUNT; i++)
    {
        float4 importanceSample = getImportanceSample(distribution, i, SAMPLE_COUNT, N, max(roughness, EPISILON));
        // NOTE: roughness is irrelavent for Lambertian intergal
        
        float3 H = float3(importanceSample.xyz);
        float pdf = importanceSample.w;

        // mipmap filtered samples (GPU Gems 3, 20.4)
        float lod = computeLod(pdf, cubemapDimension, SAMPLE_COUNT);

        // apply the bias to the lod
        lod += SAMPLE_BIAS;

        if (distribution == USE_LAMBERTIAN)
        {
            // sample lambertian at a lower resolution to avoid fireflies
            float3 lambertian = cubemap.SampleLevel(g_Sampler, H, lod).rgb;            

            //// the below operations cancel each other out
            // lambertian *= NdotH; // lamberts law
            // lambertian /= pdf; // invert bias from importance sampling
            // lambertian /= UX3D_MATH_PI; // convert irradiance to radiance https://seblagarde.wordpress.com/2012/01/08/pi-or-not-to-pi-in-game-lighting-equation/

            color += lambertian;
        }
        else if (distribution == USE_GGX || distribution == USE_CHARLIE)
        {
            // Note: reflect takes incident vector.
            float3 V = N;
            float3 L = normalize(reflect(-V, H));
            float NdotL = dot(N, L);

            if (NdotL > 0.0)
            {
                if (roughness == 0.0)
                {
                    // without this the roughness=0 lod is too high
                    lod = SAMPLE_BIAS;
                }
                float3 sampleColor = cubemap.SampleLevel(g_Sampler, H, lod).rgb;
                color += sampleColor * NdotL;
                weight += NdotL;
            }
        }
    }

    if (weight != 0.0f)
    {
        color /= weight;        
    }
    else
    {
        color /= SAMPLE_COUNT;
    }
    cubemapOut[DTid] = float4(color, 1.0f);
}

[numthreads(RENDERER_FULLSCREEN_THREADS, RENDERER_FULLSCREEN_THREADS, 1)]
void main_lut(uint3 DTid : SV_DispatchThreadID)
{
    uint dimension = u0, lut = u1;
    float2 uv = (DTid.xy + 1) / float2(dimension, dimension);
    
    RWTexture2D<float3> lutOut = ResourceDescriptorHeap[lut];
    float3 result = LUT(USE_GGX, uv.x, uv.y, SAMPLE_COUNT);
    result.b = LUT(USE_CHARLIE, uv.x, uv.y, SAMPLE_COUNT).b;
    lutOut[DTid.xy] = result;
    
}