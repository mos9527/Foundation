#pragma once
#include <Renderer/Curve.hpp>

// One tapered line segment prior to DOTS bake (from glTF LINES + _RADIUS).
struct FImportedCurveSegment
{
    float3 p0;
    float r0;
    float3 p1;
    float r1;
    float u0;
    float u1;
};

struct FImportedCurve
{
    Vector<FImportedCurveSegment> segments;

    FImportedCurve(Allocator* alloc) : segments(alloc) {}
};
