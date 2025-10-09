module Plane;
public struct Plane {
    float4 abcd; // ax + by + cz + d = 0
    public __init(float4 p) { abcd = p; }
    public __init(float3 normal, float d) { abcd = float4(normal, d); }
    public __init(float a, float b, float c, float d) { abcd = float4(a, b, c, d); }
    public float3 normal() { return abcd.xyz; }
    public float d() { return abcd.w; }
    public float distance(float3 p) { return dot(abcd.xyz, p) + abcd.w; }
    public float operator*(float3 p) { return distance(p); }
}
