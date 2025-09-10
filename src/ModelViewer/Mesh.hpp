#pragma once
#include <filesystem>
#include <RHICore/Common.hpp>
#include <Core/Core.hpp>
using namespace Foundation;
using Index = uint32_t; // 32-bit index
struct Vertex {
    uint16_t px, py, pz; // quantized fp16
    uint16_t tp;         // tangent [octa 8+8]
    struct {
        uint32_t nx : 15;
        uint32_t ny : 15;
        uint32_t sign : 2;
    } np; // normal packed [snorm octa 15+15, bitangent sign 2]
    uint16_t u, v;       // texcoord fp16
};
static constexpr RHI::RHIVertexAttribute Attributes[4]{
    {.location = 0, .offset = offsetof(Vertex, px), .format = RHI::RHIResourceFormat::R16G16B16_SIGNED_FLOAT, .binding = 0 },
    {.location = 1, .offset = offsetof(Vertex, tp), .format = RHI::RHIResourceFormat::R16_UINT, .binding = 0 },
    {.location = 2, .offset = offsetof(Vertex, np), .format = RHI::RHIResourceFormat::R32_UINT, .binding = 0 },
    {.location = 3, .offset = offsetof(Vertex, u),  .format = RHI::RHIResourceFormat::R16G16_SIGNED_FLOAT, .binding = 0 },
};
struct Mesh {
    Core::Allocator* m_allocator;
    const Core::Vector<char> m_vertex_data, m_index_data;
    const size_t m_num_vertices, m_num_indices;
    Mesh(
        Core::Span<const char> vertex_data,
        Core::Span<const char> index_data,
        size_t num_vertices, size_t num_indices,
        Core::Allocator* allocator
    ) noexcept
        : m_allocator(allocator),
        m_vertex_data(vertex_data.begin(), vertex_data.end(), allocator),
        m_index_data(index_data.begin(), index_data.end(), allocator),
        m_num_vertices(num_vertices),
        m_num_indices(num_indices)
    {}
    inline constexpr uint32_t GetStride() const noexcept { return m_vertex_data.size() / m_num_vertices; }
};
extern Mesh LoadMeshFromObjFile(std::filesystem::path const& path, Core::Allocator* allocator);
