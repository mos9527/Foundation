module Culling;

// See also ModelViewer::Camera::GetCullParams
public bool frustumCull(float4 acbc /* [pLeft xy -> ac] [pTop zw -> bc] */, float3 center /* view space */, float radius) {
    // pLeft -> ax+cz = 0, pRight -> -ax+cz=0
    // pTop -> by+cz=0, pBottom -> -by+cz=0
    // Solve for left&right
    // -> dot(left, p) >= -r && dot(right, p) >= -r
    // -> ax+cz >= -r && -ax+cz >= -r
    // -> cz >= -r + |ax|
    // Same goes for top&bottom
    float ax = abs(acbc.x * center.x);
    float by = abs(acbc.z * center.y);
    float cz = acbc.y * center.z;
    float dz = acbc.w * center.z;
    return (cz >= -radius + ax) && (dz >= -radius + by);
}

// See also https://meshoptimizer.org/#mesh-shading
public bool coneCull(float3 apex, float3 axis /* normalized */, float angleCos, float3 eye) {
    float3 toApex = normalize(apex - eye);
    return dot(toApex, axis) <= angleCos;
}