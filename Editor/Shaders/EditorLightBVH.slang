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
    uint firstLightBVHNode;
    uint numLightBVHNodes;
    uint firstLightBVHDistantNode;
    uint numLightBVHDistantNodes;
};
uniform GizmoUBO globalParams;

struct GSLightBVHNode
{
    uint header;
    float3 origin;
    float3 extent;
    float flux;
    float3 coneDirection;
    float cosConeAngle;
};
StructuredBuffer<GSLightBVHNode> lightBVHNodes;

Texture2D<float> sceneDepth;
SamplerState linSampler;

struct VertexOutput
{
    float4 svPosition : SV_Position;
    float4 color : COLOR0;
    float viewDepth : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
};

uint2 BoxEdge(uint edge)
{
    switch (edge)
    {
    case 0: return uint2(0, 1);
    case 1: return uint2(1, 3);
    case 2: return uint2(3, 2);
    case 3: return uint2(2, 0);
    case 4: return uint2(4, 5);
    case 5: return uint2(5, 7);
    case 6: return uint2(7, 6);
    case 7: return uint2(6, 4);
    case 8: return uint2(0, 4);
    case 9: return uint2(1, 5);
    case 10: return uint2(2, 6);
    default: return uint2(3, 7);
    }
}

float3 BoxVertex(GSLightBVHNode node, uint lane)
{
    if (lane >= 24u)
        return node.origin;
    uint2 edge = BoxEdge(lane >> 1u);
    uint corner = (lane & 1u) != 0u ? edge.y : edge.x;
    float3 signValue = float3(
        (corner & 1u) != 0u ? 1.0f : -1.0f,
        (corner & 2u) != 0u ? 1.0f : -1.0f,
        (corner & 4u) != 0u ? 1.0f : -1.0f);
    return node.origin + signValue * node.extent;
}

float3 DistantVertex(GSLightBVHNode node, uint lane)
{
    float3 axis = length(node.coneDirection) > 1e-6f ? normalize(node.coneDirection) : float3(0, 0, -1);
    float3 helper = abs(axis.z) < 0.999f ? float3(0, 0, 1) : float3(0, 1, 0);
    float3 tangent = normalize(cross(helper, axis));
    float3 bitangent = cross(axis, tangent);
    float cosAngle = node.cosConeAngle != -1.0f ? clamp(node.cosConeAngle, -1.0f, 1.0f) : 0.0f;
    float sinAngle = sqrt(max(0.0f, 1.0f - cosAngle * cosAngle));
    float distance = max(8.0f, globalParams.distanceFadeStart * 0.5f);

    uint ringIndex;
    if (lane < 48u)
    {
        uint segment = lane >> 1u;
        ringIndex = segment + (lane & 1u);
    }
    else
    {
        uint ray = (lane - 48u) >> 1u;
        if ((lane & 1u) == 0u)
            return globalParams.camPosition;
        ringIndex = ray * 3u;
    }

    float phi = 6.28318530717958647692f * float(ringIndex % 24u) / 24.0f;
    float3 radial = cos(phi) * tangent + sin(phi) * bitangent;
    return globalParams.camPosition + distance * (cosAngle * axis + sinAngle * radial);
}

[shader("vertex")]
VertexOutput vertMain(uint vertexID : SV_VertexID)
{
    uint nodeOffset = vertexID / 64u;
    uint lane = vertexID % 64u;
    uint nodeIndex = globalParams.firstLightBVHNode + nodeOffset;
    GSLightBVHNode node = lightBVHNodes[nodeIndex];
    bool distant = globalParams.numLightBVHDistantNodes != 0u &&
        nodeIndex >= globalParams.firstLightBVHDistantNode;
    bool leaf = (node.header & (1u << 31u)) != 0u;

    float3 position = distant ? DistantVertex(node, lane) : BoxVertex(node, lane);
    float4 color = distant
        ? (leaf ? float4(0.25f, 0.75f, 1.0f, 0.9f) : float4(0.55f, 0.35f, 1.0f, 0.45f))
        : (leaf ? float4(0.2f, 1.0f, 0.35f, 0.8f) : float4(1.0f, 0.65f, 0.15f, 0.35f));
    if (!distant && lane >= 24u)
        color.a = 0.0f;

    VertexOutput output;
    output.svPosition = mul(globalParams.viewProj, float4(position, 1.0f));
    output.color = color;
    float4 viewPos = mul(globalParams.view, float4(position, 1.0f));
    output.viewDepth = -viewPos.z;
    output.worldPos = position;
    return output;
}

[shader("fragment")]
float4 fragMain(VertexOutput input) : SV_TARGET
{
    float visibility = 1.0f;
    float2 uv = input.svPosition.xy / globalParams.screenSize;
    float sceneNdc = sceneDepth.SampleLevel(linSampler, uv, 0.0f);
    if (sceneNdc > 1e-6f)
    {
        float sceneViewDepth = globalParams.zNear / sceneNdc;
        float delta = input.viewDepth - sceneViewDepth - globalParams.depthBias;
        if (delta > 0.0f)
            visibility = 1.0f - smoothstep(0.0f, globalParams.fadeRange, delta);
    }
    float alpha = input.color.a * lerp(globalParams.occludedAlpha, 1.0f, visibility);
    return float4(input.color.rgb, alpha);
}
