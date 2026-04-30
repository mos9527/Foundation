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
public void CalcGlossyFresnel(float ior, float specularFactor, out float F0, out float F90) {
    ior = max(1.0f + 5e-3f, ior);
    specularFactor = clamp(specularFactor, 0.0f, 1.0f);
    F0 = clamp(F0FromIor(ior) * specularFactor, 0.0f, 1.0f);
    F90 = specularFactor;
}
// BSDF Layering function
public float layer(float base, float top, float topReflectance) {
    return top + base * (1.0f - topReflectance);
}
public float3 layer(float3 base, float3 top, float3 topReflectance) {
    return top + base * (1.0f - topReflectance);
}

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
    // wm = normalize(wo + eta * wi), not normalize(eta * wo + wi).
    float3 wm = normalize(wo + wi * etap);
    wm = FaceForward(wm, float3(0, 0, 1));
    float3 Fss = SchlickFresnel(F0, F90, AbsDot(wo, wm));
    float3 T = max(1.0f - Fss, 0.0f);
    float denom = Sqr(dot(wo, wm) + dot(wi, wm) * etap) * abs(CosTheta(wi) * CosTheta(wo));
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
    float3 wm = normalize(wo + wi * etap);
    wm = FaceForward(wm, float3(0, 0, 1));
    float denom = Sqr(dot(wo, wm) + dot(wi, wm) * etap);
    if (denom < EPS) return 0.0f;
    return mfDistrib.PDF(wo, wm) * AbsDot(wi, wm) * Sqr(etap) / denom;
}

public struct DiffuseBSDF : IBxDF {
    float3 baseColor;
    float  dielF0;
    float  dielF90;
    float  roughness;

    public __init(float3 baseColor, float dielF0, float dielF90, float roughness) {
        this.baseColor = baseColor;
        this.dielF0    = dielF0;
        this.dielF90   = dielF90;
        this.roughness = roughness;
    }

    public BxDFFlags Flags() { return BxDFFlags::DiffuseReflection; }

    public SampledSpectrum f(float3 wo, float3 wi, TransportMode mode) {
        if (!SameHemisphere(wo, wi)) return 0.0f;
        float2 lutE = SampleGGXLutE(AbsCosTheta(wo), roughness);
        float specularReflectance = reflectance(dielF0, dielF90, lutE);
        return baseColor * InvPi * max(1.0f - specularReflectance, 0.0f);
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

public struct GlassBSDF : IBxDF {
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
                val *= Sqr(etap);
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

public struct GlossyBSDF : IBxDF {
    float  glossyF0;
    float  glossyF90;
    float  roughness;
    bool   energyCompensation;
    TrowbridgeReitzDistribution mfDistrib;

    public __init(float glossyF0, float glossyF90, float roughness, bool energyCompensation) {
        this.glossyF0           = glossyF0;
        this.glossyF90          = glossyF90;
        this.roughness          = roughness;
        this.energyCompensation = energyCompensation;
        float alpha = roughness * roughness;
        this.mfDistrib = TrowbridgeReitzDistribution(alpha, alpha);
    }

    public BxDFFlags Flags() { return BxDFFlags::GlossyReflection; }

    public SampledSpectrum f(float3 wo, float3 wi, TransportMode mode) {
        if (!SameHemisphere(wo, wi)) return 0.0f;
        float2 lutE = SampleGGXLutE(AbsCosTheta(wo), roughness);
        return TorranceSparrowReflection(wo, wi, float3(glossyF0), float3(glossyF90), lutE, mfDistrib, energyCompensation);
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

public struct MetallicBSDF : IBxDF {
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

public enum AOVCategory {
    Undefined = -1,
    Diffuse  = 0,
    Specular = 1   // Glossy + Metallic + Transmission
};

public AOVCategory CategoryFromFlags(BxDFFlags f) {
    if (IsDiffuse(f)) return AOVCategory::Diffuse;
    return AOVCategory::Specular;
}

public float PrincipledBSSRDFBranchProbability(
    float metallic,
    float roughness,
    float ior,
    float specularFactor,
    float transmission,
    float subsurface,
    float3 wo)
{
    roughness = max(8e-3f, roughness);
    subsurface = clamp(subsurface, 0.0f, 1.0f);

    float dielF0, dielF90;
    CalcGlossyFresnel(ior, specularFactor, dielF0, dielF90);
    float sampleGlossy = reflectance(dielF0, dielF90, SampleGGXLutE(AbsCosTheta(wo), roughness));
    float rawDiffuseFamily = (1.0f - metallic) * (1.0f - sampleGlossy) * (1.0f - transmission);
    float rawMetal    = metallic;
    float rawDielSpec = (1.0f - metallic) * sampleGlossy;
    float rawTrans    = (1.0f - metallic) * (1.0f - sampleGlossy) * transmission;
    float rawSubsurface = rawDiffuseFamily * subsurface;
    float weightSum = rawDiffuseFamily + rawTrans + rawDielSpec + rawMetal;
    return weightSum > 0.0f ? rawSubsurface / weightSum : 0.0f;
}

public struct PrincipledBSDFLobes {
    // Lobe instances
    // NOTE: BSSRDF (subsurface) is not integrated via BSDF.
    //       It is sampled by the path integrator as a spatial event.
    //       Keep it out of the local BSDF mixture.
    DiffuseBSDF   diffuse;
    GlassBSDF     glass;
    GlossyBSDF    glossy;
    MetallicBSDF  metallic;

    // Normalized weights
    float wDiffuse;
    float wTrans;
    float wDielSpec;
    float wMetal;

    // Structural weights
    float cDiffuse;
    float cTrans;
    float cDielSpec;
    float cMetal;

    // Active flags
    bool hasDiffuse;
    bool hasTrans;
    bool hasDielSpec;
    bool hasMetal;

    float weightSum;

    public __init(float3 baseColor, float metallic, float roughness, float ior, float specularFactor = 1.0f,
                  float transmission = 0.0f, bool energyCompensation = true, float3 wo = float3(0,0,1),
                  float subsurface = 0.0f) {
        roughness = max(8e-3f, roughness);
        subsurface = clamp(subsurface, 0.0f, 1.0f);
        ior = max(1+5e-3f, ior);
        float dielF0, dielF90;
        CalcGlossyFresnel(ior, specularFactor, dielF0, dielF90);

        // View-dependent Fresnel weight
        float sampleGlossy = reflectance(dielF0, dielF90, SampleGGXLutE(AbsCosTheta(wo), roughness));

        // Unnormalized weights
        float rawDiffuseFamily = (1.0f - metallic) * (1.0f - sampleGlossy) * (1.0f - transmission);
        float rawMetal    = metallic;
        float rawDielSpec = (1.0f - metallic) * sampleGlossy;
        float rawTrans    = (1.0f - metallic) * (1.0f - sampleGlossy) * transmission;
        float rawDiffuse  = rawDiffuseFamily * (1.0f - subsurface);

        this.hasDiffuse  = rawDiffuse  > EPS;
        this.hasTrans    = rawTrans    > EPS;
        this.hasDielSpec = rawDielSpec > EPS;
        this.hasMetal    = rawMetal    > EPS;

        this.weightSum = rawDiffuse + rawTrans + rawDielSpec + rawMetal;
        if (this.weightSum <= 0.0f) this.weightSum = 1.0f;

        this.wDiffuse  = rawDiffuse  / this.weightSum;
        this.wTrans    = rawTrans    / this.weightSum;
        this.wDielSpec = rawDielSpec / this.weightSum;
        this.wMetal    = rawMetal    / this.weightSum;

        this.cDiffuse  = (1.0f - metallic) * (1.0f - transmission) * (1.0f - subsurface);
        this.cTrans    = (1.0f - metallic) * transmission;
        this.cDielSpec = 1.0f - metallic;
        this.cMetal    = metallic;

        // Construct each lobe
        this.diffuse  = DiffuseBSDF(baseColor, dielF0, dielF90, roughness);
        this.glass     = GlassBSDF(baseColor, ior, roughness, energyCompensation);
        this.glossy    = GlossyBSDF(dielF0, dielF90, roughness, energyCompensation);
        this.metallic  = MetallicBSDF(baseColor, roughness, energyCompensation);
    }

    // ----- Evaluate per-lobe f contribution split by AOV category -----
    // Returns separate f values for diffuse and specular (glossy+metal+glass)
    public void f_split(float3 wo, float3 wi, TransportMode mode,
                        out float3 fDiffuse, out float3 fSpecular) {
        fDiffuse  = float3(0);
        fSpecular = float3(0);
        if (hasDiffuse)  fDiffuse   = cDiffuse  * diffuse.f(wo, wi, mode);
        if (hasTrans)    fSpecular += cTrans    * glass.f(wo, wi, mode);
        if (hasDielSpec) fSpecular += cDielSpec * glossy.f(wo, wi, mode);
        if (hasMetal)    fSpecular += cMetal    * metallic.f(wo, wi, mode);
    }

    // Mixture f (sum over all lobes)
    public SampledSpectrum f(float3 wo, float3 wi, TransportMode mode) {
        float3 d, s;
        f_split(wo, wi, mode, d, s);
        return d + s;
    }

    // Mixture PDF
    public float PDF(float3 wo, float3 wi, TransportMode mode, BxDFReflTransFlags flags) {
        float result = 0.0f;
        if (hasDiffuse)  result += wDiffuse  * diffuse.PDF(wo, wi, mode, flags);
        if (hasTrans)    result += wTrans    * glass.PDF(wo, wi, mode, flags);
        if (hasDielSpec) result += wDielSpec * glossy.PDF(wo, wi, mode, flags);
        if (hasMetal)    result += wMetal    * metallic.PDF(wo, wi, mode, flags);
        return result;
    }

    // Stochastically select a lobe to sample; returns mixture f/pdf and the chosen AOV category
    public BSDFSample Sample_f(float3 wo, float uc, float2 u, TransportMode mode, BxDFReflTransFlags flags,
                               out AOVCategory category) {
        category = AOVCategory::Diffuse;
        // Categorical sampling
        float cumulative = 0.0f;
        IBxDF chosen = diffuse;
        bool found = false;

        if (hasDiffuse) {
            cumulative += wDiffuse;
            if (!found && uc < cumulative) {
                float lo = cumulative - wDiffuse;
                uc = (cumulative > lo) ? clamp((uc - lo) / (cumulative - lo), 0.0f, 1.0f) : 0.0f;
                chosen = diffuse;
                category = AOVCategory::Diffuse; found = true;
            }
        }
        if (hasTrans) {
            cumulative += wTrans;
            if (!found && uc < cumulative) {
                float lo = cumulative - wTrans;
                uc = (cumulative > lo) ? clamp((uc - lo) / (cumulative - lo), 0.0f, 1.0f) : 0.0f;
                chosen = glass;
                category = AOVCategory::Specular; found = true;
            }
        }
        if (hasDielSpec) {
            cumulative += wDielSpec;
            if (!found && uc < cumulative) {
                float lo = cumulative - wDielSpec;
                uc = (cumulative > lo) ? clamp((uc - lo) / (cumulative - lo), 0.0f, 1.0f) : 0.0f;
                chosen = glossy;
                category = AOVCategory::Specular; found = true;
            }
        }
        if (!found) {
            // Last lobe (metal) or fallback
            if (hasMetal) {
                chosen = metallic;
                category = AOVCategory::Specular; found = true;
            } else {
                return BSDFSample();
            }
        }

        BSDFSample bs = chosen.Sample_f(wo, uc, u, mode, flags);
        if (!bs.IsValid())
            return BSDFSample();

        // Mixture PDF and f ensure unbiased estimator
        float pMix = this.PDF(wo, bs.wi, mode, flags);
        if (pMix < 1e-7f)
            return BSDFSample();
        SampledSpectrum fMix = this.f(wo, bs.wi, mode);
        return BSDFSample(fMix, bs.wi, pMix, bs.flags, bs.eta);
    }
};