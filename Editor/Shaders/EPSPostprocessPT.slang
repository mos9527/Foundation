import IFlags;
import IBindless3D;
import ICommon;
import IPostprocess;
[[SpecializationConstant]] int flags;

// Buffers (Welford online mean - already averaged)
Texture2D<float4> diffuseTex;
Texture2D<float4> specularTex;
SamplerState lutSampler;
uniform UBO globalParams;

[shader("fragment")]
float4 fragMain(float2 uv: TEXCOORD0, float2 fragCoord : SV_Position) : SV_TARGET {
    int3 coord = int3(int(fragCoord.x), int(fragCoord.y), 0);

    float3 src = float3(0.0);
    if ((flags & kViewAOVDiffuse) != 0)
        src = diffuseTex.Load(coord).xyz;
    else if ((flags & kViewAOVSpecular) != 0)
        src = specularTex.Load(coord).xyz;
    else
        src = diffuseTex.Load(coord).xyz + specularTex.Load(coord).xyz;

    PostprocessInfo pp;
    pp.Exposure = globalParams.camEV;
    return float4(Postprocess(src, pp, textures3D[NonUniformResourceIndex(globalParams.viewLutIndex)], lutSampler), 1.0f);
}
