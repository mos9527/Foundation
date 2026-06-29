// Simplified example postprocess (path tracer): combine diffuse + specular AOVs.
// No view LUTs, no exposure tonemap, no debug AOVs - just clamp + gamma.
Texture2D<float4> diffuseTex;
Texture2D<float4> specularTex;

[shader("fragment")]
float4 fragMain(float2 uv: TEXCOORD0, float2 fragCoord : SV_Position) : SV_TARGET {
    int3 coord = int3(int(fragCoord.x), int(fragCoord.y), 0);
    float3 src = diffuseTex.Load(coord).xyz + specularTex.Load(coord).xyz;
    src = clamp(src, 0.0f, 1.0f);
    src = pow(src, float3(1.0f / 2.2f));
    return float4(src, 1.0f);
}
