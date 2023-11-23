#pragma once
#include <assimp/Importer.hpp>      // C++ importer interface
#include <assimp/scene.h>           // Output data structure
#include <meshoptimizer.h>
#include <assimp/postprocess.h>

#include "../../Runtime/Renderer/Shaders/Shared.h"
struct Meshlet {
	uint vertex_offset;
	uint vertex_count;

	uint triangle_offset;
	uint triangle_count;

	/* bounding sphere, useful for frustum and occlusion culling */
	float center[3];
	float radius;

	/* normal cone, useful for backface culling */
	float cone_apex[3];
	uint cone_axis_cutoff; // cutoff | z | y | x
};
struct MeshletTriangle { UINT V0 : 10, V1 : 10, V2 : 10, : 2; };
struct MeshLod {
	std::vector<UINT> indices;
	/* mesh shading */
	std::vector<Meshlet> meshlets; // offsets within meshlet_vertices , meshlet_bounds & meshlet_triangles
	std::vector<MeshletTriangle> meshlet_triangles; // primitive indices (3 verts i.e. i,i+1,i+2) into meshlet_vertices
	std::vector<UINT> meshlet_vertices; // indexes to mesh_static::{position,normal,tangent,uv}		

	void resize_max_meshlets(size_t maxMeshlets) {
		meshlets.resize(maxMeshlets);			
		meshlet_vertices.resize(maxMeshlets * MESHLET_MAX_VERTICES);
		meshlet_triangles.resize(maxMeshlets * MESHLET_MAX_PRIMITIVES);
	}
};

struct StaticMesh {
	std::vector<float3> position;
	std::vector<float3> normal;
	std::vector<float3> tangent;
	std::vector<float2> uv;
	std::vector<UINT> indices;

	MeshLod lods[MAX_LOD_COUNT];		

	void resize_vertices(size_t verts) {
		position.resize(verts);
		normal.resize(verts);
		tangent.resize(verts);
		uv.resize(verts);
	}

	inline void remap(decltype(indices)& remapping) {		
		meshopt_remapVertexBuffer(position.data(), position.data(), position.size(), sizeof(decltype(position)::value_type), remapping.data());
		meshopt_remapVertexBuffer(normal.data(), normal.data(), normal.size(), sizeof(decltype(normal)::value_type), remapping.data());
		meshopt_remapVertexBuffer(tangent.data(), tangent.data(), tangent.size(), sizeof(decltype(tangent)::value_type), remapping.data());
		meshopt_remapVertexBuffer(uv.data(), uv.data(), uv.size(), sizeof(decltype(uv)::value_type), remapping.data());
	}
	StaticMesh() = default;
	~StaticMesh() = default;
};

static inline StaticMesh load_static_mesh(aiMesh* srcMesh) {
	UINT num_vertices = srcMesh->mNumVertices;
	StaticMesh mesh;
	mesh.resize_vertices(num_vertices);
	for (UINT i = 0; i < num_vertices; i++) {
		if (srcMesh->mVertices) {
			mesh.position[i].x = srcMesh->mVertices[i].x;
			mesh.position[i].y = srcMesh->mVertices[i].y;
			mesh.position[i].z = srcMesh->mVertices[i].z;
				
		}
		if (srcMesh->mNormals) {
			mesh.normal[i].x = srcMesh->mNormals[i].x;
			mesh.normal[i].y = srcMesh->mNormals[i].y;
			mesh.normal[i].z = srcMesh->mNormals[i].z;
		}
		if (srcMesh->mTangents) {
			mesh.tangent[i].x = srcMesh->mTangents[i].x;
			mesh.tangent[i].y = srcMesh->mTangents[i].y;
			mesh.tangent[i].z = srcMesh->mTangents[i].z;
		}
		if (srcMesh->mTextureCoords[0]) {
			mesh.uv[i].x = srcMesh->mTextureCoords[0][i].x;
			mesh.uv[i].y = srcMesh->mTextureCoords[0][i].y;
		}
	}
	for (UINT i = 0; i < srcMesh->mNumFaces; i++) {
		aiFace& face = srcMesh->mFaces[i];
		for (UINT j = 0; j < face.mNumIndices; j++)
			mesh.indices.push_back(face.mIndices[j]);
	}
	// Index reording
	meshopt_optimizeVertexCache(mesh.indices.data(), mesh.indices.data(), mesh.indices.size(), num_vertices);
	meshopt_optimizeOverdraw(mesh.indices.data(), mesh.indices.data(), mesh.indices.size(), (const float*)mesh.position.data(), num_vertices, sizeof(decltype(mesh.position)::value_type), 1.05f);
	// Vertex reording
	std::vector<UINT> remapping(num_vertices);
	meshopt_optimizeVertexFetchRemap(remapping.data(), mesh.indices.data(), mesh.indices.size(), num_vertices);
	mesh.remap(remapping);
	meshopt_remapIndexBuffer(mesh.indices.data(), mesh.indices.data(), mesh.indices.size(), remapping.data());
	// LOD
	std::vector<unsigned char> meshlet_triangles; // buffer to store u8 triangles
	std::vector<meshopt_Meshlet> meshlets; // buffer to store meshlets
	for (size_t lod = 0; lod < MAX_LOD_COUNT; lod++) {
		const float target_error = 0.01f;
		size_t target_index_count = mesh.indices.size() * LOD_GET_RATIO(lod);
		auto& mesh_lod = mesh.lods[lod];
		mesh_lod.indices.resize(mesh.indices.size());
		float output_error = 0.0f;
		size_t output_index_count = meshopt_simplify(
			mesh_lod.indices.data(), mesh.indices.data(), mesh.indices.size(),
			(const float*)mesh.position.data(), num_vertices, sizeof(decltype(mesh.position)::value_type),
			target_index_count, target_error, 0, &output_error
		);
		mesh_lod.indices.resize(output_index_count);
		/* mesh shading */
		// Meshlet generation
		size_t max_meshlets = meshopt_buildMeshletsBound(mesh_lod.indices.size(), MESHLET_MAX_VERTICES, MESHLET_MAX_PRIMITIVES);						
		meshlet_triangles.resize(max_meshlets * MESHLET_MAX_PRIMITIVES * 3);
		meshlets.resize(max_meshlets);
		mesh_lod.resize_max_meshlets(max_meshlets);
		size_t meshlet_count = meshopt_buildMeshlets(
			meshlets.data(), mesh_lod.meshlet_vertices.data(), meshlet_triangles.data(),
			mesh_lod.indices.data(), mesh_lod.indices.size(), (const float*)mesh.position.data(),
			num_vertices, sizeof(decltype(mesh.position)::value_type), MESHLET_MAX_VERTICES, MESHLET_MAX_PRIMITIVES, 0.0f
		);
		// Meshlet trimming
		const auto& last = meshlets[meshlet_count - 1];
		mesh_lod.meshlet_vertices.resize((last.vertex_offset + last.vertex_count));
		meshlet_triangles.resize((last.triangle_offset + ((last.triangle_count * 3 + 3) & ~3)));			
		mesh_lod.meshlets.resize(meshlet_count);
		for (size_t i = 0; i < mesh_lod.meshlets.size(); i++) {
			Meshlet& meshlet = mesh_lod.meshlets[i];
			// Meshlet
			meshlet.triangle_count = meshlets[i].triangle_count;
			meshlet.triangle_offset = meshlets[i].triangle_offset;
			meshlet.vertex_count = meshlets[i].vertex_count;
			meshlet.vertex_offset = meshlets[i].vertex_offset;
			// Meshlet bounds
			meshopt_Bounds bounds = meshopt_computeMeshletBounds(
				&mesh_lod.meshlet_vertices[meshlet.vertex_offset],
				&meshlet_triangles[meshlet.triangle_offset],
				meshlet.triangle_count,
				(const float*)mesh.position.data(),
				mesh.position.size(),
				sizeof(decltype(mesh.position)::value_type)
			);
			meshlet.center[0] = bounds.center[0];
			meshlet.center[1] = bounds.center[1];
			meshlet.center[2] = bounds.center[2];
			meshlet.cone_apex[0] = bounds.cone_apex[0];
			meshlet.cone_apex[1] = bounds.cone_apex[1];
			meshlet.cone_apex[2] = bounds.cone_apex[2];
			meshlet.radius = bounds.radius;
			meshlet.cone_axis_cutoff = bounds.cone_cutoff_s8 << 24 | bounds.cone_axis_s8[2] << 16 | bounds.cone_axis_s8[1] << 8 | bounds.cone_axis_s8[0];
		}
		// Swizzle triangles to u32 boundaries
		for (size_t i = 0; i < meshlet_triangles.size() - 3; i += 3) {
			mesh_lod.meshlet_triangles.push_back({ meshlet_triangles[i] ,meshlet_triangles[i + 1], meshlet_triangles[i + 2] });
		}
	}
	return mesh;
}