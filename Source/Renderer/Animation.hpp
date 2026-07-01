#pragma once
#include <Core/Container.hpp>
#include <Math/Math.hpp>
#include "Mesh.hpp"

using namespace Foundation;
using namespace Core;
using namespace Math;

/**
 * @file Animation.hpp
 * @brief A deliberately small transform-hierarchy + clip layer that drives CPU deformation.
 *
 * @details The whole system is one idea applied twice: a flat, topologically sorted joint
 *          hierarchy whose local TRS we sample from a clip and accumulate into world matrices
 *          (@ref FPose::globals). Skinning then multiplies those by per-joint inverse-bind
 *          matrices to build a palette and linear-blend-skins bind-pose vertices into the
 *          GPUScene dynamic ring (@ref SkinVertices). "Rigid articulation" is the same
 *          evaluator with the skinning step omitted: a caller reads @ref FPose::globals
 *          directly and feeds them as per-instance transforms (inverse-bind is then unused /
 *          identity). There is intentionally no general retained scene graph - the only
 *          hierarchy is this asset-local skeleton.
 */

/**
 * @brief One node of a transform hierarchy (a skeleton joint or an articulation node).
 * @note Joints are stored topologically: a joint's @ref parent index is always < its own
 *       index, so a single forward pass computes every world matrix (@ref ComputeGlobals).
 */
struct FJoint
{
    int32_t parent{-1};                 // parent joint index (< this joint's index), or -1 for a root
    float3 restTranslation{0, 0, 0};    // rest-pose local TRS (used when a channel is absent)
    quat restRotation{0, 0, 0, 1};
    float3 restScale{1, 1, 1};
    mat4 inverseBind{1.0f};             // bind space -> joint space; identity for pure articulation
};

/**
 * @brief A flat skeleton: the only hierarchy the animation layer retains, owned by the asset.
 */
struct FSkeleton
{
    Vector<FJoint> joints;
    explicit FSkeleton(Allocator* alloc = GLOBAL_ALLOC) : joints(alloc) {}
    [[nodiscard]] uint32_t Count() const { return static_cast<uint32_t>(joints.size()); }
};

/** @brief Which local TRS component an animation channel drives. */
enum class FAnimPath : uint8_t
{
    Translation,
    Rotation,
    Scale,
};

/** @brief Keyframe interpolation mode (mirrors glTF sampler interpolation). */
enum class FAnimInterp : uint8_t
{
    Step,
    Linear,
    CubicSpline,
};

/**
 * @brief A single animated component of a single joint: sorted keyframe times + packed values.
 * @details Values are packed component-major, one key after another:
 *          Translation/Scale store 3 floats/key, Rotation stores 4 (xyzw). @ref FAnimInterp::CubicSpline
 *          stores 3 sub-tuples/key in glTF order (in-tangent, value, out-tangent). Times are seconds,
 *          strictly ascending; sampling clamps to the first/last key (looping is the caller's choice).
 */
struct FAnimChannel
{
    uint32_t joint{0};
    FAnimPath path{FAnimPath::Translation};
    FAnimInterp interp{FAnimInterp::Linear};
    Vector<float> times;
    Vector<float> values;
    explicit FAnimChannel(Allocator* alloc = GLOBAL_ALLOC) : times(alloc), values(alloc) {}
};

/**
 * @brief A clip: a set of channels plus its total duration (seconds).
 * @note @ref skeleton is the index of the skeleton this clip drives in the scene's skeleton
 *       table; every channel's @ref FAnimChannel::joint indexes that skeleton.
 */
struct FAnimationClip
{
    Vector<FAnimChannel> channels;
    float duration{0.0f};
    int32_t skeleton{-1};
    explicit FAnimationClip(Allocator* alloc = GLOBAL_ALLOC) : channels(alloc) {}
};

/**
 * @brief Reusable per-instance evaluation scratch for one skeleton (allocate once, reuse per frame).
 * @details Holds the working local TRS (seeded from rest, overwritten by a clip) and the resulting
 *          world matrices. @ref globals is the shared output: skinning consumes it via
 *          @ref ComputeSkinningMatrices, rigid articulation reads it directly.
 */
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

/** @brief Composes a joint's local TRS into a matrix (T * R * S). */
[[nodiscard]] mat4 JointLocalMatrix(float3 const& t, quat const& r, float3 const& s);

/** @brief Seeds a pose's local TRS from the skeleton rest pose (call before @ref SampleClip). */
void ResetToRest(FSkeleton const& skel, FPose& pose);

/**
 * @brief Samples a clip at time @p t, overwriting the seeded local TRS for every channel's joint.
 * @note Channels with no key for a component leave that component at its rest value, so call
 *       @ref ResetToRest first. @p t is clamped per channel to its key range.
 */
void SampleClip(FAnimationClip const& clip, float t, FPose& pose);

/** @brief Walks the hierarchy, composing local TRS into world matrices in @ref FPose::globals. */
void ComputeGlobals(FSkeleton const& skel, FPose& pose);

/**
 * @brief Samples a generic keyframed track of @p comps-wide values at time @p t into @p out.
 * @details Same time/value packing and interpolation rules as @ref FAnimChannel (step/linear/cubic,
 *          clamped to the key range). Used for morph-target weight tracks (comps == target count).
 * @param out Must be exactly @p comps wide.
 */
void SampleTrack(Span<const float> times, Span<const float> values, uint32_t comps, FAnimInterp interp, float t,
                 Span<float> out);

/**
 * @brief Builds the skinning palette `globals[j] * inverseBind[j]` into @p outPalette.
 * @param outPalette Must hold @ref FSkeleton::Count matrices.
 */
void ComputeSkinningMatrices(FSkeleton const& skel, FPose const& pose, Span<mat4> outPalette);

/**
 * @brief Linear-blend skins bind-pose vertices by the palette, packing results for the dynamic ring.
 * @param bind    Rest-pose, full-precision vertices (positions/normals/tangents in bind space).
 * @param binding Per-vertex joint indices + weights (parallel to @p bind).
 * @param palette Skinning matrices from @ref ComputeSkinningMatrices.
 * @param out     Destination quantized vertices (size == bind.size()); write straight into the
 *                span returned by @ref GPUScene::UpdateDynamicGeometry.
 * @param outBounds Optional local-space bounds of the skinned output positions.
 */
void SkinVertices(Span<const FVertex> bind, Span<const FSkinBinding> binding, Span<const mat4> palette,
                  Span<FQVertex> out, FSerializedBounds* outBounds = nullptr);
