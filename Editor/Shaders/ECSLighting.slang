#include "ICommon.slang"
#include "IBRDF.slang"

struct PushConstant {
    int w, h;
}
[[vk::push_constant]] PushConstant pc;

uniform UBO globalParams;
StructuredBuffer<GSMaterial> materials;
Texture2D<float4> RT0;
Texture2D<float4> RT1;
Texture2D<float4> RT2;
Texture2D<float> depth;
RWTexture2D<float4> output;

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint2 tid: SV_DispatchThreadID) {
    if (tid.x >= pc.w || tid.y >= pc.h) return;
    float4 color0 = RT0.Load(uint3(tid,0));
    float4 color1 = RT1.Load(uint3(tid,0));
    float4 color2 = RT2.Load(uint3(tid,0));
    float viewZ = depth.Load(uint3(tid,0));

    float3 baseColor = color0.xyz;
    uint32_t materialID = (uint)(color0.w * 255.0f), BSign;
    BSign = bitfieldExtract(materialID, 7, 1);
    materialID = bitfieldExtract(materialID, 0, 7);
    if (viewZ <= EPS) {
        // Infinite depth
        output[tid] = float4(0);
        return;
    }
    viewZ = globalParams.zNear / viewZ;

    GSMaterial material = materials.Load(materialID  + globalParams.firstMaterial);
    float3 n, t, b;
    unpackTBN888(color1.xyz, n, t);
    b = cross(n, t) * (BSign == 1 ? 1.0f : -1.0f);
    
    float metallic = color1.w;
    float roughness = color2.w;
    float3 emissive = color2.xyz;

    float3 v = globalParams.camDirection;
    float3 l = globalParams.sunDirection;
    float3 h = normalize(v + l);
    float NoH = saturate(dot(n,h));
    float LoH = saturate(dot(l,h));
    float ToV = saturate(dot(t,v));
    float BoV = saturate(dot(b,v));
    float ToL = saturate(dot(t,l));
    float BoL = saturate(dot(b,l));
    float NoV = abs(dot(n,v)) + EPS;
    float NoL = saturate(dot(n,l));


    float3 lighting = float3(NoL) * globalParams.sunIntensity + globalParams.ambientColor;

    // Diffuse
    // https://seblagarde.wordpress.com/2012/01/08/pi-or-not-to-pi-in-game-lighting-equation/
    float3 Fd = lighting * baseColor;

    // Specular
    float anisotropy = material.anisotropy;
    roughness = roughness * roughness;
    float at = max(roughness * (1.0 + anisotropy), 0.001);
    float ab = max(roughness * (1.0 - anisotropy), 0.001);
    float D = D_GGX_Anisotropic(NoH, h, t, b, at, ab);
    float3 F = F_Schlick(LoH, lerp(baseColor, float3(0), metallic));
    float V = V_SmithGGXCorrelated_Anisotropic(at, ab, ToV, BoV, ToL, BoL, NoV, NoL);
    float3 Fr = lighting * D * V * F;

    // Final color
    float3 Fx = Fd + Fr + emissive;
    output[tid] = float4(Fx, 1);
}
