#define FAST_OBJ_IMPLEMENTATION
#include <fast_obj.h>

#include <meshoptimizer.h>
#define CLUSTERLOD_IMPLEMENTATION
#include <clusterlod.h>

#include "Mesh.hpp"

#include "Math/Quantize.hpp"
// -- quantize
constexpr float EPS = 1e-6;
// Building an Orthonormal Basis from a 3D Unit Vector Without Normalization - Frisvad, 2012
// https://backend.orbit.dtu.dk/ws/portalfiles/portal/126824972/onb_frisvad_jgt2012_v2.pdf
inline void BuildOrthonormalBasis(const float3 n, float3& b1, float3& b2)
{
    if (n.z < -0.9999999)
    {
        b1 = float3(0.0, -1.0, 0.0);
        b2 = float3(-1.0, 0.0, 0.0);
        return;
    }
    float a = 1.0 / (1.0 + n.z);
    float b = -n.x * n.y * a;
    b1 = float3(1.0 - n.x * n.x * a, b, -n.x);
    b2 = float3(b, 1.0 - n.y * n.y * a, -n.y);
}
// Normals Compression - Octahedron by iq
// https://www.shadertoy.com/view/Mtfyzl
const float2 PackUnitOctahedral(float3 v) {
    v /= float3(fabsf(v.x) + fabsf(v.y) + fabsf(v.z));
    return v.z >= EPS ? v.xy() : (float2(1.0f) - abs(float2(v.yx()))) * sign(float2(v.xy() + EPS));
}
// Normals Compression - Octahedron by iq
// https://www.shadertoy.com/view/Mtfyzl - Rune Stubbe's version
const float3 UnpackUnitOctahedral(float2 v) {
    float3 nor = float3(v.xy(), 1.0f - fabsf(v.x) - fabsf(v.y));
    float2 xy = nor.z >= EPS ? v.xy() : (float2(1.0f) - abs(float2(v.yx()))) * sign(float2(v.xy() + EPS));
    return normalize(float3(xy.x, xy.y ,nor.z));
}
// Compact TBN frame packing
// Tangent is derived from orthonormal basis around normal with a rotation angle
// Similar to 3 BYTE TANGENT FRAMES from "Rendering the Hellscape of Doom Eternal - SIGGRAPH 2020" by Jorge Jimenez et
// al.
uint32_t FQVertex::PackTBN(const float3& normal, const float3& tangent, float bitangentSign)
{
    float3 b1, b2;
    BuildOrthonormalBasis(normal, b1, b2);
    float cosAngle = dot(tangent, b1), sinAngle = dot(tangent, b2);
    float2 octNormal = PackUnitOctahedral(normal);
    return 0;
}
void FQVertex::UnpackTBN(uint32_t packed, float3& outNormal, float3& outTangent, float& outBitangentSign) {}
FQVertex FQVertex::Pack(FVertex const& vertex)
{
    FQVertex result;
    result.position[0] = quantizeFP16(vertex.position[0]);
    result.position[1] = quantizeFP16(vertex.position[1]);
    result.position[2] = quantizeFP16(vertex.position[2]);
    result.tbn32 = PackTBN(vertex.normal, vertex.tangent, vertex.bitangentSign);
    result.uv[0] = quantizeUnorm(vertex.uv[0], 16);
    result.uv[1] = quantizeUnorm(vertex.uv[1], 16);
    return result;
}
FVertex FQVertex::Unpack(FQVertex const& vertex)
{
    FVertex result;
    result.position[0] = dequantizeFP16(vertex.position[0]);
    result.position[1] = dequantizeFP16(vertex.position[1]);
    result.position[2] = dequantizeFP16(vertex.position[2]);
    UnpackTBN(vertex.tbn32, result.normal, result.tangent, result.bitangentSign);
    result.uv[0] = dequantizeUnorm(vertex.uv[0], 16);
    result.uv[1] = dequantizeUnorm(vertex.uv[1], 16);
    return result;
}
float QuantizationError(FQVertex const& quantized, FVertex const& src)
{
    FVertex dequantized = FQVertex::Unpack(quantized);
    float posError = length(dequantized.position - src.position);
    float normalError = length(dequantized.normal - src.normal);
    float tangentError = length(dequantized.tangent - src.tangent);
    float uvError = length(dequantized.uv - src.uv);
    return posError + normalError + tangentError + uvError;
}
void FMesh::Quantize()
{
    quantizedVertices.resize(rawVertices.size());
    for (size_t i = 0; i < rawVertices.size(); i++)
    {
        quantizedVertices[i] = FQVertex::Pack(rawVertices[i]);
        float err = QuantizationError(quantizedVertices[i], rawVertices[i]);
        LOG(FMesh, LogDebug, "Err {}", err);
    }
}
void FMesh::Dequantize()
{
    rawVertices.resize(quantizedVertices.size());
    for (size_t i = 0; i < quantizedVertices.size(); i++)
        rawVertices[i] = FQVertex::Unpack(quantizedVertices[i]);
}
// -- optimize
template <typename Vertex, typename Index>
void OptimizeVertexIndex(Vector<Vertex>& vertices, Vector<Index>& indices)
{
    Vector<uint32_t> remap(vertices.size(), vertices.get_allocator());
    size_t unique = meshopt_generateVertexRemap(remap.data(), indices.data(), indices.size(), vertices.data(),
                                                vertices.size(), sizeof(FVertex));
    meshopt_remapVertexBuffer(vertices.data(), vertices.data(), vertices.size(), sizeof(FVertex), remap.data());
    meshopt_remapIndexBuffer(indices.data(), indices.data(), indices.size(), remap.data());
    vertices.resize(unique);
    meshopt_optimizeVertexCache(indices.data(), indices.data(), indices.size(), vertices.size());
    meshopt_optimizeVertexFetch(vertices.data(), indices.data(), indices.size(), vertices.data(), vertices.size(),
                                sizeof(FVertex));
}

// -- simplify
template <typename Vertex, typename Index>
float GenerateLOD(Vector<Index>& outIndices, Span<const Vertex> vertices, Span<const Index> indices,
                  const float lodScale)
{
    const float normalWeight = 0.5f;
    const float attrWeights[3] = {normalWeight, normalWeight, normalWeight};
    auto targetIndexCount = static_cast<uint32_t>(ceilf(indices.size() * lodScale));
    outIndices.resize(indices.size());
    float actualError;
    size_t lodSize =
        meshopt_simplifyWithAttributes(outIndices.data(), indices.data(), indices.size(),
                                       reinterpret_cast<const float*>(&vertices[0]), vertices.size(), sizeof(Vertex),
                                       &vertices[0].normal.x, sizeof(Vertex), attrWeights, 3, nullptr, targetIndexCount,
                                       1e-1f, // max error
                                       0, // options
                                       &actualError);
    outIndices.resize(lodSize);
    return actualError;
}

// -- clusterize
template <typename Vertex, typename Index>
void BuildMeshlets(Vector<FMeshlet>& outMeshlet, Vector<Index>& outMeshletVertices,
                   Vector<uint8_t>& outMeshletTriangles, Span<const Vertex> vertices, Span<const Index> indices)
{
    size_t maxMeshlets = meshopt_buildMeshletsBound(indices.size(), kMeshletMaxVertices, kMeshletMaxTriangles);
    // Worst bounds
    outMeshletVertices.resize(maxMeshlets * kMeshletMaxVertices);
    outMeshletTriangles.resize(maxMeshlets * kMeshletMaxTriangles);
    Vector<meshopt_Meshlet> meshoptMeshlets(maxMeshlets, outMeshlet.get_allocator());
    size_t meshlets =
        meshopt_buildMeshlets(meshoptMeshlets.data(), outMeshletVertices.data(), outMeshletTriangles.data(),
                              indices.data(), indices.size(), reinterpret_cast<const float*>(&vertices[0]),
                              vertices.size(), sizeof(Vertex), kMeshletMaxVertices, kMeshletMaxTriangles,
                              0.25f // As recommended by the docs
        );
    meshoptMeshlets.resize(meshlets);
    {
        const auto& [vertexOffset, triangleOffset, vertexCount, triangleCount] = meshoptMeshlets.back();
        outMeshletVertices.resize(vertexOffset + vertexCount);
        outMeshletTriangles.resize(triangleOffset + triangleCount * 3);
    }
    // Cone culling bounds
    outMeshlet.resize(meshoptMeshlets.size());
    for (size_t i = 0; i < meshoptMeshlets.size(); ++i)
    {
        auto& dst = outMeshlet[i];
        auto& src = meshoptMeshlets[i];
        dst.group = 0u;
        dst.vtxOffset = src.vertex_offset;
        dst.triOffset = src.triangle_offset;
        dst.vtxCount = src.vertex_count;
        dst.triCount = src.triangle_count;
        meshopt_Bounds bounds = meshopt_computeMeshletBounds(
            &outMeshletVertices[src.vertex_offset], &outMeshletTriangles[src.triangle_offset], src.triangle_count,
            reinterpret_cast<const float*>(&vertices[0]), vertices.size(), sizeof(Vertex));
        dst.centerRadius = float4(bounds.center[0], bounds.center[1], bounds.center[2], bounds.radius);
        dst.coneAxisAngle = float4(bounds.cone_axis[0], bounds.cone_axis[1], bounds.cone_axis[2], bounds.cone_cutoff);
        dst.coneApex = float3(bounds.cone_apex[0], bounds.cone_apex[1], bounds.cone_apex[2]);
    }
}

void FMesh::SimplifyLOD(int levels, float scale)
{
    CHECK_MSG(lods.size() == 1, "LOD already populated");
    for (int i = 1; i < levels; i++)
    {
        lods.emplace_back(lods.get_allocator().mResource);
        GenerateLOD<FVertex, uint32_t>(lods[i].indices, rawVertices, lods[i - 1].indices, scale);
    }
}
void FMesh::ClusterizeLOD()
{
    for (int i = 0; i < lods.size(); i++)
    {
        BuildMeshlets<FVertex, uint32_t>(lods[i].meshlets, lods[i].meshletVtx, lods[i].meshletTri, rawVertices,
                                         lods[i].indices);
    }
}
void FMesh::ClusterizeDAG()
{
    clodConfig config = clodDefaultConfig(kMeshletMaxTriangles);
    const float attribute_weights[3] = {0.5f, 0.5f, 0.5f};
    clodMesh mesh{.indices = lods[0].indices.data(),
                  .index_count = lods[0].indices.size(),
                  .vertex_count = rawVertices.size(),
                  .vertex_positions = reinterpret_cast<const float*>(&rawVertices[0].position),
                  .vertex_positions_stride = sizeof(FVertex),
                  .vertex_attributes = reinterpret_cast<const float*>(&rawVertices[0].normal),
                  .vertex_attributes_stride = sizeof(FVertex),
                  .attribute_weights = attribute_weights,
                  .attribute_count = 3};
    clodBuild(config, mesh,
              [&](clodGroup group, const clodCluster* clusters, size_t cluster_count) -> int
              {
                  size_t group_id = dag.groups.size();
                  dag.groups.push_back(FLODGroup{
                      .depth = group.depth,
                      .center = {group.simplified.center[0], group.simplified.center[1], group.simplified.center[2]},
                      .radius = group.simplified.radius,
                      .error = group.simplified.error});
                  for (size_t i = 0; i < cluster_count; i++)
                  {
                      auto& cluster = clusters[i];
                      auto& lvl = dag._clusters.emplace_back(rawVertices.get_allocator().mResource);
                      lvl.group = group_id, lvl.refined = cluster.refined;
                      auto& ind = lvl.indices;
                      ind.insert(ind.end(), cluster.indices, cluster.indices + cluster.index_count);
                  }
                  return group_id; // recorded as refined IDs
              });
    // Done - build meshlets for each cluster
    size_t numIndices = 0;
    for (auto& cluster : dag._clusters)
        numIndices += cluster.indices.size();
    // Worst bounds
    dag.meshletVtx.resize(numIndices), dag.meshletTri.resize(numIndices);
    uint32_t* vtx = dag.meshletVtx.data();
    uint8_t* tri = dag.meshletTri.data();
    dag.meshlets.reserve(dag._clusters.size());
    for (auto& cluster : dag._clusters)
    {
        FMeshlet meshlet{
            .group = cluster.group,
            .refined = cluster.refined,
            .vtxOffset = static_cast<uint32_t>(vtx - dag.meshletVtx.data()),
            .triOffset = static_cast<uint32_t>(tri - dag.meshletTri.data()),
        };
        size_t unique = clodLocalIndices(vtx, tri, cluster.indices.data(), cluster.indices.size());
        vtx += unique, tri += cluster.indices.size();
        meshlet.vtxCount = unique, meshlet.triCount = cluster.indices.size() / 3;
        // Compute bounds
        meshopt_Bounds bounds = meshopt_computeMeshletBounds(
            &dag.meshletVtx[meshlet.vtxOffset], &dag.meshletTri[meshlet.triOffset], meshlet.triCount,
            reinterpret_cast<const float*>(&rawVertices[0]), rawVertices.size(), sizeof(FVertex));
        meshlet.centerRadius = float4(bounds.center[0], bounds.center[1], bounds.center[2], bounds.radius);
        meshlet.coneAxisAngle =
            float4(bounds.cone_axis[0], bounds.cone_axis[1], bounds.cone_axis[2], bounds.cone_cutoff);
        meshlet.coneApex = float3(bounds.cone_apex[0], bounds.cone_apex[1], bounds.cone_apex[2]);
        dag.meshlets.push_back(meshlet);
    }
    dag._clusters = {}; // No longer needed
}
size_t FMesh::ApproximateSizeQuantized() const
{
    size_t size = sizeof(FMesh);
    size += quantizedVertices.size() * sizeof(FQVertex);
    for (auto const& lod : lods)
    {
        size += sizeof(decltype(lod));
        size += lod.indices.size() * sizeof(uint32_t);
        size += lod.meshlets.size() * sizeof(FMeshlet);
        size += lod.meshletVtx.size() * sizeof(uint32_t);
        size += lod.meshletTri.size() * sizeof(uint8_t);
    }
    size += sizeof(decltype(dag));
    size += dag.groups.size() * sizeof(FLODGroup);
    size += dag.meshlets.size() * sizeof(FMeshlet);
    size += dag.meshletVtx.size() * sizeof(uint32_t);
    size += dag.meshletTri.size() * sizeof(uint8_t);
    return size;
}
void FMesh::Optimize()
{
    OptimizeVertexIndex(rawVertices, lods[0].indices);
}
FMesh::FMesh(Allocator* alloc) : quantizedVertices(alloc), rawVertices(alloc), lods(alloc), dag(alloc) { lods.resize(1u, alloc); }
// -- loading
void LoadObj(FMesh& outMesh, StringView path)
{
    fastObjMesh* mesh = fast_obj_read(path.data());
    UniquePtr<fastObjMesh, decltype(&fast_obj_destroy)> raii(mesh, &fast_obj_destroy);
    CHECK_MSG(mesh, "Failed to load OBJ file: {}", path);
    uint32_t vertexBound = 0; // Bound post triangulation
    for (uint32_t face = 0; face < mesh->face_count; face++)
        vertexBound += 3u * (mesh->face_vertices[face] - 2u);
    outMesh.rawVertices.resize(vertexBound), outMesh.lods[0].indices.resize(vertexBound);
    for (size_t vtx = 0, idx = 0, face = 0; face < mesh->face_count; face++)
    {
        for (uint32_t faceVtx = 0; faceVtx < mesh->face_vertices[face]; faceVtx++)
        {
            auto [p, t, n] = mesh->indices[idx + faceVtx];
            if (faceVtx >= 3) // CCW
            {
                outMesh.rawVertices[vtx + 0] = outMesh.rawVertices[vtx - 3];
                outMesh.rawVertices[vtx + 1] = outMesh.rawVertices[vtx - 1];
                vtx += 2;
            }
            outMesh.rawVertices[vtx++] = {{mesh->positions[p * 3 + 0], mesh->positions[p * 3 + 1], mesh->positions[p * 3 + 2]},
                               {mesh->normals[n * 3 + 0], mesh->normals[n * 3 + 1], mesh->normals[n * 3 + 2]},
                                {}, {}, // No tangent info
                               {mesh->texcoords[t * 2 + 0], mesh->texcoords[t * 2 + 1]}};
        }
        idx += mesh->face_vertices[face];
    }
    std::iota(outMesh.lods[0].indices.begin(), outMesh.lods[0].indices.end(), 0);
}
