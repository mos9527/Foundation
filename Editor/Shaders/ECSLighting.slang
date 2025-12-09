#include "ICommon.slang"
#include "IBRDF.slang"
[[SpecializationConstant]] int flags;

uniform UBO globalParams;
Texture2D<float4> RT0;
Texture2D<float4> RT1;
Texture2D<float4> RT2;
Texture2D<float> depth;
RWTexture2D<float4> output;
StructuredBuffer<GSMaterial> materials;

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint2 tid: SV_DispatchThreadID) {
    if (tid.x >= globalParams.fbWidth || tid.y >= globalParams.fbHeight) return;
    float4 color0 = RT0.Load(uint3(tid,0));
    float4 color1 = RT1.Load(uint3(tid,0));
    float4 color2 = RT2.Load(uint3(tid,0));


    float3 baseColor = color0.xyz;
    uint32_t materialID = (uint)(color0.w * 255.0f), BSign;
    BSign = bitfieldExtract(materialID, 7, 1);
    materialID = bitfieldExtract(materialID, 0, 7);

    float ndcDepth = depth.Load(uint3(tid,0));
    if (ndcDepth <= EPS) {
        // Infinite depth
        output[tid] = float4(0);
        return;
    }

    float4 ndcPosition = float4(tid.xy / float2(globalParams.fbWidth, globalParams.fbHeight), ndcDepth, 1.0f);
    ndcPosition.y = 1 - ndcPosition.y;
    ndcPosition.xy = ndcPosition.xy * 2.0f - 1.0f;
    float4 wsPosition = mul(globalParams.inverseViewProj, ndcPosition);

    float3 p = wsPosition.xyz / wsPosition.w;
    float3 eye = globalParams.camPosition;

    GSMaterial material = materials.Load(materialID  + globalParams.firstMaterial);
    float3 n, t, b;
    unpackTBN888(color1.xyz, n, t);
    b = cross(n, t) * (BSign == 1 ? 1.0f : -1.0f);

    float metallic = color1.w;
    float roughness = color2.w;
    float3 emissive = color2.xyz;

    if (flags & (kViewBaseColor | kViewMeshlet)) {
        // Meshlet passed down to GBuffer RT0
        output[tid] = float4(baseColor, 1.0f);
        return;
    } else if (flags & kViewMaterialID) {
        output[tid] = float4(unpackUnorm4x8ToFloat(hash(materialID)).xyz, 1.0f);
        return;
    } else if (flags & kViewNormal) {
        output[tid] = float4(n, 1.0f);
        return;
    } else if (flags & kViewPosition) {
        output[tid] = float4(p.xyz, 1.0f);
        return;
    } else {
        // PBR Lighting
        if (flags & (kViewGBufferDiffuse | kViewGBufferSpecular)) // Lighting only
            baseColor = float3(1);

        float3 v = normalize(eye - p);
        float3 l = -globalParams.sunDirection;
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
        float3 Fd = baseColor / PI;

        // Specular
        float anisotropy = material.anisotropy;
        roughness = roughness * roughness;
        float at = max(roughness * (1.0 + anisotropy), 0.001);
        float ab = max(roughness * (1.0 - anisotropy), 0.001);
        float D = D_GGX_Anisotropic(NoH, h, t, b, at, ab);
        float V = V_SmithGGXCorrelated_Anisotropic(at, ab, ToV, BoV, ToL, BoL, NoV, NoL);

        // glTF spec calls D*V the specular BRDF, F is introduced later.
        float Fs = D * V;
        // https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#metal-brdf-and-dielectric-brdf
        float3 metalBRDF = Fs * F_Schlick(LoH, baseColor);
        float3 dielectricBRDF = lerp(Fd, Fs, F_Schlick(LoH, float3(0.04)));
        float3 material = lerp(dielectricBRDF, metalBRDF, metallic) * lighting;
        // Final color
        if (flags & kViewGBufferDiffuse) {
            output[tid] = float4(Fd * lighting, 1.0f);
            return;
        } else if (flags & kViewGBufferSpecular) {
            output[tid] = float4(float3(Fs) * lighting, 1.0f);
            return;
        } else {
            output[tid] = float4(material + emissive, 1.0f);
        }
    }
}
