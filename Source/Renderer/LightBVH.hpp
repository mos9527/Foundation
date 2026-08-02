#pragma once
#include <Core/Container.hpp>
#include <Core/Logging.hpp>
#include <Math/Math.hpp>
#include <Math/Quantize.hpp>
#include <algorithm>
#include <bit>
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

struct LightBVHNode
{
    uint32_t header;
    float3 origin;
    float3 extent;
    float flux;
    float3 coneDirection;
    float cosConeAngle;
};

struct GSLightBVHNode
{
    uint32_t data[8]{};
};
static_assert(sizeof(GSLightBVHNode) == 32);

inline bool LightBVHNodeIsLeaf(LightBVHNode const& node)
{
    return (node.header & kLightBVHLeafBit) != 0u;
}

inline uint32_t LightBVHNodeRightChild(LightBVHNode const& node)
{
    return node.header; // MSB clear for internal nodes
}

inline uint32_t LightBVHNodeLightCount(LightBVHNode const& node)
{
    return (node.header >> kLightBVHLightOffsetBits) & ((1u << kLightBVHLightCountBits) - 1u);
}

inline uint32_t LightBVHNodeLightOffset(LightBVHNode const& node)
{
    return node.header & ((1u << kLightBVHLightOffsetBits) - 1u);
}

inline void LightBVHNodeSetAABB(LightBVHNode& node, float3 const& aabbMin, float3 const& aabbMax)
{
    node.origin = (aabbMax + aabbMin) * 0.5f;
    node.extent = (aabbMax - aabbMin) * 0.5f;
}

inline void LightBVHNodeGetAABB(LightBVHNode const& node, float3& aabbMin, float3& aabbMax)
{
    aabbMin = node.origin - node.extent;
    aabbMax = node.origin + node.extent;
}

inline void LightBVHNodeSetInternal(LightBVHNode& node, uint32_t rightChildIdx)
{
    node.header = rightChildIdx;
}

inline void LightBVHNodeSetLeaf(LightBVHNode& node, uint32_t lightCount, uint32_t lightOffset)
{
    node.header = kLightBVHLeafBit | (lightCount << kLightBVHLightOffsetBits) | lightOffset;
}

inline uint16_t PackLightBVHExtent(float extent)
{
    extent = std::max(extent, 0.0f);
    uint16_t packed = quantizeFP16(extent);
    if (dequantizeFP16(packed) < extent && packed < 0x7c00u)
        ++packed;
    return packed;
}

inline uint32_t PackLightBVHConeDirection(float3 direction)
{
    if (length(direction) <= 1e-12f)
        return 0u;
    direction /= std::abs(direction.x) + std::abs(direction.y) + std::abs(direction.z);
    float2 oct = direction.z >= 0.0f
        ? direction.xy()
        : (float2(1.0f) - abs(direction.yx())) * sign(direction.xy() + float2(1e-6f));
    uint32_t x = static_cast<uint16_t>(quantizeSnorm(oct.x, 16));
    uint32_t y = static_cast<uint16_t>(quantizeSnorm(oct.y, 16));
    return x | (y << 16u);
}

inline float3 UnpackLightBVHConeDirection(uint32_t packed)
{
    int16_t x = static_cast<int16_t>(packed & 0xffffu);
    int16_t y = static_cast<int16_t>(packed >> 16u);
    float2 oct = clamp(float2(static_cast<float>(x), static_cast<float>(y)) * (1.0f / 32767.0f),
                       float2(-1.0f), float2(1.0f));
    float3 normal(oct, 1.0f - std::abs(oct.x) - std::abs(oct.y));
    float2 xy = normal.z >= 1e-6f
        ? oct
        : (float2(1.0f) - abs(oct.yx())) * sign(oct + float2(1e-6f));
    return normalize(float3(xy.x, xy.y, normal.z));
}

inline GSLightBVHNode PackLightBVHNode(LightBVHNode const& node)
{
    GSLightBVHNode packed{};
    packed.data[0] = node.header;
    packed.data[1] = std::bit_cast<uint32_t>(node.origin.x);
    packed.data[2] = std::bit_cast<uint32_t>(node.origin.y);
    packed.data[3] = std::bit_cast<uint32_t>(node.origin.z);
    uint32_t extentX = PackLightBVHExtent(node.extent.x);
    uint32_t extentY = PackLightBVHExtent(node.extent.y);
    uint32_t extentZ = PackLightBVHExtent(node.extent.z);
    float angle = std::clamp(node.cosConeAngle, -1.0f, 1.0f);
    uint32_t packedDirection = PackLightBVHConeDirection(node.coneDirection);
    if (angle > kLightBVHInvalidCosConeAngle && length(node.coneDirection) > 1e-12f)
    {
        float3 decodedDirection = UnpackLightBVHConeDirection(packedDirection);
        float directionError = std::acos(std::clamp(dot(normalize(node.coneDirection), decodedDirection), -1.0f, 1.0f));
        float widenedAngle = std::min(std::acos(angle) + directionError, std::numbers::pi_v<float>);
        angle = widenedAngle < std::numbers::pi_v<float> ? std::cos(widenedAngle)
                                                         : kLightBVHInvalidCosConeAngle;
    }
    uint32_t packedAngle = static_cast<uint32_t>((angle + 1.0f) * 32767.0f);
    packed.data[4] = extentX | (extentY << 16u);
    packed.data[5] = extentZ | (packedAngle << 16u);
    packed.data[6] = packedDirection;
    packed.data[7] = std::bit_cast<uint32_t>(node.flux);
    return packed;
}

inline LightBVHNode UnpackLightBVHNode(GSLightBVHNode const& packed)
{
    LightBVHNode node{};
    node.header = packed.data[0];
    node.origin = float3(std::bit_cast<float>(packed.data[1]), std::bit_cast<float>(packed.data[2]),
                         std::bit_cast<float>(packed.data[3]));
    node.extent = float3(dequantizeFP16(static_cast<uint16_t>(packed.data[4])),
                         dequantizeFP16(static_cast<uint16_t>(packed.data[4] >> 16u)),
                         dequantizeFP16(static_cast<uint16_t>(packed.data[5])));
    node.cosConeAngle = static_cast<float>(packed.data[5] >> 16u) * (1.0f / 32767.0f) - 1.0f;
    node.coneDirection = UnpackLightBVHConeDirection(packed.data[6]);
    node.flux = std::bit_cast<float>(packed.data[7]);
    return node;
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
    Vector<LightBVHNode> nodes;
    Vector<GSLightBVHNode> gpuNodes;
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
        allocator(alloc), nodes(alloc), gpuNodes(alloc), lightIndices(alloc), lightBitmasks(alloc),
        globalLightIndices(alloc), nodeIndices(alloc), refitLevels(alloc)
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

[[nodiscard]] LightBVHBuild BuildLightBVH(Span<GSLight const> lights, Span<GSEmissiveCluster const> clusters,
                                          LightBVHOptions const& options, Allocator* alloc);

[[nodiscard]] bool ValidateLightBVH(LightBVHBuild const& bvh, Span<GSLight const> lights,
                                    Span<GSEmissiveCluster const> clusters, String* outError = nullptr);

[[nodiscard]] bool LightBVHRunBuilderSelfTests(Allocator* alloc, String* outError = nullptr);
