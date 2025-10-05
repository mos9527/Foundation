[shader("fragment")]
float4 fragMain(float2 uv: TEXCOORD0) : SV_Target
{
    return uv.y;
}