struct GizmoPushConstant
{
    float4x4 viewProj;
    float depthBias;
    float fadeRange;
    float occludedAlpha;
    float _pad;
};
[[vk::push_constant]] GizmoPushConstant pc;

Texture2D<float> sceneDepth;

struct VertexOutput
{
    float4 svPosition : SV_Position;
    float4 color : COLOR0;
};

[shader("vertex")]
VertexOutput vertMain(float3 position : POSITION, float4 color : COLOR0)
{
    VertexOutput output;
    output.svPosition = mul(pc.viewProj, float4(position, 1.0f));
    output.color = color;
    return output;
}

[shader("fragment")]
float4 fragMain(VertexOutput input) : SV_TARGET
{
    float visibility = 1.0f;
    float sceneZ = sceneDepth.Load(int3(input.svPosition.xy, 0));
    if (sceneZ > 0.0f)
    {
        float delta = input.svPosition.z - sceneZ - pc.depthBias;
        if (delta > 0.0f)
            visibility = 1.0f - smoothstep(0.0f, pc.fadeRange, delta);
    }

    float alpha = input.color.a * lerp(pc.occludedAlpha, 1.0f, visibility);
    return float4(input.color.rgb, alpha);
}
