#pragma once
#include "../../pch.hpp"
#ifdef RHI_USE_MESH_SHADER
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
#endif
struct MeshLod {
	std::vector<UINT> indices;
#ifdef RHI_USE_MESH_SHADER
	/* mesh shading */
	std::vector<Meshlet> meshlets; // offsets within meshlet_vertices , meshlet_bounds & meshlet_triangles
	std::vector<MeshletTriangle> meshlet_triangles; // primitive indices (3 verts i.e. i,i+1,i+2) into meshlet_vertices
	std::vector<UINT> meshlet_vertices; // indexes to mesh_static::{position,normal,tangent,uv}		
	void resize_max_meshlets(size_t maxMeshlets) {
		meshlets.resize(maxMeshlets);			
		meshlet_vertices.resize(maxMeshlets * MESHLET_MAX_VERTICES);
		meshlet_triangles.resize(maxMeshlets * MESHLET_MAX_PRIMITIVES);
	}
#endif
};

struct StaticMesh {
	std::string name = "<unamed>";

	std::vector<float3> position;
	std::vector<float3> normal;
	std::vector<float3> tangent;
	std::vector<float2> uv;
	std::vector<UINT> indices;

	MeshLod lods[MAX_LOD_COUNT];		

	inline size_t num_vertices() {
		return position.size();
	}

	inline void resize_vertices(size_t verts) {
		position.resize(verts);
		normal.resize(verts);
		tangent.resize(verts);
		uv.resize(verts);
	}

	void remap_vertices(std::vector<UINT>& remapping);
	StaticMesh() = default;
	~StaticMesh() = default;
};

struct SkinnedMesh {
	std::string name = "<unamed>";
	/*Vertex Data*/
	std::vector<float3> position;
	std::vector<float3> normal;
	std::vector<float3> tangent;
	std::vector<float2> uv;
	std::vector<uint4> boneIndices;
	std::vector<float4> boneWeights;	
	
	std::vector<UINT> indices;

	std::vector<StaticMesh> keyShapes;

	MeshLod lods[MAX_LOD_COUNT];

	inline size_t num_vertices() {
		return position.size();
	}

	void resize_vertices(size_t verts) {
		position.resize(verts);
		normal.resize(verts);
		tangent.resize(verts);
		uv.resize(verts);
		boneIndices.resize(verts);
		boneWeights.resize(verts);
	}

	void remap_vertices(std::vector<UINT>& remapping);

	SkinnedMesh() = default;
	~SkinnedMesh() = default;
};
struct aiMesh;
StaticMesh load_static_mesh(aiMesh* srcMesh, bool optimize = true);
SkinnedMesh load_skinned_mesh(aiMesh* srcMesh, std::unordered_map<std::string, uint> boneIDMapping = {}, std::unordered_map<std::string, uint> keyshapeIDMapping = {});
