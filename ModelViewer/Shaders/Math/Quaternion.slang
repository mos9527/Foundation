module Quaternion;
public struct Quaternion
{
    float4 q; // (x, y, z, w)
    public __init(float4 v) { q = v; }
    public __init(float3 axis, float angle) {
        float halfAngle = angle * 0.5;
        float s = sin(halfAngle);
        q = float4(axis * s, cos(halfAngle));
    }
    // Rotate a vector by this quaternion
    public float3 rotate(float3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }
    // Multiply two quaternions
    public Quaternion mul(Quaternion r) {
        return Quaternion(float4(
            q.w * r.q.x + q.x * r.q.w + q.y * r.q.z - q.z * r.q.y,
            q.w * r.q.y - q.x * r.q.z + q.y * r.q.w + q.z * r.q.x,
            q.w * r.q.z + q.x * r.q.y - q.y * r.q.x + q.z * r.q.w,
            q.w * r.q.w - q.x * r.q.x - q.y * r.q.y - q.z * r.q.z
        ));
    }
    // Overload
    public Quaternion operator*(Quaternion r) { return mul(r); }
};
