[[SpecializationConstant]] int viewOverdraw;

StructuredBuffer<uint32_t> globalMax;
Texture2D<uint32_t> overdraw;
Texture2D gbuffer;
SamplerState sampler;

float3 heatmapGradient(float t) {
    // These are the approximate keyframes for the "Inferno" palette
    float3 c0 = float3(0.000, 0.000, 0.002); // Deep Black-Blue
    float3 c1 = float3(0.250, 0.000, 0.350); // Purple
    float3 c2 = float3(0.700, 0.100, 0.200); // Red-Magenta
    float3 c3 = float3(0.950, 0.500, 0.000); // Orange
    float3 c4 = float3(1.000, 0.950, 0.600); // White-Yellow

    // Manual Multi-Stop Gradient
    // We remap 't' to segments to blend between the colors
    float3 col = c0;
    
    // Smoothly blend based on thresholds
    // Using lerp with smoothstep for better gradients
    if (t < 0.25) {
        col = lerp(c0, c1, t / 0.25);
    } else if (t < 0.5) {
        col = lerp(c1, c2, (t - 0.25) / 0.25);
    } else if (t < 0.75) {
        col = lerp(c2, c3, (t - 0.50) / 0.25);
    } else {
        col = lerp(c3, c4, (t - 0.75) / 0.25);
    }
    
    return col;
}
[shader("fragment")]
float4 fragMain(float2 uv: TEXCOORD0, float2 fragCoord : SV_Position) : SV_TARGET {
    int3 coord = int3(fragCoord.x, fragCoord.y, 0);
    if (viewOverdraw){
        uint32_t draws = overdraw.Load(coord);
        return float4(heatmapGradient((float)draws / globalMax[0]), 1.0);
    }
    float4 gbuffer = gbuffer.Load(coord);
    return gbuffer;
    return float4(0);
}
