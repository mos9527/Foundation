struct Vertex
{
    float4 pos : SV_Position;
    float4 color;
    float2 uv;
    uint textureId;
};
struct PushConstants
{
    float2 s; // scale
    float2 t; // translation
    uint textureId;
};

[[vk::push_constant]] PushConstants pc;

SamplerState sampler;

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
    return output;
}

[shader("fragment")]
float4 fragMain(Vertex vtx) : SV_Target
{
    if (vtx.textureId == 0)
        return vtx.color;
    return vtx.color * textures[vtx.textureId].Sample(sampler, vtx.uv);
}