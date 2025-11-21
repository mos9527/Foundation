// Definitions From C++ sources
static const size_t kNumMeshDiscreteLODs = 5;
struct FVertex {
    float3 position;
    float3 normal;
    float2 uv;
};
struct FLODGroup // @ref clodGroup
{
    // DAG level the group was generated at
    int depth;

    // sphere bounds, in mesh coordinate space
    float3 center;
    float radius;

    // combined simplification error, in mesh coordinate space
    // FLT_MAX for terminal groups
    float error;
};
struct FMeshlet // @ref meshopt_Meshlet
{
    /* meshlet group */
    /* ID of the @ref FLODGroup this meshlet belongs to in a hierarchy */
    uint32_t group;
    /* ID of the @ref FLODGroup (with more triangles) that produced this meshlet during simplification (parent). ~0u if original geometry */
    uint32_t refined;

    /* meshlet */
    /* offsets within meshletVtx and meshletTri arrays with meshlet data */
    uint32_t vtxOffset; // in vertices
    uint32_t triOffset; // in indices (3*triangles)
    /* number of vertices and triangles used in the meshlet; data is stored in consecutive range defined by offset and
     * count */
    uint32_t vtxCount;
    uint32_t triCount;

    /* bounds */
    float4 centerRadius; // (x,y,z,r)
    float4 coneAxisAngle; // (x,y,z,cos(half solid angle))
    float3 coneApex; // (x,y,z)
};
struct GSMeshLOD
{
    // -- VS pipeline
    uint32_t indOffset;
    uint32_t indCount;
    // -- MS pipeline @ref FMeshlet
    uint32_t meshletOffset;
    uint32_t meshletCount;
    uint32_t meshletVtxOffset;
    uint32_t meshletTriOffset;
};
struct GSMesh
{
    // @ref FVertex
    uint32_t vtxOffset;
    uint32_t vtxCount;
    // -- DAG LODs @ref FLODGroup
    uint32_t groupOffset;
    uint32_t groupCount;
    // -- Discrete LODs
    uint32_t lodCount;
    GSMeshLOD lod[kNumMeshDiscreteLODs];
};
struct MIndex {
    uint8_t v0;
    uint8_t v1;
    uint8_t v2;
}

// 2D Polyhedral Bounds of a Clipped, Perspective-Projected 3D Sphere. Michael Mara, Morgan McGuire. 2013
bool projectSphere(float3 c, float r, float znear, float P00, float P11, out float4 aabb)
{
    // f = 1 / tan(fovY / 2), a = aspect ratio
    // P00 = f/a, P11 = f
	if (c.z < r + znear) // clipping near plane
		return false;

    P00 = abs(P00), P11 = abs(P11); // Ensure positive in case of NDC Y Flip

	float3 cr = c * r;
	float czr2 = c.z * c.z - r * r;

	float vx = sqrt(c.x * c.x + czr2);
	float minx = (vx * c.x - cr.z) / (vx * c.z + cr.x);
	float maxx = (vx * c.x + cr.z) / (vx * c.z - cr.x);

	float vy = sqrt(c.y * c.y + czr2);
	float miny = (vy * c.y - cr.z) / (vy * c.z + cr.y);
	float maxy = (vy * c.y + cr.z) / (vy * c.z - cr.y);

	aabb = float4(minx * P00, miny * P11, maxx * P00, maxy * P11);
	aabb = aabb.xwzy * float4(0.5f, -0.5f, 0.5f, -0.5f) + float4(0.5f); // clip space -> uv space

	return true;
}
