#include "LightBVH.hpp"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <limits>
#include <numbers>
#include <stack>
#include <vector>

namespace
{
struct AABB
{
    float3 minPoint{std::numeric_limits<float>::max()};
    float3 maxPoint{std::numeric_limits<float>::lowest()};

    bool valid() const { return all(lessThanEqual(minPoint, maxPoint)); }
    float3 center() const { return (minPoint + maxPoint) * 0.5f; }
    float3 extent() const { return maxPoint - minPoint; }
    float area() const
    {
        float3 d = extent();
        return 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x);
    }

    AABB& operator|=(float3 const& p)
    {
        minPoint = min(minPoint, p);
        maxPoint = max(maxPoint, p);
        return *this;
    }

    AABB& operator|=(AABB const& other)
    {
        if (!other.valid())
            return *this;
        minPoint = min(minPoint, other.minPoint);
        maxPoint = max(maxPoint, other.maxPoint);
        return *this;
    }
};

struct Range
{
    uint32_t begin = 0;
    uint32_t end = 0;
    uint32_t middle() const { return (begin + end) / 2u; }
    uint32_t length() const { return end - begin; }
};

struct SplitResult
{
    uint32_t axis = ~0u;
    uint32_t lightIndex = ~0u;
    bool isValid() const { return axis != ~0u && lightIndex != ~0u; }
};

struct LightSortData
{
    AABB bounds;
    float3 center{};
    float3 coneDirection{};
    float cosConeAngle = 1.0f;
    float flux = 0.0f;
    uint32_t lightIndex = ~0u;
};

struct BuildingData
{
    Allocator* alloc;
    Vector<GSLightBVHNode>& nodes;
    Vector<LightSortData> lightsData;
    Vector<uint32_t> lightIndices;
    Vector<uint64_t> lightBitmasks;

    BuildingData(Vector<GSLightBVHNode>& bvhNodes, Allocator* allocator) :
        alloc(allocator), nodes(bvhNodes), lightsData(allocator), lightIndices(allocator), lightBitmasks(allocator)
    {
    }
};

float safeACos(float v)
{
    return std::acos(std::clamp(v, -1.0f, 1.0f));
}

float computeCosConeAngle(float3 const& coneDir, float cosTheta, float3 const& otherConeDir, float cosOtherTheta)
{
    if (cosTheta == kLightBVHInvalidCosConeAngle || cosOtherTheta == kLightBVHInvalidCosConeAngle)
        return kLightBVHInvalidCosConeAngle;

    float const currentAngle = safeACos(cosTheta);
    float const otherAngle = safeACos(dot(coneDir, otherConeDir)) + safeACos(cosOtherTheta);
    float const resultAngle = std::max(currentAngle, otherAngle);
    return resultAngle < std::numbers::pi_v<float> ? std::cos(resultAngle) : kLightBVHInvalidCosConeAngle;
}

float3 coneUnionOld(float3 aDir, float aCosTheta, float3 bDir, float bCosTheta, float& cosResult)
{
    float3 dir = aDir + bDir;
    if (aCosTheta == kLightBVHInvalidCosConeAngle || bCosTheta == kLightBVHInvalidCosConeAngle ||
        all(equal(dir, float3(0.0f))))
    {
        cosResult = kLightBVHInvalidCosConeAngle;
        return float3(0.0f);
    }

    dir = normalize(dir);
    float const aDiff = safeACos(dot(dir, aDir));
    float const bDiff = safeACos(dot(dir, bDir));
    float const resultAngle = std::max(aDiff + safeACos(aCosTheta), bDiff + safeACos(bCosTheta));
    if (resultAngle >= std::numbers::pi_v<float>)
    {
        cosResult = kLightBVHInvalidCosConeAngle;
        return float3(0.0f);
    }
    cosResult = std::cos(resultAngle);
    return dir;
}

float aabbVolume(AABB const& bb, float epsilon)
{
    if (!bb.valid())
        return -std::numeric_limits<float>::infinity();
    float3 dims = max(float3(epsilon), bb.extent());
    return dims.x * dims.y * dims.z;
}

float evalSAH(AABB const& bounds, uint32_t lightCount, LightBVHOptions const& parameters)
{
    float aabbCost = bounds.valid() ? (parameters.useVolumeOverSA ? aabbVolume(bounds, parameters.volumeEpsilon)
                                                                  : bounds.area())
                                    : 0.0f;
    return aabbCost * static_cast<float>(lightCount);
}

float computeOrientationCost(float theta_o)
{
    float theta_w = std::min(theta_o + std::numbers::pi_v<float> * 0.5f, std::numbers::pi_v<float>);
    float sin_theta_o = std::sin(theta_o);
    float cos_theta_o = std::cos(theta_o);
    return 2.0f * std::numbers::pi_v<float> * (1.0f - cos_theta_o) +
           0.5f * std::numbers::pi_v<float> *
               (2.0f * theta_w * sin_theta_o - std::cos(theta_o - 2.0f * theta_w) - 2.0f * theta_o * sin_theta_o +
                cos_theta_o);
}

float evalSAOH(AABB const& bounds, float flux, float cosTheta, LightBVHOptions const& parameters)
{
    float fluxCost = parameters.usePreintegration ? flux : 1.0f;
    float aabbCost = bounds.valid() ? (parameters.useVolumeOverSA ? aabbVolume(bounds, parameters.volumeEpsilon)
                                                                  : bounds.area())
                                    : 0.0f;
    float theta = cosTheta != kLightBVHInvalidCosConeAngle ? safeACos(cosTheta) : std::numbers::pi_v<float>;
    float orientationCost = parameters.useLightingCones ? computeOrientationCost(theta) : 1.0f;
    return fluxCost * aabbCost * orientationCost;
}

float3 computeLightingCone(Range const& lightRange, BuildingData const& data, float& cosTheta)
{
    float3 coneDirection{};
    cosTheta = kLightBVHInvalidCosConeAngle;

    float3 coneDirectionSum{};
    for (uint32_t i = lightRange.begin; i < lightRange.end; ++i)
        coneDirectionSum += data.lightsData[i].coneDirection;

    if (length(coneDirectionSum) >= FLT_MIN)
    {
        coneDirection = normalize(coneDirectionSum);
        cosTheta = 1.0f;
        for (uint32_t i = lightRange.begin; i < lightRange.end; ++i)
        {
            LightSortData const& ld = data.lightsData[i];
            cosTheta = computeCosConeAngle(coneDirection, cosTheta, ld.coneDirection, ld.cosConeAngle);
        }
    }
    return coneDirection;
}

float3 computeLightingConesInternal(uint32_t nodeIndex, BuildingData& data, float& cosConeAngle)
{
    if (!GSLightBVHNodeIsLeaf(data.nodes[nodeIndex]))
    {
        uint32_t leftIndex = nodeIndex + 1u;
        uint32_t rightIndex = GSLightBVHNodeRightChild(data.nodes[nodeIndex]);

        float leftCos = kLightBVHInvalidCosConeAngle;
        float3 leftDir = computeLightingConesInternal(leftIndex, data, leftCos);
        float rightCos = kLightBVHInvalidCosConeAngle;
        float3 rightDir = computeLightingConesInternal(rightIndex, data, rightCos);

        float3 coneDirection = coneUnionOld(leftDir, leftCos, rightDir, rightCos, cosConeAngle);
        data.nodes[nodeIndex].cosConeAngle = cosConeAngle;
        data.nodes[nodeIndex].coneDirection = coneDirection;
        return coneDirection;
    }

    cosConeAngle = data.nodes[nodeIndex].cosConeAngle;
    return data.nodes[nodeIndex].coneDirection;
}

SplitResult computeSplitWithEqual(BuildingData const&, Range const& lightRange, AABB const& nodeBounds,
                                  LightBVHOptions const&)
{
    float3 dimensions = nodeBounds.extent();
    uint32_t dimension = dimensions.z >= dimensions.x && dimensions.z >= dimensions.y
                             ? 2u
                             : (dimensions.y >= dimensions.x ? 1u : 0u);
    SplitResult result;
    result.axis = dimension;
    result.lightIndex = lightRange.middle();
    return result;
}

SplitResult computeSplitWithBinnedSAH(BuildingData const& data, Range const& lightRange, AABB const& nodeBounds,
                                      LightBVHOptions const& parameters)
{
    struct Bin
    {
        AABB bounds;
        uint32_t lightCount = 0;

        Bin& operator|=(LightSortData const& light)
        {
            bounds |= light.bounds;
            ++lightCount;
            return *this;
        }

        Bin& operator|=(Bin const& rhs)
        {
            bounds |= rhs.bounds;
            lightCount += rhs.lightCount;
            return *this;
        }
    };

    std::pair<float, SplitResult> overallBest = {std::numeric_limits<float>::infinity(), {}};
    Vector<Bin> bins(parameters.binCount, data.alloc);
    Vector<float> costs(parameters.binCount - 1u, data.alloc);

    auto binAlongDimension = [&](uint32_t dimension)
    {
        auto getBinId = [&](LightSortData const& ld)
        {
            float bmin = nodeBounds.minPoint[dimension];
            float bmax = nodeBounds.maxPoint[dimension];
            float w = bmax - bmin;
            float scale = w > FLT_MIN ? static_cast<float>(parameters.binCount) / w : 0.0f;
            float p = ld.bounds.center()[dimension];
            return std::min(static_cast<uint32_t>((p - bmin) * scale), parameters.binCount - 1u);
        };

        for (Bin& bin : bins)
            bin = Bin{};
        for (uint32_t i = lightRange.begin; i < lightRange.end; ++i)
            bins[getBinId(data.lightsData[i])] |= data.lightsData[i];

        Bin total{};
        for (size_t i = 0; i < costs.size(); ++i)
        {
            total |= bins[i];
            costs[i] = evalSAH(total.bounds, total.lightCount, parameters);
        }
        total = Bin{};
        for (size_t i = costs.size(); i > 0; --i)
        {
            total |= bins[i];
            costs[i - 1] += evalSAH(total.bounds, total.lightCount, parameters);
        }

        std::pair<float, SplitResult> axisBest = {std::numeric_limits<float>::infinity(),
                                                  SplitResult{dimension, 0}};
        for (uint32_t i = 0, lightIdx = lightRange.begin; i < costs.size(); ++i)
        {
            lightIdx += bins[i].lightCount;
            if (costs[i] < axisBest.first)
                axisBest = {costs[i], SplitResult{dimension, lightIdx}};
        }
        if (axisBest.second.lightIndex == lightRange.begin || axisBest.second.lightIndex == lightRange.end)
            return;
        if (axisBest.first < overallBest.first)
            overallBest = axisBest;
    };

    if (parameters.splitAlongLargest)
    {
        float3 dimensions = nodeBounds.extent();
        uint32_t largest = dimensions.z >= dimensions.x && dimensions.z >= dimensions.y
                               ? 2u
                               : (dimensions.y >= dimensions.x ? 1u : 0u);
        binAlongDimension(largest);
    }
    else
    {
        for (uint32_t dimension = 0; dimension < 3; ++dimension)
            binAlongDimension(dimension);
    }

    if (!overallBest.second.isValid())
        return computeSplitWithEqual(data, lightRange, nodeBounds, parameters);
    return overallBest.second;
}

SplitResult computeSplitWithBinnedSAOH(BuildingData const& data, Range const& lightRange, AABB const& nodeBounds,
                                       LightBVHOptions const& parameters)
{
    struct Bin
    {
        AABB bounds;
        uint32_t lightCount = 0;
        float flux = 0.0f;
        float3 coneDirection{};
        float cosConeAngle = 1.0f;

        Bin& operator|=(LightSortData const& light)
        {
            bounds |= light.bounds;
            ++lightCount;
            flux += light.flux;
            coneDirection += light.coneDirection;
            return *this;
        }

        Bin& operator|=(Bin const& rhs)
        {
            bounds |= rhs.bounds;
            lightCount += rhs.lightCount;
            flux += rhs.flux;
            coneDirection += rhs.coneDirection;
            return *this;
        }
    };

    float3 dimensions = nodeBounds.extent();
    uint32_t largestDimension = dimensions.z >= dimensions.x && dimensions.z >= dimensions.y
                                    ? 2u
                                    : (dimensions.y >= dimensions.x ? 1u : 0u);

    std::pair<float, SplitResult> overallBest = {std::numeric_limits<float>::infinity(), {}};
    Vector<Bin> bins(parameters.binCount, data.alloc);
    Vector<float> costs(parameters.binCount - 1u, data.alloc);

    auto binAlongDimension = [&](uint32_t dimension)
    {
        auto getBinId = [&](LightSortData const& ld)
        {
            float bmin = nodeBounds.minPoint[dimension];
            float bmax = nodeBounds.maxPoint[dimension];
            float w = bmax - bmin;
            float scale = w > FLT_MIN ? static_cast<float>(parameters.binCount) / w : 0.0f;
            float p = ld.bounds.center()[dimension];
            return std::min(static_cast<uint32_t>((p - bmin) * scale), parameters.binCount - 1u);
        };

        for (Bin& bin : bins)
            bin = Bin{};
        for (uint32_t i = lightRange.begin; i < lightRange.end; ++i)
            bins[getBinId(data.lightsData[i])] |= data.lightsData[i];

        for (Bin& bin : bins)
        {
            bin.cosConeAngle = length(bin.coneDirection) < FLT_MIN ? kLightBVHInvalidCosConeAngle : 1.0f;
            if (bin.cosConeAngle != kLightBVHInvalidCosConeAngle)
                bin.coneDirection = normalize(bin.coneDirection);
        }
        for (uint32_t i = lightRange.begin; i < lightRange.end; ++i)
        {
            LightSortData const& ld = data.lightsData[i];
            Bin& bin = bins[getBinId(ld)];
            bin.cosConeAngle = computeCosConeAngle(bin.coneDirection, bin.cosConeAngle, ld.coneDirection,
                                                   ld.cosConeAngle);
        }

        Bin total{};
        for (size_t i = 0; i < costs.size(); ++i)
        {
            total |= bins[i];
            float cosTheta = kLightBVHInvalidCosConeAngle;
            if (length(total.coneDirection) >= FLT_MIN)
            {
                cosTheta = 1.0f;
                float3 coneDir = normalize(total.coneDirection);
                for (size_t j = 0; j <= i; ++j)
                    cosTheta = computeCosConeAngle(coneDir, cosTheta, bins[j].coneDirection, bins[j].cosConeAngle);
            }
            costs[i] = evalSAOH(total.bounds, total.flux, cosTheta, parameters);
        }

        total = Bin{};
        for (size_t i = costs.size(); i > 0; --i)
        {
            total |= bins[i];
            float cosTheta = kLightBVHInvalidCosConeAngle;
            if (length(total.coneDirection) >= FLT_MIN)
            {
                cosTheta = 1.0f;
                float3 coneDir = normalize(total.coneDirection);
                for (size_t j = i; j <= costs.size(); ++j)
                    cosTheta = computeCosConeAngle(coneDir, cosTheta, bins[j].coneDirection, bins[j].cosConeAngle);
            }
            costs[i - 1] += evalSAOH(total.bounds, total.flux, cosTheta, parameters);
        }

        std::pair<float, SplitResult> axisBest = {std::numeric_limits<float>::infinity(),
                                                  SplitResult{dimension, 0}};
        for (uint32_t i = 0, lightIdx = lightRange.begin; i < costs.size(); ++i)
        {
            lightIdx += bins[i].lightCount;
            if (costs[i] < axisBest.first)
                axisBest = {costs[i], SplitResult{dimension, lightIdx}};
        }
        axisBest.first *= dimensions[largestDimension] / std::max(dimensions[dimension], FLT_MIN);
        if (axisBest.second.lightIndex == lightRange.begin || axisBest.second.lightIndex == lightRange.end)
            return;
        if (axisBest.first < overallBest.first)
            overallBest = axisBest;
    };

    if (parameters.splitAlongLargest)
        binAlongDimension(largestDimension);
    else
    {
        for (uint32_t dimension = 0; dimension < 3; ++dimension)
            binAlongDimension(dimension);
    }

    if (!overallBest.second.isValid())
        return computeSplitWithEqual(data, lightRange, nodeBounds, parameters);
    return overallBest.second;
}

using SplitFn = SplitResult (*)(BuildingData const&, Range const&, AABB const&, LightBVHOptions const&);

SplitFn getSplitFunction(LightBVHSplitHeuristic heuristic)
{
    switch (heuristic)
    {
    case LightBVHSplitHeuristic::Equal:
        return computeSplitWithEqual;
    case LightBVHSplitHeuristic::BinnedSAH:
        return computeSplitWithBinnedSAH;
    case LightBVHSplitHeuristic::BinnedSAOH:
    default:
        return computeSplitWithBinnedSAOH;
    }
}

uint32_t buildInternal(LightBVHOptions const& options, SplitFn splitHeuristic, uint64_t bitmask, uint32_t depth,
                       Range const& lightRange, BuildingData& data)
{
    CHECK(lightRange.begin < lightRange.end);

    float nodeFlux = 0.0f;
    AABB nodeBounds;
    for (uint32_t i = lightRange.begin; i < lightRange.end; ++i)
    {
        nodeBounds |= data.lightsData[i].bounds;
        nodeFlux += data.lightsData[i].flux;
    }
    CHECK(nodeBounds.valid());

    bool trySplitting = lightRange.length() > 1u;
    SplitResult splitResult = trySplitting ? splitHeuristic(data, lightRange, nodeBounds, options) : SplitResult{};

    if (splitResult.isValid())
    {
        CHECK(lightRange.begin < splitResult.lightIndex && splitResult.lightIndex < lightRange.end);
        auto comp = [dim = splitResult.axis](LightSortData const& a, LightSortData const& b)
        { return a.bounds.center()[dim] < b.bounds.center()[dim]; };
        std::nth_element(data.lightsData.begin() + lightRange.begin, data.lightsData.begin() + splitResult.lightIndex,
                         data.lightsData.begin() + lightRange.end, comp);

        uint32_t nodeIndex = static_cast<uint32_t>(data.nodes.size());
        data.nodes.push_back({});
        GSLightBVHNode& node = data.nodes.back();
        GSLightBVHNodeSetAABB(node, nodeBounds.minPoint, nodeBounds.maxPoint);
        node.flux = nodeFlux;
        node.coneDirection = float3(0.0f);
        node.cosConeAngle = kLightBVHInvalidCosConeAngle;

        CHECK_MSG(depth < kLightBVHMaxDepth, "Light BVH depth {} exceeds max {}", depth + 1u, kLightBVHMaxDepth);

        uint32_t leftIndex =
            buildInternal(options, splitHeuristic, bitmask | (0ull << depth), depth + 1u,
                          Range{lightRange.begin, splitResult.lightIndex}, data);
        uint32_t rightIndex =
            buildInternal(options, splitHeuristic, bitmask | (1ull << depth), depth + 1u,
                          Range{splitResult.lightIndex, lightRange.end}, data);
        CHECK(leftIndex == nodeIndex + 1u);
        GSLightBVHNodeSetInternal(data.nodes[nodeIndex], rightIndex);
        return nodeIndex;
    }

    CHECK(lightRange.length() == 1u);
    uint32_t nodeIndex = static_cast<uint32_t>(data.nodes.size());
    data.nodes.push_back({});
    GSLightBVHNode& node = data.nodes.back();
    GSLightBVHNodeSetAABB(node, nodeBounds.minPoint, nodeBounds.maxPoint);
    node.flux = nodeFlux;
    float cosTheta = kLightBVHInvalidCosConeAngle;
    node.coneDirection = computeLightingCone(lightRange, data, cosTheta);
    node.cosConeAngle = cosTheta;

    uint32_t lightCount = lightRange.length();
    uint32_t lightOffset = static_cast<uint32_t>(data.lightIndices.size());
    CHECK(lightCount < kLightBVHMaxLightsPerLeaf);
    CHECK(lightOffset < kLightBVHMaxLightOffset);
    GSLightBVHNodeSetLeaf(node, lightCount, lightOffset);

    for (uint32_t i = lightRange.begin; i < lightRange.end; ++i)
    {
        uint32_t globalIndex = data.lightsData[i].lightIndex;
        data.lightIndices.push_back(globalIndex);
        data.lightBitmasks[globalIndex] = bitmask;
    }
    return nodeIndex;
}

void finalizeNodeIndices(LightBVHBuild& bvh)
{
    if (!bvh.valid || bvh.nodes.empty())
        return;

    CHECK(bvh.allocator);
    bvh.stats = {};
    bvh.stats.byteSize = static_cast<uint32_t>(bvh.nodes.size() * sizeof(GSLightBVHNode));
    bvh.stats.finiteLightCount = static_cast<uint32_t>(bvh.lightIndices.size());
    bvh.stats.globalLightCount = static_cast<uint32_t>(bvh.globalLightIndices.size());
    bvh.stats.minDepth = std::numeric_limits<uint32_t>::max();

    Vector<uint32_t> nodeCountPerLevel(bvh.allocator);
    std::stack<std::pair<uint32_t, uint32_t>> stack;
    stack.push({0u, 0u});
    while (!stack.empty())
    {
        auto [nodeIndex, depth] = stack.top();
        stack.pop();
        if (nodeCountPerLevel.size() <= depth)
            nodeCountPerLevel.resize(depth + 1u, 0u);
        ++nodeCountPerLevel[depth];

        if (GSLightBVHNodeIsLeaf(bvh.nodes[nodeIndex]))
        {
            ++bvh.stats.leafNodeCount;
            bvh.stats.treeHeight = std::max(bvh.stats.treeHeight, depth);
            bvh.stats.minDepth = std::min(bvh.stats.minDepth, depth);
        }
        else
        {
            ++bvh.stats.internalNodeCount;
            stack.push({nodeIndex + 1u, depth + 1u});
            stack.push({GSLightBVHNodeRightChild(bvh.nodes[nodeIndex]), depth + 1u});
        }
    }
    (void)nodeCountPerLevel;
    if (bvh.stats.leafNodeCount == 0)
        bvh.stats.minDepth = 0;

    bvh.refitLevels.assign(bvh.stats.treeHeight + 1u, {});
    bvh.refitLevels.back().count = bvh.stats.leafNodeCount;
    stack.push({0u, 0u});
    while (!stack.empty())
    {
        auto [nodeIndex, depth] = stack.top();
        stack.pop();
        if (!GSLightBVHNodeIsLeaf(bvh.nodes[nodeIndex]))
        {
            ++bvh.refitLevels[depth].count;
            stack.push({nodeIndex + 1u, depth + 1u});
            stack.push({GSLightBVHNodeRightChild(bvh.nodes[nodeIndex]), depth + 1u});
        }
    }

    Vector<uint32_t> perDepthOffset(bvh.refitLevels.size(), 0u, bvh.allocator);
    for (size_t i = 1; i < bvh.refitLevels.size(); ++i)
    {
        uint32_t current = bvh.refitLevels[i - 1].offset + bvh.refitLevels[i - 1].count;
        perDepthOffset[i] = bvh.refitLevels[i].offset = current;
    }

    bvh.nodeIndices.assign(bvh.stats.internalNodeCount + bvh.stats.leafNodeCount, 0u);
    stack.push({0u, 0u});
    while (!stack.empty())
    {
        auto [nodeIndex, depth] = stack.top();
        stack.pop();
        if (GSLightBVHNodeIsLeaf(bvh.nodes[nodeIndex]))
            bvh.nodeIndices[perDepthOffset.back()++] = nodeIndex;
        else
        {
            bvh.nodeIndices[perDepthOffset[depth]++] = nodeIndex;
            stack.push({nodeIndex + 1u, depth + 1u});
            stack.push({GSLightBVHNodeRightChild(bvh.nodes[nodeIndex]), depth + 1u});
        }
    }
}
} // namespace

namespace
{
void ComputeDirectionalLightBounds(GSLight const& light, float3& aabbMin, float3& aabbMax, float3& center,
                                   float3& coneDirection, float& cosConeAngle)
{
    CHECK(length(light.direction) > FLT_MIN);
    coneDirection = -normalize(light.direction);
    float angularRadius = std::max(std::abs(light.params.x) * 0.5f, kLightBVHMinSunAngularRadius);
    float extent = std::sin(std::min(angularRadius, std::numbers::pi_v<float> * 0.5f));
    aabbMin = coneDirection - float3(extent);
    aabbMax = coneDirection + float3(extent);
    center = coneDirection;
    cosConeAngle = std::cos(std::min(angularRadius, std::numbers::pi_v<float>));
}

void ComputeEnvironmentLightBounds(float3& aabbMin, float3& aabbMax, float3& center, float3& coneDirection,
                                   float& cosConeAngle)
{    
    coneDirection = float3(0.0f, 0.0f, 1.0f);
    cosConeAngle = kLightBVHInvalidCosConeAngle;
    center = coneDirection;
    aabbMin = float3(-1.0f);
    aabbMax = float3(1.0f);
}
} // namespace

void ComputeAnalyticalLightBounds(GSLight const& light, float3& aabbMin, float3& aabbMax, float3& center,
                                  float3& coneDirection, float& cosConeAngle)
{
    uint32_t type = GSLightTypeCPU(light);
    center = light.position;
    coneDirection = float3(0.0f);
    cosConeAngle = kLightBVHInvalidCosConeAngle;

    if (type == kGSLightTypePoint)
    {
        float radius = std::max(light.params.x, 1e-3f);
        aabbMin = light.position - float3(radius);
        aabbMax = light.position + float3(radius);
        return;
    }
    if (type == kGSLightTypeSpot)
    {
        float radius = std::max(light.params.x, 1e-3f);
        aabbMin = light.position - float3(radius);
        aabbMax = light.position + float3(radius);
        coneDirection = normalize(light.direction);
        cosConeAngle = std::clamp(light.params.z, -1.0f, 1.0f);
        return;
    }
    if (type == kGSLightTypeDisk)
    {
        float3 u = light.dpdu * light.params.x;
        float3 v = light.dpdv * light.params.y;
        AABB bounds;
        bounds |= light.position + u + v;
        bounds |= light.position + u - v;
        bounds |= light.position - u + v;
        bounds |= light.position - u - v;
        aabbMin = bounds.minPoint;
        aabbMax = bounds.maxPoint;
        center = light.position;
        if ((light.flags & kGSLightFlagTwoSided) == 0u)
        {
            coneDirection = normalize(light.direction);
            cosConeAngle = 1.0f;
        }
        return;
    }
    if (type == kGSLightTypeRect)
    {
        AABB bounds;
        bounds |= light.position + light.dpdu + light.dpdv;
        bounds |= light.position + light.dpdu - light.dpdv;
        bounds |= light.position - light.dpdu + light.dpdv;
        bounds |= light.position - light.dpdu - light.dpdv;
        aabbMin = bounds.minPoint;
        aabbMax = bounds.maxPoint;
        center = light.position;
        if ((light.flags & kGSLightFlagTwoSided) == 0u)
        {
            coneDirection = normalize(light.direction);
            cosConeAngle = 1.0f;
        }
        return;
    }

    aabbMin = float3(0.0f);
    aabbMax = float3(0.0f);
    center = float3(0.0f);
}

LightBVHBuild BuildLightBVH(Span<GSLight const> lights, LightBVHOptions const& options, Allocator* alloc)
{
    CHECK(alloc);
    LightBVHBuild bvh(alloc);
    bvh.lightBitmasks.assign(lights.size(), std::numeric_limits<uint64_t>::max());

    CHECK_MSG(options.binCount > 1u, "binCount must be > 1");

    BuildingData data(bvh.nodes, alloc);
    data.lightsData.reserve(lights.size());
    data.lightIndices.reserve(lights.size());
    data.lightBitmasks = bvh.lightBitmasks;
    Vector<GSLightBVHNode> distantNodes(alloc);
    BuildingData distantData(distantNodes, alloc);
    distantData.lightsData.reserve(lights.size());
    distantData.lightIndices.reserve(lights.size());
    distantData.lightBitmasks = bvh.lightBitmasks;

    for (uint32_t i = 0; i < lights.size(); ++i)
    {
        GSLight const& light = lights[i];
        uint32_t type = GSLightTypeCPU(light);
        float proposalWeight = ComputeLightProposalWeight(light);
        if (IsDistantLightType(type))
        {
            if (proposalWeight > 0.0f)
            {
                LightSortData sort{};
                if (type == kGSLightTypeEnvironment)
                    ComputeEnvironmentLightBounds(sort.bounds.minPoint, sort.bounds.maxPoint, sort.center,
                                                  sort.coneDirection, sort.cosConeAngle);
                else
                    ComputeDirectionalLightBounds(light, sort.bounds.minPoint, sort.bounds.maxPoint, sort.center,
                                                  sort.coneDirection, sort.cosConeAngle);
                sort.flux = proposalWeight;
                sort.lightIndex = i;
                distantData.lightsData.push_back(sort);
            }
            continue;
        }
        if (!IsFiniteLightType(type) || proposalWeight <= 0.0f)
            continue;

        LightSortData sort{};
        ComputeAnalyticalLightBounds(light, sort.bounds.minPoint, sort.bounds.maxPoint, sort.center,
                                     sort.coneDirection, sort.cosConeAngle);
        sort.flux = proposalWeight;
        sort.lightIndex = i;
        data.lightsData.push_back(sort);
    }

    SplitFn splitFn = getSplitFunction(options.splitHeuristic);
    if (!data.lightsData.empty())
    {
        CHECK_MSG(data.lightsData.size() <= kLightBVHMaxLightOffset + kLightBVHMaxLightsPerLeaf,
                  "Finite light count exceeds Light BVH encoding limits");
        data.nodes.reserve(2u * data.lightsData.size());
        buildInternal(options, splitFn, 0ull, 0u, Range{0u, static_cast<uint32_t>(data.lightsData.size())}, data);

        float unusedCos = kLightBVHInvalidCosConeAngle;
        computeLightingConesInternal(0u, data, unusedCos);

        bvh.lightIndices = std::move(data.lightIndices);
        bvh.lightBitmasks = std::move(data.lightBitmasks);
        bvh.valid = true;
        finalizeNodeIndices(bvh);
    }

    bvh.finiteLightIndexCount = static_cast<uint32_t>(bvh.lightIndices.size());
    if (!distantData.lightsData.empty())
    {
        distantNodes.reserve(2u * distantData.lightsData.size());
        buildInternal(options, splitFn, 0ull, 0u,
                      Range{0u, static_cast<uint32_t>(distantData.lightsData.size())}, distantData);
        float unusedCos = kLightBVHInvalidCosConeAngle;
        computeLightingConesInternal(0u, distantData, unusedCos);

        uint32_t nodeBase = static_cast<uint32_t>(bvh.nodes.size());
        uint32_t lightBase = static_cast<uint32_t>(bvh.lightIndices.size());
        bvh.distantRootNode = nodeBase;
        bvh.distantNodeCount = static_cast<uint32_t>(distantNodes.size());
        for (GSLightBVHNode& node : distantNodes)
        {
            if (GSLightBVHNodeIsLeaf(node))
                GSLightBVHNodeSetLeaf(node, GSLightBVHNodeLightCount(node),
                                      lightBase + GSLightBVHNodeLightOffset(node));
            else
                GSLightBVHNodeSetInternal(node, nodeBase + GSLightBVHNodeRightChild(node));
            bvh.nodes.push_back(node);
        }
        bvh.lightIndices.insert(bvh.lightIndices.end(), distantData.lightIndices.begin(),
                                distantData.lightIndices.end());
        for (LightSortData const& light : distantData.lightsData)
            bvh.lightBitmasks[light.lightIndex] = distantData.lightBitmasks[light.lightIndex];
    }
    bvh.stats.byteSize = static_cast<uint32_t>(bvh.nodes.size() * sizeof(GSLightBVHNode));
    bvh.stats.globalLightCount = static_cast<uint32_t>(distantData.lightsData.size());
    return bvh;
}

bool ValidateLightBVH(LightBVHBuild const& bvh, Span<GSLight const> lights, String* outError)
{
    auto Fail = [&](char const* message)
    {
        if (outError)
            *outError = message;
        return false;
    };

    if (bvh.lightBitmasks.size() != lights.size())
        return Fail("lightBitmasks size mismatch");

    uint32_t expectedDistant = 0;
    uint32_t expectedFinite = 0;
    for (GSLight const& light : lights)
    {
        uint32_t type = GSLightTypeCPU(light);
        float proposalWeight = ComputeLightProposalWeight(light);
        if (IsDistantLightType(type) && proposalWeight > 0.0f)
            ++expectedDistant;
        else if (IsFiniteLightType(type) && proposalWeight > 0.0f)
            ++expectedFinite;
    }

    if (!bvh.globalLightIndices.empty())
        return Fail("global light list should be empty");
    if (bvh.valid != (expectedFinite != 0))
        return Fail("finite BVH validity mismatch");
    if ((bvh.distantRootNode != UINT32_MAX) != (expectedDistant != 0))
        return Fail("distant BVH validity mismatch");
    if ((bvh.valid || expectedDistant != 0) && bvh.nodes.empty())
        return Fail("valid BVH has no nodes");
    if (bvh.finiteLightIndexCount != expectedFinite ||
        bvh.lightIndices.size() != static_cast<size_t>(expectedFinite + expectedDistant))
        return Fail("BVH light membership mismatch");

    std::vector<uint8_t> covered(lights.size(), 0u);
    std::stack<std::pair<uint32_t, uint32_t>> stack;
    if (bvh.valid)
        stack.push({0u, 0u});
    while (!stack.empty())
    {
        auto [nodeIndex, depth] = stack.top();
        stack.pop();
        if (nodeIndex >= bvh.nodes.size() || depth > kLightBVHMaxDepth)
            return Fail("node index/depth out of range");

        GSLightBVHNode const& node = bvh.nodes[nodeIndex];
        float3 aabbMin, aabbMax;
        GSLightBVHNodeGetAABB(node, aabbMin, aabbMax);
        if (!all(lessThanEqual(aabbMin, aabbMax)))
            return Fail("invalid node AABB");
        if (!(node.flux >= 0.0f))
            return Fail("negative node flux");

        if (GSLightBVHNodeIsLeaf(node))
        {
            uint32_t count = GSLightBVHNodeLightCount(node);
            uint32_t offset = GSLightBVHNodeLightOffset(node);
            if (count == 0 || offset + count > bvh.lightIndices.size())
                return Fail("leaf light range out of bounds");

            AABB leafBounds;
            float leafFlux = 0.0f;
            for (uint32_t i = 0; i < count; ++i)
            {
                uint32_t lightIndex = bvh.lightIndices[offset + i];
                if (lightIndex >= lights.size() || !IsFiniteLightType(GSLightTypeCPU(lights[lightIndex])))
                    return Fail("leaf contains non-finite light");
                if (covered[lightIndex])
                    return Fail("duplicate finite light in leaves");
                covered[lightIndex] = 1u;

                float3 childMin, childMax, center, coneDirection;
                float cosConeAngle = kLightBVHInvalidCosConeAngle;
                ComputeAnalyticalLightBounds(lights[lightIndex], childMin, childMax, center, coneDirection,
                                             cosConeAngle);
                leafBounds |= AABB{childMin, childMax};
                leafFlux += ComputeLightProposalWeight(lights[lightIndex]);

                uint32_t replay = 0u;
                uint64_t bits = bvh.lightBitmasks[lightIndex];
                for (uint32_t d = 0; d < depth; ++d)
                {
                    if (GSLightBVHNodeIsLeaf(bvh.nodes[replay]))
                        return Fail("path mask reached leaf early");
                    bool goRight = (bits & 1ull) != 0ull;
                    replay = goRight ? GSLightBVHNodeRightChild(bvh.nodes[replay]) : replay + 1u;
                    bits >>= 1;
                }
                if (replay != nodeIndex)
                    return Fail("path mask does not replay to leaf");
            }
            if (any(lessThan(leafBounds.minPoint, aabbMin - float3(1e-3f))) ||
                any(greaterThan(leafBounds.maxPoint, aabbMax + float3(1e-3f))))
                return Fail("leaf AABB does not cover children");
            float fluxTolerance = 1e-4f * std::max(1.0f, leafFlux);
            if (std::abs(leafFlux - node.flux) > fluxTolerance)
                return Fail("leaf flux mismatch");
        }
        else
        {
            uint32_t left = nodeIndex + 1u;
            uint32_t right = GSLightBVHNodeRightChild(node);
            if (left >= bvh.nodes.size() || right >= bvh.nodes.size() || right <= left)
                return Fail("invalid internal child indices");
            stack.push({left, depth + 1u});
            stack.push({right, depth + 1u});
        }
    }

    for (uint32_t i = 0; i < lights.size(); ++i)
    {
        if (IsFiniteLightType(GSLightTypeCPU(lights[i])) && ComputeLightProposalWeight(lights[i]) > 0.0f &&
            !covered[i])
            return Fail("finite light missing from BVH");
    }

    if (expectedDistant != 0)
    {
        stack.push({bvh.distantRootNode, 0u});
        while (!stack.empty())
        {
            auto [nodeIndex, depth] = stack.top();
            stack.pop();
            if (nodeIndex >= bvh.nodes.size() || depth > kLightBVHMaxDepth)
                return Fail("distant node index/depth out of range");

            GSLightBVHNode const& node = bvh.nodes[nodeIndex];
            if (GSLightBVHNodeIsLeaf(node))
            {
                uint32_t count = GSLightBVHNodeLightCount(node);
                uint32_t offset = GSLightBVHNodeLightOffset(node);
                if (count == 0 || offset < bvh.finiteLightIndexCount ||
                    offset + count > bvh.lightIndices.size())
                    return Fail("distant leaf light range out of bounds");
                for (uint32_t i = 0; i < count; ++i)
                {
                    uint32_t lightIndex = bvh.lightIndices[offset + i];
                    if (lightIndex >= lights.size() ||
                        !IsDistantLightType(GSLightTypeCPU(lights[lightIndex])) || covered[lightIndex])
                        return Fail("invalid or duplicate distant light");
                    covered[lightIndex] = 1u;

                    uint32_t replay = bvh.distantRootNode;
                    uint64_t bits = bvh.lightBitmasks[lightIndex];
                    for (uint32_t d = 0; d < depth; ++d)
                    {
                        bool goRight = (bits & 1ull) != 0ull;
                        replay = goRight ? GSLightBVHNodeRightChild(bvh.nodes[replay]) : replay + 1u;
                        bits >>= 1;
                    }
                    if (replay != nodeIndex)
                        return Fail("distant path mask does not replay to leaf");
                }
            }
            else
            {
                uint32_t left = nodeIndex + 1u;
                uint32_t right = GSLightBVHNodeRightChild(node);
                if (left >= bvh.nodes.size() || right >= bvh.nodes.size() || right <= left)
                    return Fail("invalid distant child indices");
                stack.push({left, depth + 1u});
                stack.push({right, depth + 1u});
            }
        }
        for (uint32_t i = 0; i < lights.size(); ++i)
        {
            if (IsDistantLightType(GSLightTypeCPU(lights[i])) &&
                ComputeLightProposalWeight(lights[i]) > 0.0f && !covered[i])
                return Fail("distant light missing from BVH");
        }
    }
    return true;
}
