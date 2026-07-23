#pragma once
#include <Core/Container.hpp>
#include <Core/Logging.hpp>
#include <Math/Math.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include "GPUScene.hpp"

using namespace Foundation;
using namespace Core;
using namespace Math;

inline constexpr float kLightBVHInvalidCosConeAngle = -1.0f;
inline constexpr uint32_t kLightBVHMaxDepth = 64u;
inline constexpr uint32_t kLightBVHLightCountBits = 4u;
inline constexpr uint32_t kLightBVHLightOffsetBits = 31u - kLightBVHLightCountBits;
inline constexpr uint32_t kLightBVHMaxLightsPerLeaf = 1u << kLightBVHLightCountBits;
inline constexpr uint32_t kLightBVHMaxLightOffset = 1u << kLightBVHLightOffsetBits;
inline constexpr uint32_t kLightBVHLeafBit = 1u << 31;
inline constexpr float kLightBVHMinSunAngularRadius = 1e-4f;

enum class LightBVHSplitHeuristic : uint32_t
{
    Equal = 0u,
    BinnedSAH = 1u,
    BinnedSAOH = 2u,
};

struct LightBVHOptions
{
    LightBVHSplitHeuristic splitHeuristic = LightBVHSplitHeuristic::BinnedSAOH;
    uint32_t binCount = 16u;
    float volumeEpsilon = 1e-3f;
    bool splitAlongLargest = false;
    bool useVolumeOverSA = false;
    bool usePreintegration = true;
    bool useLightingCones = true;
};

#pragma pack(push, 1)
// TODO Pack/compress should emissive triangles also participate in NEE
struct GSLightBVHNode
{
    uint32_t header;
    float3 origin;
    float3 extent;
    float flux;
    float3 coneDirection;
    float cosConeAngle;
};
#pragma pack(pop)
static_assert(sizeof(GSLightBVHNode) == 48);

inline bool GSLightBVHNodeIsLeaf(GSLightBVHNode const& node)
{
    return (node.header & kLightBVHLeafBit) != 0u;
}

inline uint32_t GSLightBVHNodeRightChild(GSLightBVHNode const& node)
{
    return node.header; // MSB clear for internal nodes
}

inline uint32_t GSLightBVHNodeLightCount(GSLightBVHNode const& node)
{
    return (node.header >> kLightBVHLightOffsetBits) & ((1u << kLightBVHLightCountBits) - 1u);
}

inline uint32_t GSLightBVHNodeLightOffset(GSLightBVHNode const& node)
{
    return node.header & ((1u << kLightBVHLightOffsetBits) - 1u);
}

inline void GSLightBVHNodeSetAABB(GSLightBVHNode& node, float3 const& aabbMin, float3 const& aabbMax)
{
    node.origin = (aabbMax + aabbMin) * 0.5f;
    node.extent = (aabbMax - aabbMin) * 0.5f;
}

inline void GSLightBVHNodeGetAABB(GSLightBVHNode const& node, float3& aabbMin, float3& aabbMax)
{
    aabbMin = node.origin - node.extent;
    aabbMax = node.origin + node.extent;
}

inline void GSLightBVHNodeSetInternal(GSLightBVHNode& node, uint32_t rightChildIdx)
{
    node.header = rightChildIdx;
}

inline void GSLightBVHNodeSetLeaf(GSLightBVHNode& node, uint32_t lightCount, uint32_t lightOffset)
{
    node.header = kLightBVHLeafBit | (lightCount << kLightBVHLightOffsetBits) | lightOffset;
}

using LightBVHRefitLevel = GSOffsetCount;

struct LightBVHStats
{
    uint32_t treeHeight = 0;
    uint32_t minDepth = 0;
    uint32_t byteSize = 0;
    uint32_t internalNodeCount = 0;
    uint32_t leafNodeCount = 0;
    uint32_t finiteLightCount = 0;
    uint32_t globalLightCount = 0;
};

struct LightBVHBuild
{
    Allocator* allocator = nullptr;
    Vector<GSLightBVHNode> nodes;
    Vector<uint32_t> lightIndices;
    Vector<uint64_t> lightBitmasks;
    Vector<uint32_t> globalLightIndices;
    Vector<uint32_t> nodeIndices;
    Vector<LightBVHRefitLevel> refitLevels;
    LightBVHStats stats{};
    uint32_t distantRootNode = UINT32_MAX;
    uint32_t distantNodeCount = 0;
    uint32_t finiteLightIndexCount = 0;
    bool valid = false;

    explicit LightBVHBuild(Allocator* alloc) :
        allocator(alloc), nodes(alloc), lightIndices(alloc), lightBitmasks(alloc), globalLightIndices(alloc),
        nodeIndices(alloc), refitLevels(alloc)
    {
    }
};

[[nodiscard]] inline uint32_t GSLightTypeCPU(GSLight const& light)
{
    return light.flags & kGSLightTypeMask;
}

[[nodiscard]] inline bool IsDistantLightType(uint32_t type)
{
    return type == kGSLightTypeDirectional || type == kGSLightTypeEnvironment;
}

[[nodiscard]] inline bool IsFiniteLightType(uint32_t type)
{
    return type == kGSLightTypePoint || type == kGSLightTypeSpot || type == kGSLightTypeDisk ||
           type == kGSLightTypeRect;
}

[[nodiscard]] inline bool HasLightEmission(GSLight const& light)
{
    return light.power > 0.0f && any(greaterThan(light.color, float3(0.0f)));
}
// NOTE: Must match ComputeLightProposalWeight in ICommon.slang
[[nodiscard]] inline float ComputeLightProposalWeight(GSLight const& light)
{
    float colorWeight = (std::abs(light.color.x) + std::abs(light.color.y) + std::abs(light.color.z)) / 3.0f;
    float radiometricWeight = std::max(0.0f, light.power) * colorWeight;
    uint32_t type = GSLightTypeCPU(light);
    if (type == kGSLightTypeDirectional)
    {
        return radiometricWeight;
    }
    if (type == kGSLightTypeEnvironment)
    {
        return radiometricWeight * light.params.y * std::numbers::pi_v<float>;
    }
    if (type == kGSLightTypePoint)
        return radiometricWeight * (4.0f * std::numbers::pi_v<float>);
    if (type == kGSLightTypeSpot)
    {
        float cosInner = std::clamp(std::max(light.params.y, light.params.z), -1.0f, 1.0f);
        float cosOuter = std::clamp(std::min(light.params.y, light.params.z), -1.0f, cosInner);
        float solidAngle = 2.0f * std::numbers::pi_v<float> *
            ((1.0f - cosInner) + (cosInner - cosOuter) / 5.0f);
        return radiometricWeight * solidAngle;
    }
    if (type == kGSLightTypeDisk || type == kGSLightTypeRect)
    {
        float area = type == kGSLightTypeDisk
            ? std::numbers::pi_v<float> * light.params.x * light.params.y
            : 4.0f * length(cross(light.dpdu, light.dpdv));
        float sides = (light.flags & to_integer(GSLightFlagsBits::TwoSided)) != 0u ? 2.0f : 1.0f;
        return radiometricWeight * area * std::numbers::pi_v<float> * sides;
    }
    return 0.0f;
}

void ComputeAnalyticalLightBounds(GSLight const& light, float3& aabbMin, float3& aabbMax, float3& center,
                                  float3& coneDirection, float& cosConeAngle);

[[nodiscard]] LightBVHBuild BuildLightBVH(Span<GSLight const> lights, LightBVHOptions const& options, Allocator* alloc);

[[nodiscard]] bool ValidateLightBVH(LightBVHBuild const& bvh, Span<GSLight const> lights, String* outError = nullptr);

[[nodiscard]] bool LightBVHRunBuilderSelfTests(Allocator* alloc, String* outError = nullptr);
