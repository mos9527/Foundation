import ICommon;
import IMath;
RWByteAddressBuffer dynamicPrimitives;

struct PushConstant
{
    uint32_t vertexOffset;
    uint32_t indexOffset;
    uint32_t gridQuads;
    float extent;
    float time;
    float amplitudeScale;
};
[[vk::push_constant]] PushConstant pc;

struct GerstnerWave
{
    float2 direction;
    float amplitude;
    float wavelength;
    float speed;
    float steepness;
};

void SampleGerstner(float2 xz0, out float3 position, out float3 normal, out float3 tangent)
{
    GerstnerWave waves[4] = {
        {float2(1.0, 0.15), 0.08, 1.8, 1.6, 0.7},
        {float2(0.35, 1.0), 0.05, 1.1, 2.1, 0.55},
        {float2(-0.7, 0.6), 0.035, 0.7, 2.6, 0.4},
        {float2(0.2, -0.9), 0.02, 0.45, 3.2, 0.3},
    };
    position = float3(xz0.x, 0.0, xz0.y);
    float3 dPdx = float3(1, 0, 0);
    float3 dPdz = float3(0, 0, 1);
    for (uint32_t i = 0; i < 4; ++i)
    {
        float2 D = normalize(waves[i].direction);
        float A = waves[i].amplitude * pc.amplitudeScale;
        float k = 2.0 * Pi / waves[i].wavelength;
        float phase = k * dot(D, xz0) - waves[i].speed * pc.time;
        float S, C;
        sincos(phase, S, C);
        float QA = waves[i].steepness * A;
        float kA = k * A;
        float QkA = waves[i].steepness * kA;

        position += float3(QA * D.x * C, A * S, QA * D.y * C);
        dPdx += float3(-QkA * D.x * D.x * S, kA * D.x * C, -QkA * D.x * D.y * S);
        dPdz += float3(-QkA * D.x * D.y * S, kA * D.y * C, -QkA * D.y * D.y * S);
    }
    tangent = normalize(dPdx);
    normal = normalize(cross(dPdz, dPdx));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint32_t id : SV_DispatchThreadID)
{
    uint32_t side = pc.gridQuads + 1u;
    uint32_t vertexCount = side * side;
    if (id < vertexCount)
    {
        uint32_t x = id % side;
        uint32_t z = id / side;
        float2 uv = float2(x, z) / float(pc.gridQuads);
        float2 xz0 = (uv - 0.5) * pc.extent;
        float3 position, normal, tangent;
        SampleGerstner(xz0, position, normal, tangent);
        dynamicPrimitives.Store<FQVertex>(
            pc.vertexOffset + id * sizeof(FQVertex),
            FQPack(position, normal, tangent, 1.0, uv));
    }

    uint32_t quadCount = pc.gridQuads * pc.gridQuads;
    if (id < quadCount)
    {
        uint32_t x = id % pc.gridQuads;
        uint32_t z = id / pc.gridQuads;
        uint32_t i0 = z * side + x;
        uint32_t i1 = i0 + 1u;
        uint32_t i2 = i0 + side;
        uint32_t i3 = i2 + 1u;
        uint32_t indexOffset = pc.indexOffset + id * 6u * sizeof(uint32_t);
        dynamicPrimitives.Store<uint32_t>(indexOffset + 0u, i0);
        dynamicPrimitives.Store<uint32_t>(indexOffset + 4u, i2);
        dynamicPrimitives.Store<uint32_t>(indexOffset + 8u, i1);
        dynamicPrimitives.Store<uint32_t>(indexOffset + 12u, i1);
        dynamicPrimitives.Store<uint32_t>(indexOffset + 16u, i2);
        dynamicPrimitives.Store<uint32_t>(indexOffset + 20u, i3);
    }
}
