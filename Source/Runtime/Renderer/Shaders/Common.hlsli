#ifndef COMMON_INCLUDE_HLSL
#define COMMON_INCLUDE_HLSL
#include "Shared.h"
#define DIELECTRIC_SPECULAR 0.08 // Influenced by Specular (which starts at 0.5)
#define M_PI 3.1415926535897932384626433832795
#define EPISILON 0.000001f
#define splat3(X) float3(X,X,X)
float clampedDot(float3 v1, float3 v2)
{
    return saturate(dot(v1, v2));
}
float2 encodeSpheremapNormal(float3 N)
{
    return (normalize(N.xy) * sqrt(N.z * 0.5 + 0.5));
}
float3 decodeSpheremapNormal(float2 G)
{
    float3 N;
    N.z = dot(G.xy, G.xy) * 2 - 1;
    N.xy = normalize(G.xy) * sqrt(1 - N.z * N.z);
    return N;
}
float3 decodeTangetNormalMap(float3 sample, float3 Tv, float3 Nv)
{
    float3 N = normalize(Nv);
    float3 T = normalize(Tv - dot(Tv, N) * N);
    float3 B = cross(N, T);
    float3 tN = sample * 2 - 1;
    N = normalize(tN.x * T + tN.y * B + tN.z * N);
    return N;
}
float ClipZ2ViewZ(float Zss, float zNear, float zFar)
{
#ifdef INVERSE_Z
    return 2.0 * zNear * zFar / (zFar + zNear - (Zss * 2 - 1) * (zNear - zFar));
#endif
    return 2.0 * zNear * zFar / (zFar + zNear - (Zss * 2 - 1) * (zFar - zNear));
}
float2 UV2Clip(float2 UV)
{
    UV.y = 1 - UV.y;
    return UV * 2 - float2(1, 1);
}
float3 UV2XYZ(float2 uv, uint face)
{
    uv = 2 * uv - float2(1, 1);
    uv.y *= -1;
    float3 R;
    switch (face)
    {
        // ** https://learn.microsoft.com/en-us/windows/win32/direct3d11/overviews-direct3d-11-resources-textures-intro
        case 0: // +x
            R = float3(1.0, uv.y, -uv.x);
            break;
        case 1: // -x
            R = float3(-1.0, uv.y, uv.x);
            break;
        case 2: // +y
            R = float3(uv.x, 1.0, -uv.y);
            break;
        case 3: // -y
            R = float3(uv.x, -1.0, uv.y);
            break;
        case 4: // +z
            R = float3(uv.x, uv.y, 1.0);
            break;
        default: // -z
            R = float3(-uv.x, uv.y, -1.0);
            break;
    }
    return R;
}
float3 UV2WorldSpace(float2 UV, float Zss, matrix inverseViewProjection)
{
    float2 clipXY = UV2Clip(UV);
    float4 PositionProj = float4(clipXY, Zss, 1);
    float4 PositionWS = mul(PositionProj, inverseViewProjection);
    PositionWS /= PositionWS.w;
    return PositionWS.xyz;
}
#define INTERSECT_PLANE_POSITIVE_HS 1
#define INTERSECT_PLANE_NEGATIVE_HS 2
#define INTERSECT_PLANE_INTERSECTION 3
int intersectSphereToPlane(float3 center, float radius, float4 plane)
{    
    float3 normal = plane.xyz;
    float offset = plane.w;
    
    float dist = dot(center, normal) + offset;
    
    if (dist > radius)
        return INTERSECT_PLANE_POSITIVE_HS;
    if (dist < -radius)
        return INTERSECT_PLANE_NEGATIVE_HS;
    return INTERSECT_PLANE_INTERSECTION;
}
// given AABB center C extents Ex,Ey,Ez, with U1(1,0,0) U2(0,1,0) U3(0,0,1) as axis...
// the 6 vertices Vi = { E * U | E of {Ex,Ey,Ez}, U of {U1,U2,U3} }
// projecting Vi to plane normal ( Vi - C ) dot N is simply
// = { ... } * N
// = Ex +- (U0 * N) + Ey +- (U1 * N) + Ez +- (U2 * N) 
// = +-Ex * Nx +-Ey * Ny +-Ez * Nz
// taking the maximum projected distance (in abs value) yeilds the max distance of all vertices to this plane:
// = Ex * |Nx| + Ey * |Ny| + Ez * |Nz|
// * see also https://github.com/kcloudy0717/Kaguya/blob/master/Source/Application/Kaguya/Shaders/Math.hlsli#L236
int intersectAABBToPlane(float3 center, float3 extents, float4 plane)
{
    float3 normal = plane.xyz;
    float offset = plane.w;
    
    float dist = dot(center, normal) + offset;
    float radius = dot(extents, abs(normal));
    
    if (dist > radius)
        return INTERSECT_PLANE_POSITIVE_HS;
    if (dist < -radius)
        return INTERSECT_PLANE_NEGATIVE_HS;
    return INTERSECT_PLANE_INTERSECTION;
}
#define INTERSECT_VOLUMES_DISJOINT 0
#define INTERSECT_VOLUMES_CONTAIN 1
#define INTERSECT_VOLUMES_INTERSECT 2
int intersectSphereToFrustum(float3 center, float radius, float4 left, float4 right, float4 bottom, float4 top, float4 near, float4 far)
{
    int i0 = intersectSphereToPlane(center, radius, left);
    int i1 = intersectSphereToPlane(center, radius, right);
    int i2 = intersectSphereToPlane(center, radius, bottom);
    int i3 = intersectSphereToPlane(center, radius, top);
    int i4 = intersectSphereToPlane(center, radius, near);
    int i5 = intersectSphereToPlane(center, radius, far);
    int outside = i0 == INTERSECT_PLANE_NEGATIVE_HS;
    outside += i1 == INTERSECT_PLANE_NEGATIVE_HS;
    outside += i2 == INTERSECT_PLANE_NEGATIVE_HS;
    outside += i3 == INTERSECT_PLANE_NEGATIVE_HS;
    outside += i4 == INTERSECT_PLANE_NEGATIVE_HS;
    outside += i5 == INTERSECT_PLANE_NEGATIVE_HS;
    int inside = i0 == INTERSECT_PLANE_POSITIVE_HS;
    inside += i1 == INTERSECT_PLANE_POSITIVE_HS;
    inside += i2 == INTERSECT_PLANE_POSITIVE_HS;
    inside += i3 == INTERSECT_PLANE_POSITIVE_HS;
    inside += i4 == INTERSECT_PLANE_POSITIVE_HS;
    inside += i5 == INTERSECT_PLANE_POSITIVE_HS;
    if (outside != 0)
        return INTERSECT_VOLUMES_DISJOINT;
    if (inside == 6)
        return INTERSECT_VOLUMES_CONTAIN;
    return INTERSECT_VOLUMES_INTERSECT;
}
int intersectAABBToFrustum(float3 center, float3 extents, float4 left, float4 right, float4 bottom, float4 top, float4 near, float4 far)
{
    int i0 = intersectAABBToPlane(center, extents, left);
    int i1 = intersectAABBToPlane(center, extents, right);
    int i2 = intersectAABBToPlane(center, extents, bottom);
    int i3 = intersectAABBToPlane(center, extents, top);
    int i4 = intersectAABBToPlane(center, extents, near);
    int i5 = intersectAABBToPlane(center, extents, far);
    int outside = i0 == INTERSECT_PLANE_NEGATIVE_HS;
    outside += i1 == INTERSECT_PLANE_NEGATIVE_HS;
    outside += i2 == INTERSECT_PLANE_NEGATIVE_HS;
    outside += i3 == INTERSECT_PLANE_NEGATIVE_HS;
    outside += i4 == INTERSECT_PLANE_NEGATIVE_HS;
    outside += i5 == INTERSECT_PLANE_NEGATIVE_HS;
    int inside = i0 == INTERSECT_PLANE_POSITIVE_HS;
    inside += i1 == INTERSECT_PLANE_POSITIVE_HS;
    inside += i2 == INTERSECT_PLANE_POSITIVE_HS;
    inside += i3 == INTERSECT_PLANE_POSITIVE_HS;
    inside += i4 == INTERSECT_PLANE_POSITIVE_HS;
    inside += i5 == INTERSECT_PLANE_POSITIVE_HS;
    if (outside != 0)
        return INTERSECT_VOLUMES_DISJOINT;
    if (inside == 6)
        return INTERSECT_VOLUMES_CONTAIN;
    return INTERSECT_VOLUMES_INTERSECT;
}
// from https://github.com/microsoft/DirectX-Graphics-Samples/blob/master/Samples/Desktop/D3D12MeshShaders/src/DynamicLOD
float sphereScreenSpaceRadius(float3 center, float radius,float3 eye,float fov)
{
    float3 V = center - eye;        
    float Rss = rcp(tan(fov / 2)) * radius / sqrt(dot(V,V) - radius * radius);        
    return min(Rss, 1.0f);
}
#endif