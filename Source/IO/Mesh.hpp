#pragma once
#include <assimp/Importer.hpp>      // C++ importer interface
#include <assimp/scene.h>           // Output data structure
#include <meshoptimizer.h>
#include <assimp/postprocess.h>
#include "IO.hpp"
namespace IO {
	typedef aiScene scene_t;
#define MESHLET_MAX_VERTICES 64u // https://developer.nvidia.com/blog/introduction-turing-mecacsh-shaders/
#define MESHLET_MAX_PRIMITIVES 124u // 4b aligned
	typedef meshopt_Meshlet Meshlet;
	typedef meshopt_Bounds MeshletBound;
	struct MeshletTriangle { UINT V0 : 10, V1 : 10, V2 : 10, : 2; /* 3 8-bits packed into 32-bit bitfields */ };
	struct StaticMesh {
		std::vector<Vector3> position;
		std::vector<Vector3> normal;
		std::vector<Vector3> tangent;
		std::vector<Vector2> uv;

		std::vector<UINT> indices;

		std::vector<Meshlet> meshlets;
		std::vector<MeshletBound> meshlet_bounds;
		std::vector<unsigned char> meshlet_triangles_u8; // primitive_indices
		std::vector<UINT> meshlet_vertices; // indexes to original indices buffer

		aiMesh* p_aiMesh;
		aiNode* p_aiNode;
		void swizzle_meshlet_triangles(std::vector<MeshletTriangle>& outTriangles) {
			for (size_t i = 0; i < meshlet_triangles_u8.size() - 3; i += 3) {
				MeshletTriangle triangle{};
				triangle.V0 = meshlet_triangles_u8[i];
				triangle.V1 = meshlet_triangles_u8[i + 1];
				triangle.V2 = meshlet_triangles_u8[i + 2];
				outTriangles.push_back(triangle);
			}
		}
		void resize_vertices(size_t verts) {
			position.resize(verts);
			normal.resize(verts);
			tangent.resize(verts);
			uv.resize(verts);
		}
		void resize_max_meshlets(size_t maxMeshlets) {
			meshlets.resize(maxMeshlets);
			meshlet_bounds.resize(maxMeshlets);
			meshlet_vertices.resize(maxMeshlets * MESHLET_MAX_VERTICES);
			meshlet_triangles_u8.resize(maxMeshlets * MESHLET_MAX_PRIMITIVES * 3);
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
	static inline void load_static_meshes(const scene_t* scene, auto mesh_loader_callback) {
		auto process_static_mesh = [&](aiMesh* srcMesh, aiNode* parent) -> void {
			UINT nVertices = srcMesh->mNumVertices;
			StaticMesh mesh;
			mesh.resize_vertices(nVertices);
			for (UINT i = 0; i < nVertices; i++) {
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
			meshopt_optimizeVertexCache(mesh.indices.data(), mesh.indices.data(), mesh.indices.size(), nVertices);
			meshopt_optimizeOverdraw(mesh.indices.data(), mesh.indices.data(), mesh.indices.size(), (const float*)mesh.position.data(), nVertices, sizeof(decltype(mesh.position)::value_type), 1.05f);
			// Vertex reording
			decltype(mesh.indices) remapping(nVertices);
			meshopt_optimizeVertexFetchRemap(remapping.data(), mesh.indices.data(), mesh.indices.size(), nVertices);
			mesh.remap(remapping);
			// TODO: LOD
			// xxx
			// Meshlet generation
			size_t max_meshlets = meshopt_buildMeshletsBound(mesh.indices.size(), MESHLET_MAX_VERTICES, MESHLET_MAX_PRIMITIVES);
			mesh.resize_max_meshlets(max_meshlets);
			size_t meshlet_count = meshopt_buildMeshlets(
				mesh.meshlets.data(), mesh.meshlet_vertices.data(), mesh.meshlet_triangles_u8.data(),
				mesh.indices.data(), mesh.indices.size(), (const float*)mesh.position.data(),
				nVertices, sizeof(decltype(mesh.position)::value_type), MESHLET_MAX_VERTICES, MESHLET_MAX_PRIMITIVES, 0.0f
			);
			// Meshlet trimming
			const Meshlet& last = mesh.meshlets[meshlet_count - 1];
			mesh.meshlet_vertices.resize(last.vertex_offset + last.vertex_count);
			mesh.meshlet_triangles_u8.resize((last.triangle_offset + ((last.triangle_count * 3 + 3) & ~3)));
			mesh.meshlet_bounds.resize(meshlet_count);
			mesh.meshlets.resize(meshlet_count);
			// Meshlet bounds
			for (size_t i = 0; i < mesh.meshlets.size(); i++) {
				Meshlet& meshlet = mesh.meshlets[i];
				meshopt_Bounds bounds = meshopt_computeMeshletBounds(
					&mesh.meshlet_vertices[meshlet.vertex_offset],
					&mesh.meshlet_triangles_u8[meshlet.triangle_offset],
					meshlet.triangle_count,
					(const float*)mesh.position.data(),
					mesh.position.size(),
					sizeof(decltype(mesh.position)::value_type)
				);
				mesh.meshlet_bounds[i] = bounds;
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