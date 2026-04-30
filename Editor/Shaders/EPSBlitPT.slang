import IFlags;
import ICommon;
import IPostprocess;
import IMath;
import glsl;
[[SpecializationConstant]] int flags;
// Buffers (Welford online mean — already averaged)
Texture2D diffuseTex;
Texture2D specularTex;
Texture2D gBuffer0Tex;  // BaseColor.rgb, MaterialID (packed)
Texture2D gBuffer1Tex;  // Normal.xyz * 0.5 + 0.5, 0
Texture2D gBuffer2Tex;  // Position.xyz, 0
Texture2D<uint32_t> pickIDBuffer; // Instance ID map (R32_UINT), ~0u = no object
RWStructuredBuffer<uint32_t> pickResult; // [0] written when fragCoord == pickPixel
uniform UBO globalParams;

struct PickPC { int px; int py; };
[[vk::push_constant]] PickPC pickPC;

[shader("fragment")]
float4 fragMain(float2 uv: TEXCOORD0, float2 fragCoord : SV_Position) : SV_TARGET {
    int3 coord = int3(int(fragCoord.x), int(fragCoord.y), 0);
    uint32_t instanceID = pickIDBuffer.Load(coord);
    uint32_t pickID = pickResult[0];
    // GPU picking: if this pixel is the clicked one, write instanceID to result buffer
    if (pickPC.px >= 0 && int(fragCoord.x) == pickPC.px && int(fragCoord.y) == pickPC.py)
        pickResult[0] = instanceID;

    PostprocessInfo pp;
    pp.Exposure = globalParams.camEV;
    pp.HdrEnabled = globalParams.enableHDR;
    pp.HdrPaperWhiteNits = globalParams.paperWhiteNits;
    // GBuffer debug views — buffers already hold running averages (Welford)
    float4 result;
    if ((flags & kViewBaseColor) != 0) {
        float3 baseColor = gBuffer0Tex.Load(coord).xyz;
        result = float4(baseColor, 1.0f);
    } else if ((flags & kViewNormal) != 0) {
        float3 normal = gBuffer1Tex.Load(coord).xyz;
        result = float4(normal, 1.0f);
    } else if ((flags & kViewMaterialID) != 0) {
        float matIDPacked = gBuffer0Tex.Load(coord).w;
        uint materialID = uint(matIDPacked * 255.0f + 0.5f);
        result = float4(unpackUnorm4x8ToFloat(hash(materialID)).xyz, 1.0f);
    } else if ((flags & kViewPosition) != 0) {
        float3 position = gBuffer2Tex.Load(coord).xyz;
        result = float4(position, 1.0f);
    } else if ((flags & (kViewAOVDiffuse | kViewAOVSpecular)) != 0) {
        // Per-lobe AOV isolation views (tone mapped)
        float3 aov = float3(0);
        if ((flags & kViewAOVDiffuse) != 0)  aov = diffuseTex.Load(coord).xyz;
        if ((flags & kViewAOVSpecular) != 0) aov = specularTex.Load(coord).xyz;
        result = float4(Postprocess(aov, pp), 1.0f);
    } else {
        // Composite lighting = diffuse + specular (already averaged), then tone map
        float3 diffuse  = diffuseTex.Load(coord).xyz;
        float3 specular = specularTex.Load(coord).xyz;
        float3 Lpix = diffuse + specular;
        result = float4(Postprocess(Lpix, pp), 1.0f);
    }

    // Cross filter for a simple outline
    if (globalParams.postShowOutline != 0 && pickID != ~0u) {
        bool isSel  = (instanceID == pickID);
        bool selL = (pickIDBuffer.Load(coord + int3(-1, 0, 0)) == pickID);
        bool selR = (pickIDBuffer.Load(coord + int3( 1, 0, 0)) == pickID);
        bool selT = (pickIDBuffer.Load(coord + int3( 0,-1, 0)) == pickID);
        bool selB = (pickIDBuffer.Load(coord + int3( 0, 1, 0)) == pickID);
        bool isEdge = isSel && !(selL && selR && selT && selB);
        if (isEdge)
            result = float4(1.0, 0.6, 0.1, 1.0);
    }
    return result;
}