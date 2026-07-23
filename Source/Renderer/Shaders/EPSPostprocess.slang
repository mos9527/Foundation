import IFlags;
import ICommon;
import IPostprocess;
import IBindless3D;

Texture2D<float4> diffuseTex;
Texture2D<float4> specularTex;
SamplerState lutSampler;
uniform PostprocessUBO globalParams;

[shader("fragment")]
float4 fragMain(float2 uv: TEXCOORD0, float2 fragCoord : SV_Position) : SV_TARGET {
    int3 coord = int3(int(fragCoord.x), int(fragCoord.y), 0);
    float4 src = diffuseTex.Load(coord);

    uint32_t debugViews = uint32_t(ViewFlagsBits.BaseColor) | uint32_t(ViewFlagsBits.Normal) |
                          uint32_t(ViewFlagsBits.Position) | uint32_t(ViewFlagsBits.Meshlet) |
                          uint32_t(ViewFlagsBits.TextureLOD);
    if ((globalParams.dbgViewFlags & debugViews) == 0)
        src += specularTex.Load(coord);

    PostprocessInfo pp;
    pp.Exposure = globalParams.camEV;
    if ((globalParams.dbgViewFlags & debugViews) != 0)
        pp.Exposure = 0.0f;

    return float4(Postprocess(src.xyz, pp, textures3D[NonUniformResourceIndex(globalParams.viewLutIndex)], lutSampler), 1.0f);
}
