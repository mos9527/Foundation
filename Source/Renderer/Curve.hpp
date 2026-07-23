#pragma once
#include <Math/Math.hpp>
#include "Metadata.hpp"
#include "Serialization.hpp"
using namespace Foundation;
using namespace Core;
using namespace Math;
#pragma pack(push, 1)
// Triangle BLAS vertex for DOTS.
struct FCurveDOTSVertex
{
    uint16_t position[4]; // quantized FP16 [xyz] padding [w]
};
// Tapered capsule leaf reconstructed at shading time (primitiveIndex / 4).
struct FCurveLeaf
{
    float3 p0;
    float r0;
    float3 p1;
    float r1;
    float u0;
    float u1;
};
#pragma pack(pop)
struct FSerializedCurve
{
    FUUID id{};
    FSerializedBounds bounds;
    FBlobRef vertices; // FCurveDOTSVertex
    FBlobRef indices;  // uint32_t, 12 indices (4 tris) per leaf
    FBlobRef leaves;   // FCurveLeaf
};
