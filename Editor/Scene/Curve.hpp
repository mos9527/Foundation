#pragma once
#include <Core/Container.hpp>
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
enum class FCurveBasis : uint32_t
{
    Linear = 0,
    Bezier = 1,
    BSpline = 2,
    CatmullRom = 3,
};
enum class FCurveRenderMode : uint32_t
{
    Capsule = 0,
};
struct FCurveSet
{
    Vector<FCurvePoint> points;
    Vector<uint32_t> curveVertexCounts;
    FCurveBasis basis{FCurveBasis::Linear};
    FCurveRenderMode renderMode{FCurveRenderMode::Capsule};
    uint32_t materialIndex{0};

    FCurveSet(Allocator* alloc) : points(alloc), curveVertexCounts(alloc) {}
};
struct FSerializedCurve
{
    FBlobRef points;
    FBlobRef segments;
    FBlobRef aabbs;
    uint32_t materialIndex{0};
};
