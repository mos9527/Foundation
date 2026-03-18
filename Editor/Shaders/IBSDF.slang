import IBxDF;
import IMath;

[[vk_binding(0,1)]] SamplerState lutSampler;
[[vk_binding(1,1)]] Texture2D<float2> ggxLutE;

public float2 SampleGGXLutE(float mu, float roughness) {
    return ggxLutE.SampleLevel(lutSampler, float2(mu, roughness), 0);
}
// -- OSL stdlib routines
import glsl;
// Directional Albedo
public float reflectance(float F0, float F90, float2 lutE) {
    return F0 * lutE.x + (F90 - F0) * lutE.y;
}
public float3 reflectance(float3 F0, float3 F90, float2 lutE) {
    return F0 * lutE.x + (F90 - F0) * lutE.y;
}
// BSDF Layering function
public float layer(float base, float top, float topReflectance) {
    return top + base * (1.0f - topReflectance);
}
public float3 layer(float3 base, float3 top, float3 topReflectance) {
    return top + base * (1.0f - topReflectance);
}

// ============================================================
// Shared microfacet helpers (used by multiple lobes)
// ============================================================

// Torrance-Sparrow reflection with Schlick Fresnel + energy compensation
float3 TorranceSparrowReflection(
    float3 wo, float3 wi,
    float3 F0, float3 F90,
    float2 lutE,
    TrowbridgeReitzDistribution mfDistrib,
    bool energyCompensation)
{
    float cosWo = AbsCosTheta(wo);
    float cosWi = AbsCosTheta(wi);
    if (cosWo < EPS || cosWi < EPS) return 0.0f;
    float3 wm = normalize(wo + wi);
    float3 Fss = SchlickFresnel(F0, F90, AbsDot(wo, wm));
    float3 Fms = 0.0f;
    if (energyCompensation) {
        float3 Epp = max(reflectance(F0, F90, lutE), EPS);
        Fms = F0 * (1.0f / Epp - 1.0f) * Fss;
    }
    return (Fss + Fms) * mfDistrib.D(wm) * mfDistrib.G(wo, wi) / (4.0f * cosWo * cosWi);
}

// Torrance-Sparrow transmittance with Schlick Fresnel (no energy compensation)
// TODO: Pre-integrate IOR multi-scatter
float3 TorranceSparrowTransmittance(
    float3 wo, float3 wi,
    float3 F0, float3 F90,
    float eta,
    TrowbridgeReitzDistribution mfDistrib)
{
    float etap = (CosTheta(wo) > 0) ? eta : 1.0f / eta;
    // Note: our Refract() uses eta = eta_t/eta_i. The corresponding transmission half-vector is
    // wm = normalize(eta * wo + wi), not normalize(wo + eta * wi).
    float3 wm = normalize(wo * etap + wi);
    wm = FaceForward(wm, float3(0, 0, 1));
    float3 Fss = SchlickFresnel(F0, F90, AbsDot(wo, wm));
    float3 T = max(1.0f - Fss, 0.0f);
    float denom = Sqr(dot(wi, wm) + dot(wo, wm) / etap) * abs(CosTheta(wi) * CosTheta(wo));
    if (denom < EPS) return 0.0f;
    return T * mfDistrib.D(wm) * mfDistrib.G(wo, wi) * abs(dot(wi, wm) * dot(wo, wm) / denom);
}

// Glossy reflection PDF (Torrance-Sparrow Jacobian)
float PDFGlossy(float3 wo, float3 wi, TrowbridgeReitzDistribution mfDistrib) {
    float3 wm = normalize(wo + wi);
    float denom = 4.0f * AbsDot(wo, wm);
    if (denom < EPS) return 0.0f;
    return mfDistrib.PDF(wo, wm) / denom;
}

// Transmittance PDF (rough dielectric Jacobian)
float PDFTransmittance(float3 wo, float3 wi, float eta, TrowbridgeReitzDistribution mfDistrib) {
    float etap = (CosTheta(wo) > 0) ? eta : 1.0f / eta;
    // Must match Refract()'s eta convention (eta_t/eta_i)
    float3 wm = normalize(wo * etap + wi);
    wm = FaceForward(wm, float3(0, 0, 1));
    float denom = Sqr(dot(wi, wm) + dot(wo, wm) / etap);
    if (denom < EPS) return 0.0f;
    return mfDistrib.PDF(wo, wm) * AbsDot(wi, wm) / denom;
}

// ============================================================
// Lobe 1: Principled Diffuse
// Lambertian reflection, attenuated by dielectric Fresnel energy
// ============================================================
public struct PrincipledDiffuseLobe : IBxDF {
    float3 baseColor;
    float  dielF0;
    float  roughness;

    public __init(float3 baseColor, float dielF0, float roughness) {
        this.baseColor = baseColor;
        this.dielF0    = dielF0;
        this.roughness = roughness;
    }

    public BxDFFlags Flags() { return BxDFFlags::DiffuseReflection; }

    public SampledSpectrum f(float3 wo, float3 wi, TransportMode mode) {
        if (!SameHemisphere(wo, wi)) return 0.0f;
        float2 lutE = SampleGGXLutE(AbsCosTheta(wo), roughness);
        return baseColor * InvPi * (1.0f - reflectance(dielF0, 1.0f, lutE));
    }

    public BSDFSample Sample_f(float3 wo, float uc, float2 u, TransportMode mode, BxDFReflTransFlags flags) {
        if (!(flags & BxDFReflTransFlags::Reflection)) return BSDFSample();
        float3 wi = SampleCosineHemisphere(u);
        wi = FaceForward(wi, float3(0, 0, 1));
        float pdf = CosineHemispherePDF(AbsCosTheta(wi));
        return BSDFSample(this.f(wo, wi, mode), wi, pdf, BxDFFlags::DiffuseReflection);
    }

    public float PDF(float3 wo, float3 wi, TransportMode mode, BxDFReflTransFlags flags) {
        if (!(flags & BxDFReflTransFlags::Reflection) || !SameHemisphere(wo, wi)) return 0.0f;
        return CosineHemispherePDF(AbsCosTheta(wi));
    }
};

// ============================================================
// Lobe 2: Principled Transmittance
// Rough dielectric BTDF; handles TIR by falling back to reflection
// ============================================================
public struct PrincipledTransmittanceLobe : IBxDF {
    float3 baseColor;
    float  eta;
    float  roughness;
    bool   energyCompensation;
    TrowbridgeReitzDistribution mfDistrib;

    public __init(float3 baseColor, float eta, float roughness, bool energyCompensation) {
        this.baseColor          = baseColor;
        this.eta                = eta;
        this.roughness          = roughness;
        this.energyCompensation = energyCompensation;
        float alpha = roughness * roughness;
        this.mfDistrib = TrowbridgeReitzDistribution(alpha, alpha);
    }

    public BxDFFlags Flags() { return BxDFFlags::GlossyReflection | BxDFFlags::GlossyTransmission; }

    public SampledSpectrum f(float3 wo, float3 wi, TransportMode mode) {
        bool reflected = SameHemisphere(wo, wi);
        float etap = (CosTheta(wo) > 0) ? eta : 1.0f / eta;
        float3 transmittanceF0 = float3(F0FromIor(etap));
        if (reflected) {
            // TIR contribution
            if (!IsTIR(wo, wi, eta)) return 0.0f;
            float cosWo = AbsCosTheta(wo), cosWi = AbsCosTheta(wi);
            if (cosWo < EPS || cosWi < EPS) return 0.0f;
            float3 wm = normalize(wo + wi);
            return baseColor * mfDistrib.D(wm) * mfDistrib.G(wo, wi) / (4.0f * cosWo * cosWi);
        } else {
            float3 val = baseColor * TorranceSparrowTransmittance(wo, wi, transmittanceF0, float3(1.0f), eta, mfDistrib);
            if (mode == TransportMode::Radiance)
                val /= Sqr(etap);
            return val;
        }
    }

    public BSDFSample Sample_f(float3 wo, float uc, float2 u, TransportMode mode, BxDFReflTransFlags flags) {
        float3 wm = mfDistrib.Sample_wm(wo, u);
        float etap;
        float3 wi;
        bool tir = !Refract(wo, wm, eta, wi, etap);
        if (tir) {
            // TIR: fall back to reflection
            wi = Reflect(wo, wm);
            return BSDFSample(this.f(wo, wi, mode), wi, PDFGlossy(wo, wi, mfDistrib), BxDFFlags::GlossyReflection);
        }
        if (SameHemisphere(wo, wi) || wi.z == 0) return BSDFSample();
        float pdf = PDFTransmittance(wo, wi, eta, mfDistrib);
        return BSDFSample(this.f(wo, wi, mode), wi, pdf, BxDFFlags::GlossyTransmission, etap);
    }

    public float PDF(float3 wo, float3 wi, TransportMode mode, BxDFReflTransFlags flags) {
        bool reflected = SameHemisphere(wo, wi);
        if (reflected) {
            // TIR reflection PDF
            if (!IsTIR(wo, wi, eta)) return 0.0f;
            return PDFGlossy(wo, wi, mfDistrib);
        }
        return PDFTransmittance(wo, wi, eta, mfDistrib);
    }
};

// ============================================================
// Lobe 3: Principled Dielectric Specular
// Torrance-Sparrow reflection with dielectric Fresnel
// ============================================================
public struct PrincipledDielectricLobe : IBxDF {
    float  dielF0;
    float  roughness;
    bool   energyCompensation;
    TrowbridgeReitzDistribution mfDistrib;

    public __init(float dielF0, float roughness, bool energyCompensation) {
        this.dielF0             = dielF0;
        this.roughness          = roughness;
        this.energyCompensation = energyCompensation;
        float alpha = roughness * roughness;
        this.mfDistrib = TrowbridgeReitzDistribution(alpha, alpha);
    }

    public BxDFFlags Flags() { return BxDFFlags::GlossyReflection; }

    public SampledSpectrum f(float3 wo, float3 wi, TransportMode mode) {
        if (!SameHemisphere(wo, wi)) return 0.0f;
        float2 lutE = SampleGGXLutE(AbsCosTheta(wo), roughness);
        return TorranceSparrowReflection(wo, wi, float3(dielF0), float3(1.0f), lutE, mfDistrib, energyCompensation);
    }

    public BSDFSample Sample_f(float3 wo, float uc, float2 u, TransportMode mode, BxDFReflTransFlags flags) {
        if (!(flags & BxDFReflTransFlags::Reflection)) return BSDFSample();
        float3 wm = mfDistrib.Sample_wm(wo, u);
        float3 wi = Reflect(wo, wm);
        if (!SameHemisphere(wo, wi)) return BSDFSample();
        float pdf = PDFGlossy(wo, wi, mfDistrib);
        if (pdf < 1e-7f) return BSDFSample();
        return BSDFSample(this.f(wo, wi, mode), wi, pdf, BxDFFlags::GlossyReflection);
    }

    public float PDF(float3 wo, float3 wi, TransportMode mode, BxDFReflTransFlags flags) {
        if (!(flags & BxDFReflTransFlags::Reflection) || !SameHemisphere(wo, wi)) return 0.0f;
        return PDFGlossy(wo, wi, mfDistrib);
    }
};

// ============================================================
// Lobe 4: Principled Metal
// Torrance-Sparrow reflection with conductor Fresnel (baseColor as F0)
// ============================================================
public struct PrincipledMetalLobe : IBxDF {
    float3 metalF0;
    float  roughness;
    bool   energyCompensation;
    TrowbridgeReitzDistribution mfDistrib;

    public __init(float3 metalF0, float roughness, bool energyCompensation) {
        this.metalF0            = metalF0;
        this.roughness          = roughness;
        this.energyCompensation = energyCompensation;
        float alpha = roughness * roughness;
        this.mfDistrib = TrowbridgeReitzDistribution(alpha, alpha);
    }

    public BxDFFlags Flags() { return BxDFFlags::GlossyReflection; }

    public SampledSpectrum f(float3 wo, float3 wi, TransportMode mode) {
        if (!SameHemisphere(wo, wi)) return 0.0f;
        float2 lutE = SampleGGXLutE(AbsCosTheta(wo), roughness);
        return TorranceSparrowReflection(wo, wi, metalF0, float3(1.0f), lutE, mfDistrib, energyCompensation);
    }

    public BSDFSample Sample_f(float3 wo, float uc, float2 u, TransportMode mode, BxDFReflTransFlags flags) {
        if (!(flags & BxDFReflTransFlags::Reflection)) return BSDFSample();
        float3 wm = mfDistrib.Sample_wm(wo, u);
        float3 wi = Reflect(wo, wm);
        if (!SameHemisphere(wo, wi)) return BSDFSample();
        float pdf = PDFGlossy(wo, wi, mfDistrib);
        if (pdf < 1e-7f) return BSDFSample();
        return BSDFSample(this.f(wo, wi, mode), wi, pdf, BxDFFlags::GlossyReflection);
    }

    public float PDF(float3 wo, float3 wi, TransportMode mode, BxDFReflTransFlags flags) {
        if (!(flags & BxDFReflTransFlags::Reflection) || !SameHemisphere(wo, wi)) return 0.0f;
        return PDFGlossy(wo, wi, mfDistrib);
    }
};

// ============================================================
// PrincipledBSDF — LobeMixture factory
// Layered model: [ [Diffuse (mix) Transmittance] (layer) Dielectric Specular ] (mix) Metal
// Lobe weights mirror the old Select() probabilities, now expressed as
// normalized mixture weights for unbiased stochastic sampling.
// ============================================================
public struct PrincipledBSDF : IBxDF {
    LobeMixture mixture;

    public __init(float3 baseColor, float metallic, float roughness, float ior, float transmission = 0.0f, bool energyCompensation = true) {
        ior       = max(1.0f + 5e-3f, ior);
        roughness = max(5e-3f, roughness);

        float dielF0 = F0FromIor(ior);
        // sampleGlossy: energy-weighted probability of sampling the dielectric specular lobe
        float sampleGlossy = reflectance(dielF0, 1.0f, SampleGGXLutE(0.5f, roughness));

        // Lobe weights (unnormalized, matching old Select() branching probabilities):
        //   metal  lobe weight = metallic
        //   dielectric specular weight = (1 - metallic) * sampleGlossy
        //   transmittance weight = (1 - metallic) * (1 - sampleGlossy) * transmission
        //   diffuse weight = (1 - metallic) * (1 - sampleGlossy) * (1 - transmission)
        float wMetal    = metallic;
        float wDielSpec = (1.0f - metallic) * sampleGlossy;
        float wTrans    = (1.0f - metallic) * (1.0f - sampleGlossy) * transmission;
        float wDiffuse  = (1.0f - metallic) * (1.0f - sampleGlossy) * (1.0f - transmission);

        mixture = LobeMixture();
        if (wDiffuse  > EPS) mixture.AddLobe(PrincipledDiffuseLobe(baseColor, dielF0, roughness),                    wDiffuse);
        if (wTrans    > EPS) mixture.AddLobe(PrincipledTransmittanceLobe(baseColor, ior, roughness, energyCompensation), wTrans);
        if (wDielSpec > EPS) mixture.AddLobe(PrincipledDielectricLobe(dielF0, roughness, energyCompensation),        wDielSpec);
        if (wMetal    > EPS) mixture.AddLobe(PrincipledMetalLobe(baseColor, roughness, energyCompensation),          wMetal);
        mixture.Finalize();
    }

    public BxDFFlags Flags() { return mixture.Flags(); }

    public SampledSpectrum f(float3 wo, float3 wi, TransportMode mode) {
        return mixture.f(wo, wi, mode);
    }

    public float PDF(float3 wo, float3 wi, TransportMode mode, BxDFReflTransFlags flags) {
        return mixture.PDF(wo, wi, mode, flags);
    }

    public BSDFSample Sample_f(float3 wo, float uc, float2 u, TransportMode mode, BxDFReflTransFlags flags) {
        return mixture.Sample_f(wo, uc, u, mode, flags);
    }
};