[[vk::push_constant]] float time;

[shader("fragment")]
float4 pass1(float2 p: TEXCOORD0) : SV_Target
{
    return float4(abs(cos(p * time)), 0, 1);
}

[shader("fragment")]
float4 pass2(float2 p: TEXCOORD0) : SV_Target
{
    return float4(abs(sin(p + time)), 0, 0.5);
}
