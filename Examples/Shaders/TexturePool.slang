/**
* Bindless texture array example with animation.
*/
struct PushConstant {
    float time;
    uint32_t num_textures;
    uint32_t first_texture;
}
[[vk::push_constant]] PushConstant pc;
SamplerState sampler;
[[vk::binding(0, 1)]] // [binding, set]
texture2D textures[];

[shader("fragment")]
float4 fragMain(float2 pix: TEXCOORD0) : SV_Target
{
    float theta = atan(pix.y / pix.x);
    float r = length(pix);
    // Animation!
    float t = pc.time * 0.5f;

    float2 translation = float2(sin(t),cos(t)) * 0.1f;
    float rotation = t * 0.1f;
    float scale = 0.75f + 0.5 * cos(t);

    float2 p = float2(cos(theta + rotation), sin(theta + rotation)) * r;
    p += translation;
    p *= scale;
    // Render individual textures, bindless style.
    float sq = sqrt(pc.num_textures);
    int2 pid = p * sq;
    uint32_t id = pid.y * sq + pid.x;
    id = max(1, id) % pc.num_textures;
    float2 uv = p * sq;
    return textures[id + pc.first_texture].Sample(sampler, uv);
}
