// See also Scripts/ColorPrimaries.ipynb
static const float3x3 XYZ_BT709 = float3x3(
    3.240969942f, -1.537383178f, -0.498610760f,
   -0.969243636f,  1.875967502f,  0.041555057f,
    0.055630080f, -0.203976959f,  1.056971514f);

struct ViewPushConstant
{
    float centerX;
    float centerY;
    float rangeX;
    float rangeY;
    uint mode;
};
[[vk::push_constant]] ViewPushConstant pc;

struct VertexOutput
{
    float4 svPosition : SV_Position;
    float2 xy : TEXCOORD0;
    float4 color : COLOR0;
};

float3 xyYToXYZ(float2 xy, float Y)
{
    float y = max(xy.y, 1e-5f);
    return float3(
        xy.x * Y / y,
        Y,
        (1.0f - xy.x - xy.y) * Y / y);
}

float3 linearToSrgb(float3 L)
{
    L = saturate(L);
    float3 hi = 1.055f * pow(L, 1.0f / 2.4f) - 0.055f;
    float3 lo = 12.92f * L;
    return lerp(lo, hi, float3(L > float3(0.0031308f)));
}

float gridLine(float value, float spacing, float width)
{
    float d = abs(frac(value / spacing) - 0.5f) * spacing;
    return 1.0f - smoothstep(0.0f, width, d);
}

[shader("vertex")]
VertexOutput vertMain(float2 xy : POSITION, float4 color : COLOR0)
{
    VertexOutput output;
    float2 ndc = float2(
        (xy.x - pc.centerX) * 2.0f / pc.rangeX,
        (pc.centerY - xy.y) * 2.0f / pc.rangeY);
    output.svPosition = float4(ndc, 0.0f, 1.0f);
    output.xy = xy;
    output.color = color;
    return output;
}

[shader("fragment")]
float4 fragMain(float2 xy : TEXCOORD0, float4 color : COLOR0) : SV_Target
{
    if (pc.mode == 1)
        return color;

    float3 XYZ = xyYToXYZ(xy, color.a);
    float3 rgb709 = mul(XYZ_BT709, XYZ);
    float3 col = linearToSrgb(rgb709);

    float grid = max(gridLine(xy.x, 0.1f, 0.0015f), gridLine(xy.y, 0.1f, 0.0015f));
    col = lerp(col, float3(0.0f), grid * 0.35f);

    return float4(col, 1.0f);
}
