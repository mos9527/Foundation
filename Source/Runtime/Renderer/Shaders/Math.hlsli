#ifndef MATH_INCLUDE
#define MATH_INCLUDE
#define M_PI 3.1415926535897932384626433832795
#define EPISILON 0.000001f
#define splat3(X) float3(X,X,X)
/* MATRIX OPERATIONS */
float3 transformToScalePow2(matrix m)
{
    return float3(
        dot(float3(m._11, m._21, m._31), float3(m._11, m._21, m._31)),
        dot(float3(m._12, m._22, m._32), float3(m._12, m._22, m._32)),
        dot(float3(m._31, m._32, m._33), float3(m._31, m._32, m._33))
    );
}
float3 transformToScale(matrix m)
{
    float3 pow2 = transformToScalePow2(m);
    return float3(sqrt(pow2.x), sqrt(pow2.y), sqrt(pow2.z));
}
/* COLLISIONS */ 
// Between plane-volume
#define INTERSECT_PLANE_POSITIVE_HS 1
#define INTERSECT_PLANE_NEGATIVE_HS 2
#define INTERSECT_PLANE_INTERSECTION 3
// Between volumes
#define INTERSECT_VOLUMES_DISJOINT 4
#define INTERSECT_VOLUMES_CONTAIN 5
#define INTERSECT_VOLUMES_INTERSECT 6
struct Plane // wraps XMFLOAT4 { .xyz = Normal, .w = Offset }
{
    float3 Normal;
    float Offset;
};
struct BoundingSphere // wraps BoundingSphere { float3, float }
{
    float3 Center;
    float Radius;
    BoundingSphere Transform(matrix m)
    {
        BoundingSphere s;
        s.Center = mul(float4(Center, 1.0f), m).xyz;
        float3 scales = transformToScalePow2(m);
        s.Radius = Radius * sqrt(max(scales.x, max(scales.y, scales.z)));
        return s;
    }
    bool Contains(BoundingSphere Other)
    {
        float3 d = Center - Other.Center;
        float dist2 = dot(d, d);
        float radiusDiff = Radius - Other.Radius;
        return radiusDiff > 0 && dist2 <= radiusDiff * radiusDiff;
    }
    bool Intersects(BoundingSphere Other)
    {
        float3 d = Center - Other.Center;
        float dist2 = dot(d, d);
        float radiusSum = Radius + Other.Radius;
        return dist2 <= radiusSum * radiusSum;
    }
    int Intersect(Plane Other)
    {
        float dist = dot(Center, Other.Normal) + Other.Offset;
        if (dist > Radius)
            return INTERSECT_PLANE_POSITIVE_HS;
        if (dist < -Radius)
            return INTERSECT_PLANE_NEGATIVE_HS;
        return INTERSECT_PLANE_INTERSECTION;
    }
    int Intersect(Plane left, Plane right, Plane bottom, Plane top, Plane near, Plane far)
    {
        int i0 = Intersect(left);
        int i1 = Intersect(right);
        int i2 = Intersect(bottom);;
        int i3 = Intersect(top);
        int i4 = Intersect(near);
        int i5 = Intersect(far);
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
};
struct BoundingBox // wraps BoundingBox { float3, float3 }
{
    float3 Center;
    float3 Extents;
    BoundingBox Transform(matrix m)
    {
        float3 t = m[3].xyz;
        BoundingBox b;
        b.Center = t;
        b.Extents = float3(0.0f, 0.0f, 0.0f);
        for (int i = 0; i < 3; ++i)
        {
            for (int j = 0; j < 3; ++j)
            {
                b.Center[i] += m[i][j] * Center[j];
                b.Extents[i] += abs(m[i][j]) * Extents[j];
            }
        }
        return b;
    }
    bool Intersects(BoundingBox Other)
    {
        float3 minA = Center - Extents;
        float3 maxA = Center + Extents;

        float3 minB = Other.Center - Other.Extents;
        float3 maxB = Other.Center + Other.Extents;

		// for each i in (x, y, z) if a_min(i) > b_max(i) or b_min(i) > a_max(i) then return false
        return maxA.x >= minB.x && minA.x <= maxB.x &&
			   maxA.y >= minB.y && minA.y <= maxB.y &&
			   maxA.z >= minB.z && minA.z <= maxB.z;
    }
    bool Contains(BoundingBox Other)
    {
        float3 minA = Center - Extents;
        float3 maxA = Center + Extents;

        float3 minB = Other.Center - Other.Extents;
        float3 maxB = Other.Center + Other.Extents;

		// for each i in (x, y, z) if a_min(i) <= b_min(i) and b_max(i) <= a_max(i) then A contains B
        return minA.x <= minB.x && maxB.x <= maxA.x &&
			   minA.y <= minB.y && maxB.y <= maxA.y &&
			   minA.z <= minB.z && maxB.z <= maxA.z;
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
    // * see also DirectXColision::FastIntersectAxisAlignedBoxPlane
    int Intersect(Plane Other)
    {
        float dist = dot(Center, Other.Normal) + Other.Offset;
        float radius = dot(Extents, abs(Other.Normal));
    
        if (dist > radius)
            return INTERSECT_PLANE_POSITIVE_HS;
        if (dist < -radius)
            return INTERSECT_PLANE_NEGATIVE_HS;
        return INTERSECT_PLANE_INTERSECTION;
    }
    int Intersect(Plane left, Plane right, Plane bottom, Plane top, Plane near, Plane far)
    {
        int i0 = Intersect(left);
        int i1 = Intersect(right);
        int i2 = Intersect(bottom);;
        int i3 = Intersect(top);
        int i4 = Intersect(near);
        int i5 = Intersect(far);
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
};
/* UTILITIES */
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
float clipZ2ViewZ(float Zss, float zNear, float zFar)
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
// from https://github.com/microsoft/DirectX-Graphics-Samples/blob/master/Samples/Desktop/D3D12MeshShaders/src/DynamicLOD
float sphereScreenSpaceRadius(float3 center, float radius, float3 eye, float fov)
{
    float3 V = center - eye;
    float Rss = rcp(tan(fov / 2)) * radius / sqrt(dot(V, V) - radius * radius);
    return min(Rss, 1.0f);
}
#endif