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
uniform GizmoUBO globalParams;

Texture2D<float> sceneDepth;
SamplerState linSampler;

struct VertexOutput
{
    float4 svPosition : SV_Position;
    float4 color : COLOR0;
    float viewDepth : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
};

// Reverse-Z infinite perspective: ndcDepth = zNear / viewDepth, viewDepth = -view.z > 0.
float NdcDepthToViewDepth(float ndcDepth)
{
    return globalParams.zNear / ndcDepth;
}

[shader("vertex")]
VertexOutput vertMain(float3 position : POSITION, float4 color : COLOR0)
{
    VertexOutput output;
    output.svPosition = mul(globalParams.viewProj, float4(position, 1.0f));
    output.color = color;
    float4 viewPos = mul(globalParams.view, float4(position, 1.0f));
    output.viewDepth = -viewPos.z;
    output.worldPos = position;
    return output;
}

float DistanceFade(float3 worldPos)
{
    float dist = length(worldPos - globalParams.camPosition);
    float t = 1.0f - smoothstep(globalParams.distanceFadeStart, globalParams.distanceFadeEnd, dist);
    return lerp(0.15f, 1.0f, t);
}

[shader("fragment")]
float4 fragMain(VertexOutput input) : SV_TARGET
{
    static const float kDepthEps = 1e-6f;

    float visibility = 1.0f;
    float2 uv = input.svPosition.xy / globalParams.screenSize;
    float sceneNdc = sceneDepth.SampleLevel(linSampler, uv, 0.0f);
    if (sceneNdc > kDepthEps)
    {
        float sceneViewDepth = NdcDepthToViewDepth(sceneNdc);
        float delta = input.viewDepth - sceneViewDepth - globalParams.depthBias;
        if (delta > 0.0f)
            visibility = 1.0f - smoothstep(0.0f, globalParams.fadeRange, delta);
    }

    float distanceFade = DistanceFade(input.worldPos);
    float alpha = input.color.a * distanceFade * lerp(globalParams.occludedAlpha, 1.0f, visibility);
    return float4(input.color.rgb, alpha);
}
