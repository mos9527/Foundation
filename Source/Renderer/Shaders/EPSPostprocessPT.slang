import IFlags;
import IBindless3D;
import ICommon;
import IPostprocess;

// Buffers (Welford online mean - already averaged)
Texture2D<float4> diffuseTex;
Texture2D<float4> specularTex;
SamplerState lutSampler;
uniform PostprocessUBO globalParams;


float3 LoadPTPostprocessRadiance(int3 coord)
{
    if ((globalParams.dbgViewFlags & uint32_t(ViewFlagsBits.AOVSampleCount)) != 0)
    {
        float samples = diffuseTex.Load(coord).a;
        float maxSamples = float(globalParams.ptAccumulatedFrames);
        float ratio = maxSamples > 0.0f ? saturate(samples / maxSamples) : 0.0f;
        // Simple heatmap: Blue (0) -> Green (0.5) -> Red (1.0)
        return float3(
            ratio > 0.5f ? (ratio - 0.5f) * 2.0f : 0.0f,
            ratio <= 0.5f ? ratio * 2.0f : 1.0f - (ratio - 0.5f) * 2.0f,
            ratio < 0.5f ? 1.0f - ratio * 2.0f : 0.0f
        );
    }
    if ((globalParams.dbgViewFlags & uint32_t(ViewFlagsBits.AOVDiffuse)) != 0)
        return diffuseTex.Load(coord).xyz;
    if ((globalParams.dbgViewFlags & uint32_t(ViewFlagsBits.AOVSpecular)) != 0)
        return specularTex.Load(coord).xyz;
    return diffuseTex.Load(coord).xyz + specularTex.Load(coord).xyz;
}

[shader("fragment")]
float4 fragMain(float2 uv: TEXCOORD0, float2 fragCoord : SV_Position) : SV_TARGET {
    int3 coord = int3(int(fragCoord.x), int(fragCoord.y), 0);

    float3 src = LoadPTPostprocessRadiance(coord);
    
    PostprocessInfo pp;
    pp.Exposure = globalParams.camEV;
    uint32_t debugViews = uint32_t(ViewFlagsBits.BaseColor) | uint32_t(ViewFlagsBits.Normal) |
                          uint32_t(ViewFlagsBits.Position) | uint32_t(ViewFlagsBits.TextureLOD) |
                          uint32_t(ViewFlagsBits.AOVSampleCount);
    if ((globalParams.dbgViewFlags & debugViews) != 0)
        pp.Exposure = 0.0f;

    return float4(Postprocess(src, pp, textures3D[NonUniformResourceIndex(globalParams.viewLutIndex)], lutSampler), 1.0f);
}