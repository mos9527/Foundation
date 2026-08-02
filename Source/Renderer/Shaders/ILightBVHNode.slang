import IMath;

public static const float kLightBVHInvalidCosConeAngle = -1.0f;
public static const uint kLightBVHLeafBit = 1u << 31;
public static const uint kLightBVHLightCountBits = 4u;
public static const uint kLightBVHLightOffsetBits = 31u - kLightBVHLightCountBits;

public struct LightBVHNodeAttributes
{
    public float3 origin;
    public float3 extent;
    public float flux;
    public float3 coneDirection;
    public float cosConeAngle;
};

public struct GSLightBVHNode
{
    public uint4 data0;
    public uint4 data1;
};

public bool LightBVHNodeIsLeaf(GSLightBVHNode node)
{
    return (node.data0.x & kLightBVHLeafBit) != 0u;
}

public uint LightBVHNodeHeader(GSLightBVHNode node)
{
    return node.data0.x;
}

public uint LightBVHNodeRightChild(GSLightBVHNode node)
{
    return node.data0.x;
}

public uint LightBVHNodeLightCount(GSLightBVHNode node)
{
    return (node.data0.x >> kLightBVHLightOffsetBits) & ((1u << kLightBVHLightCountBits) - 1u);
}

public uint LightBVHNodeLightOffset(GSLightBVHNode node)
{
    return node.data0.x & ((1u << kLightBVHLightOffsetBits) - 1u);
}

float3 DecodeLightBVHConeDirection(uint packed)
{
    int2 encoded = int2(int(packed << 16u) >> 16, int(packed) >> 16);
    float2 oct = clamp(float2(encoded) * (1.0f / 32767.0f), -1.0f, 1.0f);
    return unpackUnitOctahedralSnorm(oct);
}

uint EncodeLightBVHConeDirection(float3 direction)
{
    if (dot(direction, direction) <= 1e-12f)
        return 0u;
    float2 oct = packUnitOctahedralSnorm(normalize(direction));
    int2 encoded = int2(round(clamp(oct, -1.0f, 1.0f) * 32767.0f));
    return (uint(encoded.x) & 0xffffu) | ((uint(encoded.y) & 0xffffu) << 16u);
}

uint PackLightBVHExtent(float extent)
{
    extent = max(extent, 0.0f);
    uint packed = f32tof16(extent);
    if (f16tof32(packed) < extent && packed < 0x7c00u)
        ++packed;
    return packed;
}

public LightBVHNodeAttributes UnpackLightBVHNode(GSLightBVHNode node)
{
    LightBVHNodeAttributes attributes;
    attributes.origin = asfloat(node.data0.yzw);
    attributes.extent = float3(
        f16tof32(node.data1.x & 0xffffu),
        f16tof32(node.data1.x >> 16u),
        f16tof32(node.data1.y & 0xffffu));
    attributes.cosConeAngle = float(node.data1.y >> 16u) * (1.0f / 32767.0f) - 1.0f;
    attributes.coneDirection = DecodeLightBVHConeDirection(node.data1.z);
    attributes.flux = asfloat(node.data1.w);
    return attributes;
}

public void PackLightBVHNodeAttributes(inout GSLightBVHNode node, LightBVHNodeAttributes attributes)
{
    uint extentX = PackLightBVHExtent(attributes.extent.x);
    uint extentY = PackLightBVHExtent(attributes.extent.y);
    uint extentZ = PackLightBVHExtent(attributes.extent.z);
    float coneAngle = clamp(attributes.cosConeAngle, -1.0f, 1.0f);
    uint packedDirection = EncodeLightBVHConeDirection(attributes.coneDirection);
    if (coneAngle > kLightBVHInvalidCosConeAngle &&
        dot(attributes.coneDirection, attributes.coneDirection) > 1e-12f)
    {
        float3 decodedDirection = DecodeLightBVHConeDirection(packedDirection);
        float directionError = acos(clamp(dot(normalize(attributes.coneDirection), decodedDirection), -1.0f, 1.0f));
        float widenedAngle = min(acos(coneAngle) + directionError, Pi);
        coneAngle = widenedAngle < Pi ? cos(widenedAngle) : kLightBVHInvalidCosConeAngle;
    }
    uint angle = uint(clamp((coneAngle + 1.0f) * 32767.0f, 0.0f, 65535.0f));
    node.data0.yzw = asuint(attributes.origin);
    node.data1.x = extentX | (extentY << 16u);
    node.data1.y = extentZ | (angle << 16u);
    node.data1.z = packedDirection;
    node.data1.w = asuint(attributes.flux);
}
