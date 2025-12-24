// Reference: https://projects.blender.org/blender/blender/src/commit/00546eb2f34cc95976a640d268deb371b7ca9210/intern/cycles/app/cycles_precompute.cpp
import "../Editor/Shaders/IMath";
import "../Editor/Shaders/IBRDF";
RWStructuredBuffer<float> output : register(u0);

float sampleGGX_E(float rough, float cosTheta, float uc, float2 u){
    float alpha = Sqr(rough);
    TrowbridgeReitzDistribution mfDistrib = TrowbridgeReitzDistribution(alpha, alpha);
    if (mfDistrib.EffectivelySmooth())
        return 1.0f; // Dirac delta - though we're conuting energy, so return 1
    float3 wo = float3(sqrt(1.0 - cosTheta * cosTheta), 0.0, cosTheta);
    float3 wm = mfDistrib.Sample_wm(wo, u);
    float3 wi = Reflect(wo, wm);    
    if (!SameHemisphere(wo, wi))
        return 0.0f; // Absorbtion
    float pdf = mfDistrib.PDF(wo, wm) / (4 * AbsDot(wo, wm));
    float f = mfDistrib.D(wm)  * mfDistrib.G(wo, wi) / (4 * AbsCosTheta(wi) * AbsCosTheta(wo));
    return f * AbsCosTheta(wi) / pdf;
}

[shader("compute")]
[numthreads(32,32,1)]
void integrateGGX_E(uint2 p : SV_DispatchThreadID)
{
    static const uint kSamples = 1 << 12;
    float sum = 0.0f, samples = 0.0f;
    float u = (float(p.x) + 0.5f) / 32.0f, v = (float(p.y) + 0.5f) / 32.0f;
    PCG rng = PCG(uint4(p, 0, 0));
    for (uint i = 0; i < kSamples;i++) {
        float cosTheta = clamp(u, 1e-4, 1.0f);
        float rough = clamp(v, 1e-4, 1.0f);  
        sum += sampleGGX_E(rough, cosTheta, rng.sample(), rng.sample2D());
        samples += 1.0f;        
    }
    output[dot(p, uint2(1, 32))] = sum / samples;
}

[shader("compute")]
[numthreads(32,1,1)]
void integrateGGX_Eavg(uint2 p : SV_DispatchThreadID)
{
    static const uint kSamples = 1 << 14;
    float sum = 0.0f, samples = 0.0f;
    float u = (float(p.x) + 0.5f) / 32.0f;
    PCG rng = PCG(uint4(p, 0, 0));
    for (uint i = 0; i < kSamples;i++) {        
        float cosTheta = rng.sample();
        float rough = clamp(u, 1e-4, 1.0f);  
        sum += 2 * cosTheta * sampleGGX_E(rough, cosTheta, rng.sample(), rng.sample2D());
        samples += 1.0f;        
    }
    output[dot(p, uint2(1, 32))] = sum / samples;
}
