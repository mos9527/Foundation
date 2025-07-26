#pragma once
#include "Blobs.hpp"
namespace Foundation::Blobs {
    struct Mesh : public Blob {
        Core::Allocator* m_allocator;
        const Core::StlVector<char> m_vertex_data, m_index_data;
        const size_t m_num_vertices, m_num_indices;
        Mesh(
            Core::StlSpan<const char> vertex_data,
            Core::StlSpan<const char> index_data,
            size_t num_vertices, size_t num_indices,
            Core::Allocator* allocator
        ) noexcept
            : m_allocator(allocator),
            m_vertex_data(vertex_data.begin(), vertex_data.end(), allocator),
            m_index_data(index_data.begin(), index_data.end(), allocator),
            m_num_vertices(num_vertices),
            m_num_indices(num_indices)
        { }

        inline constexpr uint32_t GetStride() const noexcept { return m_vertex_data.size() / m_num_vertices; }

        void Serialize(Stream& stream) override;
        void Deserialize(Stream& stream) override;
    };

}
