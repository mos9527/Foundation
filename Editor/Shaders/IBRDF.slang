// Source: https://github.com/boksajak/referencePT/blob/master/shaders/brdf.h
float3 F_Schlick(float u, float3 f0, float3 f90 = float3(1.0)) {
    return f0 + (f90 - f0) * pow(1.0 - u, 5.0);
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
float D_GGX(float alphaSq, float NoH) {
	float b = ((alphaSq - 1.0f) * NoH * NoH + 1.0f);
	return alphaSq / (PI * b * b);
}
float G1_SmithGGX(float3 v, float alpha) {
    float NdotV = v.z;
    if (NdotV <= 0.0f) return 0.0f;

    float alpha2 = alpha * alpha;
    float NdotV2 = NdotV * NdotV;

    // Lambda function for GGX
    float tanTheta2 = (1.0f - NdotV2) / NdotV2;
    float lambda = 0.5f * (sqrt(1.0f + alpha2 * tanTheta2) - 1.0f);

    return 1.0f / (1.0f + lambda);
}
// Smith G1 term (masking function) optimized for GGX distribution (by substituting G_Lambda_GGX into G1)
float G1_SmithGGX(float a) {
	float a2 = a * a;
	return 2.0f / (sqrt((a2 + 1.0f) / a2) + 1.0f);
}
// Smith G1 term (masking function) further optimized for GGX distribution (by substituting G_a into G1_GGX)
float G1_SmithGGX(float alpha, float alphaSq, float NoSsq) {
	return 2.0f / (sqrt(((alphaSq * (1.0f - NoSsq)) + NoSsq) / NoSsq) + 1.0f);
}
// A fraction G2/G1 where G2 is height correlated can be expressed using only G1 terms
// Source: "Implementing a Simple Anisotropic Rough Diffuse Material with Stochastic Evaluation", Appendix A by Heitz & Dupuy
float G2_Over_G1_SmithHeightCorrelated(float alpha, float alphaSq, float NoL, float NoV) {
	float G1V = G1_SmithGGX(alpha, alphaSq, NoV * NoV);
	float G1L = G1_SmithGGX(alpha, alphaSq, NoL * NoL);
	return G1L / (G1V + G1L - G1V * G1L);
}
float V_SmithGGXCorrelated(float NoV, float NoL, float a) {
    float a2 = a * a;
    float GGXL = NoV * sqrt((-NoL * a2 + NoL) * NoL + a2);
    float GGXV = NoL * sqrt((-NoV * a2 + NoV) * NoV + a2);
    return 0.5 / (GGXV + GGXL);
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
float3 SpecularF0(float3 baseColor, float metallic)
{
    return lerp(float3(0.04f), baseColor, metallic);
}
float3 DiffuseReflectance(float3 baseColor, float metallic)
{
    return baseColor * (1.0f - metallic);
}
// Specular over specular+diffuse
float EvalSpecularBRDFProbability(float3 baseColor, float metallic, float3 v, float3 n)
{
    float VoN = saturate(dot(v, n)); // Half-vector not known
    float Fspec = dot(LUMINANCE_RGB, F_Schlick(VoN, SpecularF0(baseColor, metallic)));
    float Fdiff = dot(LUMINANCE_RGB, DiffuseReflectance(baseColor, metallic)) * (1 - Fspec);
    return clamp(Fspec / max(EPS, Fspec + Fdiff), EPS, 1.0f - EPS);
}
// https://jcgt.org/published/0007/04/01/
// PDF is 'G1(NoV) * D'
float3 SampleGGXVNDF(float3 Ve, float2 alpha2D, float2 u) {

	// Section 3.2: transforming the view direction to the hemisphere configuration
	float3 Vh = normalize(float3(alpha2D.x * Ve.x, alpha2D.y * Ve.y, Ve.z));

	// Section 4.1: orthonormal basis (with special case if cross product is zero)
	float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
	float3 T1 = lensq > 0.0f ? float3(-Vh.y, Vh.x, 0.0f) * rsqrt(lensq) : float3(1.0f, 0.0f, 0.0f);
	float3 T2 = cross(Vh, T1);

	// Section 4.2: parameterization of the projected area
	float r = sqrt(u.x);
	float phi = 2 * PI * u.y;
	float t1 = r * cos(phi);
	float t2 = r * sin(phi);
	float s = 0.5f * (1.0f + Vh.z);
	t2 = lerp(sqrt(1.0f - t1 * t1), t2, s);

	// Section 4.3: reprojection onto hemisphere
	float3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0f, 1.0f - t1 * t1 - t2 * t2)) * Vh;

	// Section 3.4: transforming the normal back to the ellipsoid configuration
	return normalize(float3(alpha2D.x * Nh.x, alpha2D.y * Nh.y, max(0.0f, Nh.z)));
}
// This should have D_GGX canceled out. Use SampleGGXVNDFWeight instead
float SampleGGXVNDFPdf(float alpha, float alphaSq, float NoH, float NoV, float LoH) {
	NoH = max(EPS, NoH);
	NoV = max(EPS, NoV);
	return (D_GGX(max(EPS, alphaSq), NoH) * G1_SmithGGX(alpha, alphaSq, NoV * NoV)) / (4.0f * NoV);
}
float SampleGGXVNDFWeight(float alpha, float alphaSq, float NoL, float NoV) {
    return G2_Over_G1_SmithHeightCorrelated(alpha, alphaSq, NoL, NoV);
}