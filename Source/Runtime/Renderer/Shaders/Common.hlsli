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