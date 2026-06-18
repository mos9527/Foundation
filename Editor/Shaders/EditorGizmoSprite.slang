import ITonemapping;

struct GizmoUBO
{
    float4x4 viewProj;
    float4x4 view;
    float zNear;
    float depthBias;
    float fadeRange;
    float occludedAlpha;
    float3 camRight;
    float _pad0;
    float3 camUp;
    float _pad1;
    float iconWorldHalfExtent;
    float _pad2;
    float2 _pad3;
};
uniform GizmoUBO globalParams;

Texture2D<float> sceneDepth;
SamplerState linSampler;

static const int kFlagsOutputSrgb = 1 << 0;
static const int kFlagsOutputSt2084 = 1 << 1;
[[SpecializationConstant]] int flags;

struct VertexOutput
{
    float4 svPosition : SV_Position;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
    nointerpolation uint textureId : TEXCOORD1;
    float viewDepth : TEXCOORD2;
};

float NdcDepthToViewDepth(float ndcDepth)
{
    return globalParams.zNear / ndcDepth;
}

float GizmoVisibility(float viewDepth, float2 fragCoord)
{
    static const float kDepthEps = 1e-6f;
    float sceneNdc = sceneDepth.Load(int3(fragCoord, 0));
    if (sceneNdc <= kDepthEps)
        return 1.0f;

    float sceneViewDepth = NdcDepthToViewDepth(sceneNdc);
    float delta = viewDepth - sceneViewDepth - globalParams.depthBias;
    if (delta <= 0.0f)
        return 1.0f;
    return 1.0f - smoothstep(0.0f, globalParams.fadeRange, delta);
}

[shader("vertex")]
VertexOutput vertMain(float3 center : POSITION, float2 corner : TEXCOORD0, float2 uv : TEXCOORD1, float4 color : COLOR0,
                      uint textureId : TEXCOORD2)
{
    float3 position = center + (globalParams.camRight * corner.x + globalParams.camUp * corner.y) *
                                  globalParams.iconWorldHalfExtent;

    VertexOutput output;
    output.svPosition = mul(globalParams.viewProj, float4(position, 1.0f));
    output.color = color;
    output.uv = uv;
    output.textureId = textureId;
    float4 viewPos = mul(globalParams.view, float4(center, 1.0f));
    output.viewDepth = -viewPos.z;
    return output;
}

public[[vk_binding(0, 1)]] Texture2D<float4> textures[];
[shader("fragment")]
float4 fragMain(VertexOutput input) : SV_TARGET
{
    float2 uv = float2(input.uv.x, 1.0f - input.uv.y); // Vulkan UV coords
    float4 sample = input.color * textures[NonUniformResourceIndex(input.textureId)].Sample(linSampler, uv);

    float visibility = GizmoVisibility(input.viewDepth, input.svPosition.xy);
    sample.a *= lerp(globalParams.occludedAlpha, 1.0f, visibility);

    TonemapInfo info;
    if ((flags & kFlagsOutputSt2084) != 0)
    {
        PqOOTF ootf;
        PqEOTF eotf;
        SrgbEOTF srgb;
        sample.rgb = srgb.Forward(info, sample.rgb);
        float3 L = ootf.Transfer(info, sample.rgb);
        return float4(eotf.Inverse(info, L), sample.a);
    }
    return sample;
}
