#ifndef MATH_INCLUDE
#define MATH_INCLUDE
#define M_PI 3.1415926535897932384626433832795
#define EPSILON 0.000001f
#define splat3(X) float3(X,X,X)
#define splat4(X) float4(X,X,X,X)
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
static const float3 BoxCornerOffset[] = {
    { -1.0f, -1.0f,  1.0f },
    {  1.0f, -1.0f,  1.0f },
    {  1.0f,  1.0f,  1.0f },
    { -1.0f,  1.0f,  1.0f },
    { -1.0f, -1.0f, -1.0f },
    {  1.0f, -1.0f, -1.0f },
    {  1.0f,  1.0f, -1.0f },
    { -1.0f,  1.0f, -1.0f }
};
struct BoundingBox // wraps BoundingBox { float3, float3 }
{
    float3 Center;
    float3 Extents;
    // Previously the one from https://github.com/kcloudy0717/Kaguya/blob/master/Source/Application/Kaguya/Shaders/Math.hlsli#L179
    // was used. Which was incorrect. Extents are not transformed correctly by rotation as it's simply rotated/scaled
    // This implementation once again comes from DirectXCollision
    BoundingBox Transform(matrix m)
    {
        BoundingBox b;
        float3 Corner = Extents * BoxCornerOffset[0] + Center;   
        float4 Corner4 = mul(float4(Corner, 1.0f), m);
        Corner = Corner4.xyz / Corner4.w;
        float3 Min = Corner, Max = Corner;
        for (int i = 1; i < 8; i++)
        {
            Corner = Extents * BoxCornerOffset[i] + Center;
            Corner4 = mul(float4(Corner, 1.0f), m);
            Corner = Corner4.xyz / Corner4.w;
            
            Min = min(Min, Corner);
            Max = max(Max, Corner);
        }
        b.Center = (Min + Max) / 2;
        b.Extents = (Max - Min) / 2;
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
    // the 8 vertices Vi = { E * U | E of {Ex,Ey,Ez}, U of {U1,U2,U3} }
    // projecting Vi to plane normal ( Vi - C ) dot N is simply
    // = { ... } * N
    // = Ex +- (U0 * N) + Ey +- (U1 * N) + Ez +- (U2 * N) 
    // = +-Ex * Nx +-Ey * Ny +-Ez * Nz
    // taking the maximum projected distance (in abs value) yeilds the max distance of all vertices to this plane:
    // = Ex * |Nx| + Ey * |Ny| + Ez * |Nz|
    // * see also https://github.com/kcloudy0717/Kaguya/blob/master/Source/Application/Kaguya/Shaders/Math.hlsli#L236
    // * see also DirectXCollision::FastIntersectAxisAlignedBoxPlane
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
struct BoundingSphere // wraps BoundingSphere { float3, float }
{
    float3 Center;
    float Radius;
    // https://jcgt.org/published/0002/02/05/
    // https://github.com/zeux/niagara/blob/master/src/shaders/math.h#L2
    bool ProjectRect(float nearZ, float p00, float p11, out float2 ndcXYMin, out float2 ndcXYMax)
    {
        if ((Center.z + Radius) < nearZ)
            return false;
        float3 cr = Center * Radius;
        float czr2 = Center.z * Center.z - Radius * Radius;

        float vx = sqrt(Center.x * Center.x + czr2);
        float minx = (vx * Center.x - cr.z) / (vx * Center.z + cr.x);
        float maxx = (vx * Center.x + cr.z) / (vx * Center.z - cr.x);

        float vy = sqrt(Center.y * Center.y + czr2);
        float miny = (vy * Center.y - cr.z) / (vy * Center.z + cr.y);
        float maxy = (vy * Center.y + cr.z) / (vy * Center.z - cr.y);

        ndcXYMin = float2(minx * p00, miny * p11);
        ndcXYMax = float2(maxx * p00, maxy * p11);
        return true;
    }
    BoundingSphere FromBox(BoundingBox box)
    {
        BoundingSphere sphere;
        sphere.Center = box.Center;
        sphere.Radius = length(box.Extents);
        return sphere;
    }
    BoundingSphere Transform(matrix m)
    {
        BoundingSphere s;
        float4 Center4 = mul(float4(Center, 1.0f), m);
        s.Center = Center4.xyz / Center4.w;
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
// https://github.com/KhronosGroup/SPIRV-Cross/blob/main/reference/shaders-hlsl/frag/unorm-snorm-packing.frag
float2 pack16toUnorm8(uint value)
{
    value = value & 0xffff;
    uint hi = value >> 8;
    uint low = value & 0xff;
    return clamp(float2(hi, low) / 255.0f, 0, 1);
}
uint unpackUnorm8to16(float2 value)
{
    uint2 packed = uint2(round(saturate(value) * 255.0));
    return packed.x << 8 | packed.y;
}
float2 unpackUnorm2x16(uint value)
{
    uint2 Packed = uint2(value & 0xffff, value >> 16);
    return float2(Packed) / 65535.0;
}

uint packSnorm2x16(float2 value)
{
    int2 Packed = int2(round(clamp(value, -1.0, 1.0) * 32767.0)) & 0xffff;
    return uint(Packed.x | (Packed.y << 16));
}
float clipZ2ViewZ(float Zss, float zNear, float zFar)
{
#ifdef INVERSE_Z
    return 2.0 * zNear * zFar / (zFar + zNear - (Zss * 2 - 1) * (zNear - zFar));
#endif
    return 2.0 * zNear * zFar / (zFar + zNear - (Zss * 2 - 1) * (zFar - zNear));
}
float2 clip2UV(float2 clip)
{
    return float2(0.5f, 0.5f) + clip * float2(0.5f, -0.5f);
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
        // see https://github.com/KhronosGroup/glTF-IBL-Sampler/blob/master/lib/source/shaders/filter.frag
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
float2 XYZToPanoUV(float3 dir)
{
    // tan(elevation) = z / x
    // cos(azimuth) = y
    // take the inverse and map into UV space
    return float2(0.5f + 0.5f * atan2(dir.z,dir.x) / M_PI, acos(dir.y) / M_PI);
}
float3 UV2WorldSpace(float2 UV, float Zss, matrix inverseViewProjection)
{
    float2 clipXY = UV2Clip(UV);
    float4 PositionProj = float4(clipXY, Zss, 1);
    float4 PositionWS = mul(PositionProj, inverseViewProjection);
    PositionWS.xyz /= PositionWS.w;
    return PositionWS.xyz;
}
// from https://github.com/microsoft/DirectX-Graphics-Samples/blob/master/Samples/Desktop/D3D12MeshShaders/src/DynamicLOD
float sphereScreenSpaceRadius(float3 center, float radius, float3 eye, float fov)
{
    float3 V = center - eye;
    float Rss = rcp(tan(fov / 2)) * radius / sqrt(dot(V, V) - radius * radius);
    return min(Rss, 1.0f);
}
// code from [Frisvad2012]
void buildOrthonormalBasis(
in float3 n, out float3 b1, out float3 b2)
{
    if (n.z < -0.9999999)
    {
        b1 = float3(0.0, -1.0, 0.0);
        b2 = float3(-1.0, 0.0, 0.0);
        return;
    }
    float a = 1.0 / (1.0 + n.z);
    float b = -n.x * n.y * a;
    b1 = float3(1.0 - n.x * n.x * a, b, -n.x);
    b2 = float3(b, 1.0 - n.y * n.y * a, -n.y);
}
// from https://github.com/TheRealMJP/DeferredTexturing/blob/master/SampleFramework12/v1.01/Shaders/Quaternion.hlsl#L18
#define Quaternion float4
float3 QuatRotate(in float3 v, in Quaternion q)
{
    float3 t = 2 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

Quaternion QuatFrom3x3(in float3x3 m)
{
    float3x3 a = transpose(m);
    Quaternion q;
    float trace = a[0][0] + a[1][1] + a[2][2];
    if (trace > 0)
    {
        float s = 0.5f / sqrt(trace + 1.0f);
        q.w = 0.25f / s;
        q.x = (a[2][1] - a[1][2]) * s;
        q.y = (a[0][2] - a[2][0]) * s;
        q.z = (a[1][0] - a[0][1]) * s;
    }
    else
    {
        if (a[0][0] > a[1][1] && a[0][0] > a[2][2])
        {
            float s = 2.0f * sqrt(1.0f + a[0][0] - a[1][1] - a[2][2]);
            q.w = (a[2][1] - a[1][2]) / s;
            q.x = 0.25f * s;
            q.y = (a[0][1] + a[1][0]) / s;
            q.z = (a[0][2] + a[2][0]) / s;
        }
        else if (a[1][1] > a[2][2])
        {
            float s = 2.0f * sqrt(1.0f + a[1][1] - a[0][0] - a[2][2]);
            q.w = (a[0][2] - a[2][0]) / s;
            q.x = (a[0][1] + a[1][0]) / s;
            q.y = 0.25f * s;
            q.z = (a[1][2] + a[2][1]) / s;
        }
        else
        {
            float s = 2.0f * sqrt(1.0f + a[2][2] - a[0][0] - a[1][1]);
            q.w = (a[1][0] - a[0][1]) / s;
            q.x = (a[0][2] + a[2][0]) / s;
            q.y = (a[1][2] + a[2][1]) / s;
            q.z = 0.25f * s;
        }
    }
    return q;
}

float3x3 QuatTo3x3(in Quaternion q)
{
    float3x3 m = float3x3(1.0f - 2.0f * q.y * q.y - 2.0f * q.z * q.z, 2.0f * q.x * q.y - 2.0f * q.z * q.w, 2.0f * q.x * q.z + 2.0f * q.y * q.w,
                          2.0f * q.x * q.y + 2.0f * q.z * q.w, 1.0f - 2.0f * q.x * q.x - 2.0f * q.z * q.z, 2.0f * q.y * q.z - 2.0f * q.x * q.w,
                          2.0f * q.x * q.z - 2.0f * q.y * q.w, 2.0f * q.y * q.z + 2.0f * q.x * q.w, 1.0f - 2.0f * q.x * q.x - 2.0f * q.y * q.y);
    return transpose(m);
}

float4x4 QuatTo4x4(in Quaternion q)
{
    float3x3 m3x3 = QuatTo3x3(q);
    return float4x4(m3x3._m00, m3x3._m01, m3x3._m02, 0.0f,
                    m3x3._m10, m3x3._m11, m3x3._m12, 0.0f,
                    m3x3._m20, m3x3._m21, m3x3._m22, 0.0f,
                    0.0f, 0.0f, 0.0f, 1.0f);
}
float4 PackQuaternion(in Quaternion q)
{
    Quaternion absQ = abs(q);
    float absMaxComponent = max(max(absQ.x, absQ.y), max(absQ.z, absQ.w));

    uint maxCompIdx = 0;
    float maxComponent = q.x;

    [unroll]
    for (uint i = 0; i < 4; ++i)
    {
        if (absQ[i] == absMaxComponent)
        {
            maxCompIdx = i;
            maxComponent = q[i];
        }
    }

    if (maxComponent < 0.0f)
        q *= -1.0f;

    float3 components;
    if (maxCompIdx == 0)
        components = q.yzw;
    else if (maxCompIdx == 1)
        components = q.xzw;
    else if (maxCompIdx == 2)
        components = q.xyw;
    else
        components = q.xyz;

    const float maxRange = 1.0f / sqrt(2.0f);
    components /= maxRange;
    components = components * 0.5f + 0.5f;

    return float4(components, maxCompIdx / 3.0f);
}
Quaternion UnpackQuaternion(in float4 packed)
{
    uint maxCompIdx = uint(packed.w * 3.0f);
    packed.xyz = packed.xyz * 2.0f - 1.0f;
    const float maxRange = 1.0f / sqrt(2.0f);
    packed.xyz *= maxRange;
    float maxComponent = sqrt(1.0f - saturate(packed.x * packed.x + packed.y * packed.y + packed.z * packed.z));

    Quaternion q;
    if (maxCompIdx == 0)
        q = Quaternion(maxComponent, packed.xyz);
    else if (maxCompIdx == 1)
        q = Quaternion(packed.x, maxComponent, packed.yz);
    else if (maxCompIdx == 2)
        q = Quaternion(packed.xy, maxComponent, packed.z);
    else
        q = Quaternion(packed.xyz, maxComponent);

    return q;
}
#endif