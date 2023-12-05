#include "MeshImporter.hpp"
#include <assimp/Importer.hpp>      // C++ importer interface
#include <assimp/scene.h>           // Output data structure
#include <assimp/postprocess.h>
#include <meshoptimizer.h>

void StaticMesh::remap_vertices(std::vector<UINT>& remapping) {
	meshopt_remapVertexBuffer(position.data(), position.data(), position.size(), sizeof(decltype(position)::value_type), remapping.data());
	meshopt_remapVertexBuffer(normal.data(), normal.data(), normal.size(), sizeof(decltype(normal)::value_type), remapping.data());
	meshopt_remapVertexBuffer(tangent.data(), tangent.data(), tangent.size(), sizeof(decltype(tangent)::value_type), remapping.data());
	meshopt_remapVertexBuffer(uv.data(), uv.data(), uv.size(), sizeof(decltype(uv)::value_type), remapping.data());
}

void SkinnedMesh::remap_vertices(std::vector<UINT>& remapping) {
	meshopt_remapVertexBuffer(position.data(), position.data(), position.size(), sizeof(decltype(position)::value_type), remapping.data());
	meshopt_remapVertexBuffer(normal.data(), normal.data(), normal.size(), sizeof(decltype(normal)::value_type), remapping.data());
	meshopt_remapVertexBuffer(tangent.data(), tangent.data(), tangent.size(), sizeof(decltype(tangent)::value_type), remapping.data());
	meshopt_remapVertexBuffer(uv.data(), uv.data(), uv.size(), sizeof(decltype(uv)::value_type), remapping.data());
	meshopt_remapVertexBuffer(boneIndices.data(), boneIndices.data(), boneIndices.size(), sizeof(decltype(boneIndices)::value_type), remapping.data());
	meshopt_remapVertexBuffer(boneWeights.data(), boneWeights.data(), boneWeights.size(), sizeof(decltype(boneWeights)::value_type), remapping.data());
}

template<typename T> void optimize_mesh(T& mesh) {
	// Index reording
	meshopt_optimizeVertexCache(mesh.indices.data(), mesh.indices.data(), mesh.indices.size(), mesh.position.size());
	meshopt_optimizeOverdraw(mesh.indices.data(), mesh.indices.data(), mesh.indices.size(), (const float*)mesh.position.data(), mesh.position.size(), sizeof(decltype(mesh.position)::value_type), 1.05f);
	// Vertex reording
	std::vector<UINT> remapping(mesh.position.size());
	meshopt_optimizeVertexFetchRemap(remapping.data(), mesh.indices.data(), mesh.indices.size(), mesh.position.size());
	mesh.remap_vertices(remapping);		
	meshopt_remapIndexBuffer(mesh.indices.data(), mesh.indices.data(), mesh.indices.size(), remapping.data());
}

template<typename T> void generate_lod(T& mesh) {
	float ratio = 1.0f;
	constexpr float step = 0.75f;
	for (size_t lod = 0; lod < MAX_LOD_COUNT; lod++) {
		const float target_error = 0.01f;
		size_t target_index_count = mesh.indices.size() * ratio;
		ratio *= step;
		auto& mesh_lod = mesh.lods[lod];
		mesh_lod.indices.resize(mesh.indices.size());
		float output_error = 0.0f;
		size_t output_index_count = meshopt_simplify(
			mesh_lod.indices.data(), mesh.indices.data(), mesh.indices.size(),
			(const float*)mesh.position.data(), mesh.position.size(), sizeof(decltype(mesh.position)::value_type),
			target_index_count, target_error, 0, &output_error
		);
		mesh_lod.indices.resize(output_index_count);
#ifdef RHI_USE_MESH_SHADER
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
#endif
	}
}

StaticMesh load_static_mesh(aiMesh* srcMesh, bool optimize) {
	StaticMesh mesh;
	UINT num_vertices = srcMesh->mNumVertices;
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
	if (optimize) 
		optimize_mesh(mesh);
	generate_lod(mesh);
	if (srcMesh->mName.length) 
		mesh.name = srcMesh->mName.C_Str();
	return mesh;
}

SkinnedMesh load_skinned_mesh(aiMesh* srcMesh) {
	SkinnedMesh mesh;
	UINT num_vertices = srcMesh->mNumVertices;
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
	// optimize_mesh(mesh);
	generate_lod(mesh);
	std::vector<char> boneWeightCount(mesh.position.size());
	for (UINT i = 0; i < srcMesh->mNumBones; i++) {
		aiBone* bone = srcMesh->mBones[i];
		mesh.boneNames.push_back(bone->mName.C_Str());
		mesh.boneInvBindMatrices.push_back(XMMatrixTranspose(XMMATRIX(&bone->mOffsetMatrix.a1)));
		for (UINT j = 0; j < bone->mNumWeights; j++) {
			aiVertexWeight weight = bone->mWeights[j];			
			auto write_xyzw_by_index = [](uint index, auto value, auto& to) {
				if (index == 0) to.x = value;
				else if (index == 1) to.y = value;
				else if (index == 2) to.z = value;
				else if (index == 3) to.w = value;
				else assert(false && "OOB");
			};
			write_xyzw_by_index(boneWeightCount[weight.mVertexId], i, mesh.boneIndices[weight.mVertexId]);
			write_xyzw_by_index(boneWeightCount[weight.mVertexId], weight.mWeight, mesh.boneWeights[weight.mVertexId]);
			boneWeightCount[weight.mVertexId]++;			
		}
	}
	for (UINT i = 0; i < srcMesh->mNumAnimMeshes; i++) {
		aiAnimMesh* animMesh = srcMesh->mAnimMeshes[i];
		StaticMesh& keyShape = mesh.keyShapes.emplace_back();
		keyShape.resize_vertices(animMesh->mNumVertices);
		for (UINT j = 0; j < animMesh->mNumVertices; j++) {
			if (animMesh->mVertices) {
				keyShape.position[j].x = animMesh->mVertices[j].x - srcMesh->mVertices[j].x;
				keyShape.position[j].y = animMesh->mVertices[j].y - srcMesh->mVertices[j].y;
				keyShape.position[j].z = animMesh->mVertices[j].z - srcMesh->mVertices[j].z;
			}
			if (animMesh->mNormals) {
				keyShape.normal[j].x = animMesh->mNormals[j].x - srcMesh->mNormals[j].x;
				keyShape.normal[j].y = animMesh->mNormals[j].y - srcMesh->mNormals[j].y;
				keyShape.normal[j].z = animMesh->mNormals[j].z - srcMesh->mNormals[j].z;
			}
			if (animMesh->mTangents) {
				keyShape.tangent[j].x = animMesh->mTangents[j].x - srcMesh->mTangents[j].x;
				keyShape.tangent[j].y = animMesh->mTangents[j].y - srcMesh->mTangents[j].y;
				keyShape.tangent[j].z = animMesh->mTangents[j].z - srcMesh->mTangents[j].z;
			}
			if (animMesh->mTextureCoords[0]) {
				keyShape.uv[j].x = animMesh->mTextureCoords[0][j].x - srcMesh->mTextureCoords[0][j].x;
				keyShape.uv[j].y = animMesh->mTextureCoords[0][j].y - srcMesh->mTextureCoords[0][j].y;
			}
		}
		// optimize_mesh(keyShape);
		generate_lod(keyShape);
		keyShape.name = animMesh->mName.C_Str();
	}
	if (srcMesh->mName.length)
		mesh.name = srcMesh->mName.C_Str();
	return mesh;
}