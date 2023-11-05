#pragma once
#include <assimp/Importer.hpp>      // C++ importer interface
#include <assimp/scene.h>           // Output data structure
#include <meshoptimizer.h>
#include <assimp/postprocess.h>
#include "IO.hpp"

namespace IO {
	typedef aiScene scene_t;
	typedef meshopt_Meshlet meshlet;
	typedef meshopt_Bounds meshlet_bound;		
	struct meshlet_triangle_u32 { UINT V0 : 10, V1 : 10, V2 : 10, : 2; /* 3 8-bits packed into 32-bit bitfields */ };
	struct mesh_lod {
		std::vector<UINT> indices;
		/* mesh shading */
		std::vector<meshlet> meshlets; // offsets within meshlet_vertices , meshlet_bounds & meshlet_triangles
		std::vector<meshlet_bound> meshlet_bounds;
		std::vector<unsigned char> meshlet_triangles; // primitive indices (3 verts i.e. i,i+1,i+2) into meshlet_vertices
		std::vector<UINT> meshlet_vertices; // indexes to mesh_static::{position,normal,tangent,uv}

		void resize_max_meshlets(size_t maxMeshlets) {
			meshlets.resize(maxMeshlets);
			meshlet_bounds.resize(maxMeshlets);
			meshlet_vertices.resize(maxMeshlets * MESHLET_MAX_VERTICES);
			meshlet_triangles.resize(maxMeshlets * MESHLET_MAX_PRIMITIVES * 3);
		}
	};
	struct mesh_static {
		std::vector<Vector3> position;
		std::vector<Vector3> normal;
		std::vector<Vector3> tangent;
		std::vector<Vector2> uv;
		std::vector<UINT> indices;

		mesh_lod lods[LOD_COUNT];		

		/* private data */
		aiMesh* p_aiMesh;
		aiNode* p_aiNode;

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
		mesh_static() = default;
		~mesh_static() = default;
	};

	static inline void load_static_meshes(const scene_t* scene, auto mesh_loader_callback) {
		auto process_static_mesh = [&](aiMesh* srcMesh, aiNode* parent) -> void {
			UINT num_vertices = srcMesh->mNumVertices;
			mesh_static mesh;
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
				if (srcMesh->mTextureCoords) {
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
			decltype(mesh.indices) remapping(num_vertices);
			meshopt_optimizeVertexFetchRemap(remapping.data(), mesh.indices.data(), mesh.indices.size(), num_vertices);
			mesh.remap(remapping);
			// LOD
			for (size_t lod = 0; lod < LOD_COUNT; lod++) {				
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
				mesh_lod.resize_max_meshlets(max_meshlets);
				size_t meshlet_count = meshopt_buildMeshlets(
					mesh_lod.meshlets.data(), mesh_lod.meshlet_vertices.data(), mesh_lod.meshlet_triangles.data(),
					mesh_lod.indices.data(), mesh_lod.indices.size(), (const float*)mesh.position.data(),
					num_vertices, sizeof(decltype(mesh.position)::value_type), MESHLET_MAX_VERTICES, MESHLET_MAX_PRIMITIVES, 0.0f
				);
				// Meshlet trimming
				const meshlet& last = mesh_lod.meshlets[meshlet_count - 1];
				mesh_lod.meshlet_vertices.resize(last.vertex_offset + last.vertex_count);
				mesh_lod.meshlet_triangles.resize((last.triangle_offset + ((last.triangle_count * 3 + 3) & ~3)));
				mesh_lod.meshlet_bounds.resize(meshlet_count);
				mesh_lod.meshlets.resize(meshlet_count);
				// Meshlet bounds
				for (size_t i = 0; i < mesh_lod.meshlets.size(); i++) {
					meshlet& meshlet = mesh_lod.meshlets[i];
					meshopt_Bounds bounds = meshopt_computeMeshletBounds(
						&mesh_lod.meshlet_vertices[meshlet.vertex_offset],
						&mesh_lod.meshlet_triangles[meshlet.triangle_offset],
						meshlet.triangle_count,
						(const float*)mesh.position.data(),
						mesh.position.size(),
						sizeof(decltype(mesh.position)::value_type)
					);
					mesh_lod.meshlet_bounds[i] = bounds;
				}
			}
			mesh.p_aiMesh = srcMesh;
			mesh.p_aiNode = parent;
			mesh_loader_callback(mesh);
		};
		auto dfs_nodes = [&](auto& func, aiNode* node) -> void {
			// http://pedromelendez.com/blog/2015/07/16/recursive-lambdas-in-c14/
			for (UINT i = 0; i < node->mNumMeshes; i++) {
				process_static_mesh(scene->mMeshes[node->mMeshes[i]], node);
			}
			for (UINT i = 0; i < node->mNumChildren; i++) {
				func(func, node);
			}
		};
		dfs_nodes(dfs_nodes, scene->mRootNode);
	}
}