import ITonemapping;

struct Vertex
{
    float4 pos : SV_Position;
    float4 color;
    float2 uv;
    uint textureId;
    uint samplerId;
};
struct PushConstants
{
    float2 s; // scale
    float2 t; // translation
    uint textureId;
    uint samplerId;
};

[[vk::push_constant]] PushConstants pc;

SamplerState linSampler;
SamplerState nearSampler;

[[vk::binding(0, 1)]] // [binding, set]
texture2D textures[];

[shader("vertex")]
Vertex vertMain(
    float2 pos   : POSITION,
    float2 uv    : TEXCOORD,
    float4 color : COLOR)
{
    Vertex output;
    output.pos = float4(pos * pc.s + pc.t, 0.0, 1.0);
    output.color = color;
    output.uv = uv;
    output.textureId = pc.textureId;
    output.samplerId = pc.samplerId;
    return output;
}

// Tonemap ImGui outputs to these colorspaces
// On Blending:
//  - Our output is always
static const int kFlagsOutputSrgb = 1 << 0;
static const int kFlagsOutputSt2084 = 1 << 1;
[[SpecializationConstant]] int flags;

[shader("fragment")]
float4 fragMain(Vertex vtx) : SV_Target
{
    float4 sample;
    switch (vtx.samplerId){
        case 0:
            sample = textures[vtx.textureId].Sample(linSampler, vtx.uv);
            break;
        case 1:
            sample = textures[vtx.textureId].Sample(nearSampler, vtx.uv);
            break;
    }
    sample = vtx.color * sample;
    TonemapInfo info;
    if ((flags & kFlagsOutputSt2084) != 0) {
        PqOOTF ootf;
        PqEOTF eotf;
        // 1.0 -> Paperwhite
        // We take linear input. So invert (gamma-linear) first.
        SrgbEOTF srgb;
        sample.rgb = srgb.Forward(info, sample.rgb);
        float3 L = ootf.Transfer(info, sample.rgb);
        return float4(eotf.Inverse(info, L), sample.a);
    } else {
        // Output is already gamma-encoded.
        return sample;
    }
}