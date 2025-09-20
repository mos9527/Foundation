/**
* Simple one-pass Bloom
* ---
* Blur based on https://github.com/XorDev/1PassBlur
*/

Texture2D screen;
SamplerState sampler;

struct PushConstant{
    float time;
    uint32_t width;
    uint32_t height;
}
[[vk::push_constant]] PushConstant pc;

float2x2 rotate(float theta){
    float c = cos(theta);
    float s = sin(theta);
    return float2x2(c, -s, s, c);
}

[shader("fragment")]
float4 fragMain(float2 uv: TEXCOORD0) : SV_TARGET {
    const float2x2 golden_angle = rotate(137.508 / 360.0); // radians
    float r = 8.0; // pixels
    int n = 64; // samples
    float threshold = 0.35; // luminance threshold
    float intensity = 1.25; // bloom intensity

    float3 bloom = float3(0);
    float2 offset = float2(r, 0) / sqrt(n);

    for (int i = 0;i < n; i++){
        offset = mul(offset,golden_angle);
        float2 sample_uv = uv + offset * sqrt(i) / float2(pc.width, pc.height);
        float3 sample = screen.Sample(sampler, sample_uv).rgb * (1 - i / n); // weight samples
        float luma = dot(sample, float3(0.299, 0.587, 0.114));
        bloom += (luma > threshold) ? sample : float3(0);
    }
    bloom /= n; // average
    bloom *= intensity; 
    bloom += screen.Sample(sampler, uv).rgb; // add original
    return float4(bloom, 1);
}
