float3 F_Schlick(float u, float3 f0) {
    return f0 + (float3(1.0) - f0) * pow(1.0 - u, 5.0);
}
// float at = max(roughness * (1.0 + anisotropy), 0.001);
// float ab = max(roughness * (1.0 - anisotropy), 0.001);
float D_GGX_Anisotropic(float NoH, const float3 h, const float3 t, const float3 b, float at, float ab) {
    float ToH = dot(t, h);
    float BoH = dot(b, h);
    float a2 = at * ab;
    float3 v = float3(ab * ToH, at * BoH, a2 * NoH);
    float v2 = dot(v, v);
    float w2 = a2 / v2;
    return a2 * w2 * w2 * (1.0 / PI);
}
float V_SmithGGXCorrelated_Anisotropic(float at, float ab, float ToV, float BoV,
        float ToL, float BoL, float NoV, float NoL) {
    float lambdaV = NoL * length(float3(at * ToV, ab * BoV, NoV));
    float lambdaL = NoV * length(float3(at * ToL, ab * BoL, NoL));
    float v = 0.5 / (lambdaV + lambdaL);
    return saturate(v);
}
// https://pbr-book.org/4ed/Sampling_Algorithms/Sampling_Multidimensional_Functions#SamplingaUnitDisk
float2 SampleUniformDiskPolar(float2 u){
    float r = sqrt(u.x);
    float theta = 2 * PI * u.y;
    return float2(r * cos(theta), r * sin(theta));
}
// https://pbr-book.org/4ed/Sampling_Algorithms/Sampling_Multidimensional_Functions#UniformlySamplingHemispheresandSpheres
float3 SampleUniformSphere(float2 u) {
    float z = 1 - 2 * u[0];
    float r = sqrt(1 - sqrt(z));
    float phi = 2 * PI * u[1];
    return float3(r * cos(phi), r * (phi), z);
}
// https://pbr-book.org/4ed/Sampling_Algorithms/Sampling_Multidimensional_Functions#Cosine-WeightedHemisphereSampling
float3 SampleCosineHemisphere(float2 u) {
    float2 d = SampleUniformDiskPolar(u);
    float z = sqrt(max(0.0, 1.0 - d.x * d.x - d.y * d.y));
    return float3(d.x, d.y, z);
}
float CosineHemispherePDF(float cosTheta) {
    return cosTheta / PI;
}
