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
// On Blending (execessively detailed since it's something we've been dealing with at work):
//  - Our output is ALWAYAS in whatever encoding the *colorspace* set by flags is
//  - Our framebuffer format (backbuffer in this case) is ALWAYAS UNORM
//  - Blending HW doesn't care about colorspace and it's always done linearlly... (unless it's sRGB formats)
// For opaque UI this is correct. But for alpha-blended ones we actually produce *different* results for sRGB/HDR displays:
//  > SDR/sRGB is Gamma blending (as in Photoshop/Figma) where colors are always encoded in Gamma space and linearly blended.
//  > HDR/PQ is done in PQ encoded colors and again linearly blended. Non-standard and to put it plainly is simply wrong.
// Gamma blending _is_ ideal for UI elements.
//  > HDR can't do that yet without doing things in sRGB and output in PQ (*shudders*)
//  > Should we opt for linear we can do UIs offscreen. (ex. RE Engine https://www.docswell.com/s/CAPCOM_RandD/KENVMJ-RE2023#p34)
// For ImGui elements we'll let the wrong blending happen. Artists don't use our tool for UI anyways other than getting ground-truth renderings w/o UI.
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
