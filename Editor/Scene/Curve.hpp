#pragma once
#include <Core/Container.hpp>
#include <Math/Math.hpp>
#include "Serialization.hpp"
using namespace Foundation;
using namespace Core;
using namespace Math;
struct FCurvePoint
{
    float3 position;
    float radius;
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
    FBlobRef curveVertexCounts;
    uint32_t numSegments{0};
    FCurveBasis basis{FCurveBasis::Linear};
    FCurveRenderMode renderMode{FCurveRenderMode::Capsule};
    uint32_t materialIndex{0};
};
template <>
inline void FSerialize(FWriter& w, FCurveSet const& obj)
{
    FSerialize(w, obj.points);
    FSerialize(w, obj.curveVertexCounts);
    FSerialize(w, obj.basis);
    FSerialize(w, obj.renderMode);
    FSerialize(w, obj.materialIndex);
}
template <>
inline void FDeserialize(FReader& r, FCurveSet& obj)
{
    FDeserialize(r, obj.points);
    FDeserialize(r, obj.curveVertexCounts);
    FDeserialize(r, obj.basis);
    FDeserialize(r, obj.renderMode);
    FDeserialize(r, obj.materialIndex);
}
