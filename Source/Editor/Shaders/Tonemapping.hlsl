#include "Common.hlsli"
#include "Shared.h"
// see https://bruop.github.io/exposure/
//     https://www.alextardif.com/HistogramLuminance.html
cbuffer Constants : register(b0, space0)
{
    uint width;    
    uint height;
    uint histogramUAV;
    uint framebufferUAV;
    uint avgLuminaceUAV;
}
ConstantBuffer<SceneGlobals> g_SceneGlobals : register(b1, space0);
float RGB2Luminance(float3 color)
{
    return dot(color, float3(0.2127f, 0.7152f, 0.0722f));
}
uint RGB2Bin(float3 rgb, float minLogL, float rangeLogL)
{
    float L = RGB2Luminance(rgb);
    if (L < 0.005f)
        return 0;
    float logL = saturate((log2(L) - minLogL) / rangeLogL);
    return (uint) (logL * 254 + 1.0f); // bin 0 stores OOB luminance (i.e. comepletely black) pixels
}

groupshared uint HistogramShared[RENDERER_TONEMAPPING_THREADS * RENDERER_TONEMAPPING_THREADS];

[numthreads(RENDERER_TONEMAPPING_THREADS, RENDERER_TONEMAPPING_THREADS, 1)]
void main_histogram(uint2 DTid : SV_DispatchThreadID, uint gid : SV_GroupIndex)
{
    HistogramShared[gid] = 0;
    GroupMemoryBarrierWithGroupSync();
    if (any(DTid > g_SceneGlobals.frameDimension))
        return;
    Texture2D<float4> frameBuffer = ResourceDescriptorHeap[framebufferUAV];
    float4 color = frameBuffer[DTid];
    uint bin = RGB2Bin(color.rgb, g_SceneGlobals.camera.logLuminaceMin, g_SceneGlobals.camera.logLuminanceRange);
    InterlockedAdd(HistogramShared[bin], 1);
    GroupMemoryBarrierWithGroupSync(); // Finish group reads
    RWByteAddressBuffer histogram = ResourceDescriptorHeap[histogramUAV];
    histogram.InterlockedAdd(gid * 4, HistogramShared[gid]);
}

// Dispatch(1,1,1)
[numthreads(RENDERER_TONEMAPPING_THREADS, RENDERER_TONEMAPPING_THREADS, 1)]
void main_avg(uint2 DTid : SV_DispatchThreadID, uint gid : SV_GroupIndex)
{
    RWByteAddressBuffer histogram = ResourceDescriptorHeap[histogramUAV];
    uint count = histogram.Load(gid * 4);
    HistogramShared[gid] = count * gid;
    
    GroupMemoryBarrierWithGroupSync();
    histogram.Store(gid * 4, 0); // chance to clear!
    // Parallel sum average
    [unroll]
    for (uint histogramSampleIndex = ((RENDERER_TONEMAPPING_THREADS * RENDERER_TONEMAPPING_THREADS) >> 1); histogramSampleIndex > 0; histogramSampleIndex >>= 1)
    {
        if (gid < histogramSampleIndex)
        {
            HistogramShared[gid] += HistogramShared[gid + histogramSampleIndex];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    // Store average
    if (gid == 0)
    {
        float avgLogL = (float) HistogramShared[0] / max(width * height - count, 1) - 1; // count[gid=0] is for L=0 pixels
        float avgL = exp2((avgLogL / 254.0f) * g_SceneGlobals.camera.logLuminanceRange + g_SceneGlobals.camera.logLuminaceMin);
        RWTexture2D<float> avgBuffer = ResourceDescriptorHeap[avgLuminaceUAV];
        float avgLlastFrame = avgBuffer[uint2(0, 0)];
        if (g_SceneGlobals.camera.luminaceAdaptRate <= EPSILON || isnan(avgLlastFrame) || isinf(avgLlastFrame) || avgLlastFrame == 0)
        {
            avgBuffer[uint2(0, 0)] = avgL;
            return;
        }
        float adaptedL = avgLlastFrame + (avgL - avgLlastFrame) * (1 - exp(-g_SceneGlobals.frameTimePrev * g_SceneGlobals.camera.luminaceAdaptRate));;
        avgBuffer[uint2(0, 0)] = adaptedL;
    }
}
// taken from https://github.com/BruOp/bae/blob/master/examples/common/shaderlib.sh
float3 convertXYZ2Yxy(float3 _xyz)
{
	// Reference:
	// http://www.brucelindbloom.com/index.html?Eqn_XYZ_to_xyY.html
    float inv = 1.0 / dot(_xyz, float3(1.0, 1.0, 1.0));
    return float3(_xyz.y, _xyz.x * inv, _xyz.y * inv);
}
float3 convertRGB2XYZ(float3 _rgb)
{
	// Reference:
	// RGB/XYZ Matrices
	// http://www.brucelindbloom.com/index.html?Eqn_RGB_XYZ_Matrix.html
    float3 xyz;
    xyz.x = dot(float3(0.4124564, 0.3575761, 0.1804375), _rgb);
    xyz.y = dot(float3(0.2126729, 0.7151522, 0.0721750), _rgb);
    xyz.z = dot(float3(0.0193339, 0.1191920, 0.9503041), _rgb);
    return xyz;
}
float3 convertYxy2XYZ(float3 _Yxy)
{
	// Reference:
	// http://www.brucelindbloom.com/index.html?Eqn_xyY_to_XYZ.html
    float3 xyz;
    xyz.x = _Yxy.x * _Yxy.y / _Yxy.z;
    xyz.y = _Yxy.x;
    xyz.z = _Yxy.x * (1.0 - _Yxy.y - _Yxy.z) / _Yxy.z;
    return xyz;
}
float3 convertXYZ2RGB(float3 _xyz)
{
    float3 rgb;
    rgb.x = dot(float3(3.2404542, -1.5371385, -0.4985314), _xyz);
    rgb.y = dot(float3(-0.9692660, 1.8760108, 0.0415560), _xyz);
    rgb.z = dot(float3(0.0556434, -0.2040259, 1.0572252), _xyz);
    return rgb;
}
float3 convertRGB2Yxy(float3 _rgb)
{
    return convertXYZ2Yxy(convertRGB2XYZ(_rgb));
}
float3 convertYxy2RGB(float3 _Yxy)
{
    return convertXYZ2RGB(convertYxy2XYZ(_Yxy));
}
float Tonemap_ACES(float x)
{
    // Narkowicz 2015, "ACES Filmic Tone Mapping Curve"
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return (x * (a * x + b)) / (x * (c * x + d) + e);
}
[numthreads(RENDERER_FULLSCREEN_THREADS, RENDERER_FULLSCREEN_THREADS, 6)]
void main_tonemap(uint2 DTid : SV_DispatchThreadID)
{
    if (any(DTid > g_SceneGlobals.frameDimension))
        return;
    RWTexture2D<float4> frameBuffer = ResourceDescriptorHeap[framebufferUAV];
    Texture2D avgBuffer = ResourceDescriptorHeap[avgLuminaceUAV];
    float3 rgb = frameBuffer[DTid].rgb;    
    float lum = avgBuffer[uint2(0, 0)].r;
    // Y -> luminance
    float3 Yxy = convertRGB2Yxy(rgb);
    // https://bruop.github.io/exposure/
    // TL;DR
    // EV100 = log2(Lavg * S/K)
    // Lmax = 78/qS * exp(2,EV100) = 78/qS * (Lavg * S/K) = Lavg * 78/q/K
    // take q=0.65, S=100 (canceled), K=12.5
    // you'd get Lmax = 9.6 * Lavg
    // thus L/Lmax -> mapping luminance to [0,1] range
    Yxy.x = Yxy.x / (9.6 * lum + EPSILON);
    rgb = convertYxy2RGB(Yxy);
    rgb.r = Tonemap_ACES(rgb.r);
    rgb.g = Tonemap_ACES(rgb.g);
    rgb.b = Tonemap_ACES(rgb.b);
    frameBuffer[DTid] = float4(pow(rgb, 1/2.2), 1.0f); // to sRGB space
}