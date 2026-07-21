#pragma once
#include <Core/Container.hpp>
#include <Math/Math.hpp>
#include "Mesh.hpp"

using namespace Foundation;
using namespace Core;
using namespace Math;

// Flat, topologically sorted joint hierarchy (parent < self): one forward pass yields world matrices.
struct FJoint
{
    int32_t parent{-1};
    float3 restTranslation{0, 0, 0};
    quat restRotation{0, 0, 0, 1};
    float3 restScale{1, 1, 1};
    mat4 inverseBind{1.0f}; // identity for pure articulation nodes
};

struct FSkeleton
{
    FUUID id{};
    Vector<FJoint> joints;
    explicit FSkeleton(Allocator* alloc = GLOBAL_ALLOC) : joints(alloc) {}
    [[nodiscard]] uint32_t Count() const { return static_cast<uint32_t>(joints.size()); }
};

enum class FAnimPath : uint8_t { Translation, Rotation, Scale };
enum class FAnimInterp : uint8_t { Step, Linear, CubicSpline };

// Component-major keyframes: Translation/Scale = 3 floats/key, Rotation = 4 (xyzw). Cubic = [in,value,out]*comp.
struct FAnimChannel
{
    uint32_t joint{0};
    FAnimPath path{FAnimPath::Translation};
    FAnimInterp interp{FAnimInterp::Linear};
    Vector<float> times;
    Vector<float> values;
    explicit FAnimChannel(Allocator* alloc = GLOBAL_ALLOC) : times(alloc), values(alloc) {}
};

// TRS channels driving one skeleton; @ref skeleton is kNilUUID for a degenerate (no-op) clip.
struct FAnimationClip
{
    FUUID id{};
    FUUID name{};
    Vector<FAnimChannel> channels;
    float duration{0.0f};
    FUUID skeleton{};
    explicit FAnimationClip(Allocator* alloc = GLOBAL_ALLOC) : channels(alloc) {}
};

// Per-skeleton evaluation scratch: seeded from rest, overwritten by clips, accumulated into globals.
struct FPose
{
    Vector<float3> translations;
    Vector<quat> rotations;
    Vector<float3> scales;
    Vector<mat4> globals;
    explicit FPose(Allocator* alloc = GLOBAL_ALLOC)
        : translations(alloc), rotations(alloc), scales(alloc), globals(alloc)
    {
    }
    void Resize(uint32_t jointCount);
};

[[nodiscard]] mat4 JointLocalMatrix(float3 const& t, quat const& r, float3 const& s);
void ResetToRest(FSkeleton const& skel, FPose& pose);
void SampleClip(FAnimationClip const& clip, float t, FPose& pose);
void BlendClip(FAnimationClip const& clip, float t, float weight, FPose& pose);
void ComputeGlobals(FSkeleton const& skel, FPose& pose);
void ComputeSkinningMatrices(FSkeleton const& skel, FPose const& pose, Span<mat4> outPalette);

// CPU LBS fallback / validation path; GPU skinning is the primary deformation route.
void SkinVertices(Span<const FVertex> bind, Span<const FSkinBinding> binding, Span<const mat4> palette,
                  Span<FQVertex> out, FSerializedBounds* outBounds = nullptr);
