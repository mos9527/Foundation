static const float kTau = 6.28318530718;
static const float kSpeed = 0.5;
static const float kScale = 1.0;
static const int kIterations = 5;
static const float kIntensity = 0.005;
static const float kContrast = 1.4;
static const float kBrightness = 1.17;
static const float kSharpness = 8.0;
static const float kTimeOffset = 23.0;
static const float kSeedOffset = 250.0;
static const float3 kColorTint = float3(0.0, 0.35, 0.5);
static const float3 kColorMul = float3(1.0, 1.0, 1.0);

struct PushConstant
{
    uint32_t textureIndex;
    uint32_t width;
    uint32_t height;
    float time;
};
[[vk::push_constant]] PushConstant pc;

float3 UVToDir(float2 uv)
{
    float2 t = uv * 2.0 - 1.0;
    return normalize(float3(t.x, 1.0, t.y));
}

float4 EvalWaterTurbulence(float2 uv, float time)
{
    time = time * kSpeed + kTimeOffset;
    float3 dir = UVToDir(uv);

    float3 p = dir * kScale * kTau - kSeedOffset;
    float3 iter = p;
    float c = 1.0;
    float inten = kIntensity;

    [unroll]
    for (int n = 0; n < kIterations; ++n)
    {
        float t = time * (1.0 - (3.5 / float(n + 1)));
        iter = p + float3(
            cos(t - iter.x) + sin(t + iter.y),
            sin(t - iter.y) + cos(t + iter.z),
            cos(t - iter.z) + sin(t + iter.x));
        c += 1.0 / length(float3(
            p.x / (sin(iter.x + t) / inten),
            p.y / (cos(iter.y + t) / inten),
            p.z / (sin(iter.z + t) / inten)));
    }

    c /= float(kIterations);
    c = kBrightness - pow(c, kContrast);
    float intensity = pow(abs(c), kSharpness);

    float3 colour = intensity.xxx;
    colour = saturate(colour + kColorTint);
    colour *= kColorMul;
    return float4(colour, 1.0f);
}

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VertexOutput vertMain(uint vertexID : SV_VertexID)
{
    VertexOutput output;
    output.uv = float2((vertexID << 1) & 2, vertexID & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

[shader("fragment")]
float4 fragMain(VertexOutput input) : SV_Target
{
    return EvalWaterTurbulence(input.uv, pc.time);
}
