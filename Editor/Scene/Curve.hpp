#pragma once
#include <Renderer/Curve.hpp>

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
struct FImportedCurve
{
    Vector<FCurvePoint> points;
    Vector<uint32_t> curveVertexCounts;
    FCurveBasis basis{FCurveBasis::Linear};
    FCurveRenderMode renderMode{FCurveRenderMode::Capsule};

    FImportedCurve(Allocator* alloc) : points(alloc), curveVertexCounts(alloc) {}
};
