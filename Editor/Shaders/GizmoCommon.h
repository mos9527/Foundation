struct GizmoUBO
{
    float4x4 viewProj;
    float4x4 view;
    float zNear;
    float depthBias;
    float fadeRange;
    float occludedAlpha;
    float3 camRight;
    float _pad0;
    float3 camUp;
    float _pad1;
    float iconWorldHalfExtent;
    float distanceFadeStart;
    float distanceFadeEnd;
    float3 camPosition;
    float2 screenSize;
};
