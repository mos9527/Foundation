// Reference: https://projects.blender.org/blender/blender/src/commit/00546eb2f34cc95976a640d268deb371b7ca9210/intern/cycles/app/cycles_precompute.cpp
import "../Source/Renderer/Shaders/IMath";
import "../Source/Renderer/Shaders/IBxDF";
RWStructuredBuffer<float2> ggxE;

float sampleGGX_E(float rough, float cosTheta, float uc, float2 u, out float VdotH){
    float alpha = Sqr(rough);
    TrowbridgeReitzDistribution mfDistrib = TrowbridgeReitzDistribution(alpha, alpha);
    float3 wo = float3(sqrt(1.0 - cosTheta * cosTheta), 0.0, cosTheta);
    if (mfDistrib.EffectivelySmooth()){
        float3 wi = float3(-wo.x, -wo.y, wo.z);
        float3 wm = normalize(wo + wi);
        VdotH = AbsDot(wo, wm);
        return 1.0f; // Dirac delta - though we're counting energy, so return 1
    }    
    float3 wm = mfDistrib.Sample_wm(wo, u);
    VdotH = AbsDot(wo, wm);
    float3 wi = Reflect(wo, wm);    
    if (!SameHemisphere(wo, wi)){
        VdotH = 0.0f;
        return 0.0f; // Absorption
    }
    float pdf = mfDistrib.PDF(wo, wm) / (4 * AbsDot(wo, wm));
    float f = mfDistrib.D(wm)  * mfDistrib.G(wo, wi) / (4 * AbsCosTheta(wi) * AbsCosTheta(wo));
    return f * AbsCosTheta(wi) / pdf;
}

float sampleGGX_E_IOR(float rough, float cosTheta, float eta, float uc, float2 u){
    float alpha = Sqr(rough);
    TrowbridgeReitzDistribution mfDistrib = TrowbridgeReitzDistribution(alpha, alpha);
    float3 wo = float3(sqrt(1.0 - cosTheta * cosTheta), 0.0, cosTheta);
    if (mfDistrib.EffectivelySmooth() || abs(eta - 1.0f) <= 0.01f)
        return 1.0f; 
    float3 wm = mfDistrib.Sample_wm(wo, u);
    float R = FrDielectric(dot(wo, wm), eta), T = 1 - R;
    float pr = R, pt = T;
    float pSum = pr + pt;
    if (pSum <= EPS) return 0.0f;

    float3 wi;
    float pdf = 0.0f;
    float f = 0.0f;

    if (uc < pr / pSum) {
        wi = Reflect(wo, wm);
        if (!SameHemisphere(wo, wi)) return 0.0f;
        pdf = mfDistrib.PDF(wo, wm) / (4.0f * AbsDot(wo, wm)) * pr / pSum;
        f = R * mfDistrib.D(wm) * mfDistrib.G(wo, wi) /
            (4.0f * AbsCosTheta(wi) * AbsCosTheta(wo));
    } else {
        float etap;
        bool valid = Refract(wo, wm, eta, wi, etap);
        if (!valid || SameHemisphere(wo, wi) || wi.z == 0.0f) return 0.0f;
        float denom = Sqr(dot(wo, wm) + dot(wi, wm) * etap);
        if (denom < EPS) return 0.0f;
        pdf = mfDistrib.PDF(wo, wm) * AbsDot(wi, wm) * Sqr(etap) / denom * pt / pSum;
        f = T * mfDistrib.D(wm) * mfDistrib.G(wo, wi) *
            abs(dot(wi, wm) * dot(wo, wm) / (denom * AbsCosTheta(wi) * AbsCosTheta(wo)));
        f *= Sqr(etap);
    }

    if (pdf <= EPS) return 0.0f;
    return max(0.0f, f * AbsCosTheta(wi) / pdf);
}

[shader("compute")]
[numthreads(32,32,1)]
void integrateGGX_E(uint2 p : SV_DispatchThreadID)
{
    static const uint kSamples = 1 << 20;
    float E = 0.0f, Eprime = 0.0f, samples = 0.0f;
    float u = p.x / 31.0f, v = p.y / 31.0f;    
    PCG rng = PCG(uint4(p, 0, 0));
    for (uint i = 0; i < kSamples;i++) {
        float cosTheta = clamp(u, 1e-4, 1.0f);
        float rough = clamp(v, 1e-4, 1.0f);
        float3 wi;          
        float VdotH;
        float sample = sampleGGX_E(rough, cosTheta, rng.sample(), rng.sample2D(), VdotH);        
        float lambda = pow(1 - VdotH, 5);
        E += sample;
        Eprime += lambda * sample;
        samples += 1.0f;        
    }
    ggxE[dot(p, uint2(1, 32))] = float2(E / samples, Eprime / samples);
}

RWStructuredBuffer<float> ggxEavg;
[shader("compute")]
[numthreads(32,1,1)]
void integrateGGX_Eavg(uint2 p : SV_DispatchThreadID)
{
    static const uint kSamples = 1 << 16;
    float sum = 0.0f, samples = 0.0f;
    float u = p.x / 31.0f;
    PCG rng = PCG(uint4(p, 0, 0));
    for (uint i = 0; i < kSamples;i++) {        
        float cosTheta = rng.sample();
        float rough = clamp(u, 1e-4, 1.0f);  
        float VdotH;
        sum += 2 * cosTheta * sampleGGX_E(rough, cosTheta, rng.sample(), rng.sample2D(), VdotH);
        samples += 1.0f;        
    }
    ggxEavg[dot(p, uint2(1, 32))] = sum / samples;
}
// Cycle's parameterization
float IorFromZ(float z){
    return IorFromF0(Sqr(Sqr(z)));
}
RWStructuredBuffer<float> ggxEIOR;
uniform bool inverse;

[shader("compute")]
[numthreads(16,16,1)] // Dispatch with 1,1,16; D3D spec requires x*y*z <= 1024
void integrateGGX_IOR_E(uint3 p : SV_DispatchThreadID)
{
    static const uint kSamples = 1 << 23;
    float E = 0.0f, samples = 0.0f;
    float u = p.x / 15.0f, v = p.y / 15.0f, w = p.z / 15.0f;    
    PCG rng = PCG(uint4(p, 0));
    for (uint i = 0; i < kSamples;i++) {
        float cosTheta = clamp(u, 1e-4, 1.0f);
        float rough = clamp(v, 1e-4, 1.0f);
        float ior = IorFromZ(clamp(w, 1e-4, 0.99f));
        ior = inverse ? 1.0f / ior : ior;
        float sample = sampleGGX_E_IOR(rough, cosTheta, ior, rng.sample(), rng.sample2D());
        E += sample;
        samples += 1.0f;
    }
    ggxEIOR[dot(p, uint3(1, 16, 16 * 16))] = E / samples;
}

RWStructuredBuffer<float> ggxEIORavg;

[shader("compute")]
[numthreads(32,32,1)]
void integrateGGX_IOR_Eavg(uint2 p : SV_DispatchThreadID)
{
    static const uint kSamples = 1 << 16;
    float sum = 0.0f, samples = 0.0f;
    float u = p.x / 31.0f, v = p.y / 31.0f;
    PCG rng = PCG(uint4(p, 0, 0));
    for (uint i = 0; i < kSamples;i++) {
        float cosTheta = rng.sample();
        float rough = clamp(u, 1e-4, 1.0f);
        float ior = IorFromZ(clamp(v, 1e-4, 0.99f));
        ior = inverse ? 1.0f / ior : ior;
        sum += 2.0f * cosTheta * sampleGGX_E_IOR(rough, cosTheta, ior, rng.sample(), rng.sample2D());
        samples += 1.0f;
    }
    ggxEIORavg[dot(p, uint2(1, 32))] = sum / samples;
}
