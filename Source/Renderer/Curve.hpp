#pragma once
#include <Math/Math.hpp>
#include "Serialization.hpp"
using namespace Foundation;
using namespace Core;
using namespace Math;
#pragma pack(push, 1)
struct FCurvePoint
{
    float3 position;
    float radius;
};
struct FSerializedCurveSegment
{
    uint32_t p0;
    uint32_t p1;
    float u0;
    float u1;
};
#pragma pack(pop)
struct FSerializedCurveAABB
{
    float minX, minY, minZ;
    float maxX, maxY, maxZ;
};
struct FSerializedCurve
{
    FBlobRef points;
    FBlobRef segments;
    FBlobRef aabbs;
    uint32_t materialIndex{0};
};
