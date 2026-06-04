import IBxDF;
import IBindless2D;
import IBindless3D;
import IMath;

SamplerState lutSampler;

public float2 SampleGGXLutE(uint32_t ggxLutEIndex, float mu, float roughness) {
    return textures[NonUniformResourceIndex(ggxLutEIndex)].SampleLevel(lutSampler, float2(mu, roughness), 0).xy;
}
public float SampleGGXLutEavg(uint32_t ggxLutEavgIndex, float roughness) {
    return textures[NonUniformResourceIndex(ggxLutEavgIndex)].SampleLevel(lutSampler, float2(roughness, 0.5f), 0).x;
}
public float SampleGGXLutEIOR(uint32_t ggxLutEIORIndex, uint32_t ggxLutEIORInvIndex, float mu, float roughness, float ior) {
    uint32_t lut = ior > 1.0f ? ggxLutEIORIndex : ggxLutEIORInvIndex;
    float z = ZFromIor(ior > 1.0f ? ior : 1.0f / ior);
    return textures3D[NonUniformResourceIndex(lut)].SampleLevel(lutSampler, float3(mu, roughness, z), 0).x;
}
public float SampleGGXLutEIORavg(uint32_t ggxLutEIORavgIndex, uint32_t ggxLutEIORInvavgIndex, float roughness, float ior) {
    uint32_t lut = ior > 1.0f ? ggxLutEIORavgIndex : ggxLutEIORInvavgIndex;
    float z = ZFromIor(ior > 1.0f ? ior : 1.0f / ior);
    return textures[NonUniformResourceIndex(lut)].SampleLevel(lutSampler, float2(roughness, z), 0).x;
}
public float4 SampleSheenLTC(uint32_t sheenLtcIndex, float mu, float roughness) {
    if (sheenLtcIndex == ~0u) return float4(0.0f);
    return textures[NonUniformResourceIndex(sheenLtcIndex)].SampleLevel(lutSampler, float2(mu, roughness), 0);
}
// -- OSL stdlib routines
import glsl;
// Directional Albedo for reflectance
public float reflectance(float F0, float F90, float2 lutE) {
    return F0 * lutE.x + (F90 - F0) * lutE.y;
}
public float3 reflectance(float3 F0, float3 F90, float2 lutE) {
    return F0 * lutE.x + (F90 - F0) * lutE.y;
}
public void CalcGlossyFresnel(float ior, float specularFactor, float3 specularColorFactor, out float3 F0, out float3 F90) {
    ior = max(1.0f, ior);
    specularFactor = clamp(specularFactor, 0.0f, 1.0f);
    if (abs(ior - 1.0f) <= 0.01f || specularFactor <= EPS) {
        F0 = float3(0.0f);
        F90 = float3(0.0f);
        return;
    }
    F0 = clamp(F0FromIor(ior) * max(specularColorFactor, float3(0.0f)), float3(0.0f), float3(1.0f)) * specularFactor;
    F90 = float3(specularFactor);
}

public void CalcCoatFresnel(out float3 F0, out float3 F90) {
    F0 = float3(F0FromIor(1.5f));
    F90 = float3(1.0f);
}

public float2 GGXDirectionalAlbedoLut(uint32_t ggxLutEIndex, float3 wo, float roughness, TrowbridgeReitzDistribution mfDistrib) {
    if (mfDistrib.EffectivelySmooth()) {
        float mu = AbsCosTheta(wo);
        return float2(1.0f, pow(1.0f - mu, 5.0f));
    }
    return SampleGGXLutE(ggxLutEIndex, AbsCosTheta(wo), roughness);
}

public float GGXAverageAlbedo(uint32_t ggxLutEavgIndex, float roughness, TrowbridgeReitzDistribution mfDistrib) {
    if (mfDistrib.EffectivelySmooth()) return 1.0f;
    return SampleGGXLutEavg(ggxLutEavgIndex, roughness);
}

public float3 GlossyEnergyCompensationScale(
    float3 F0,
    float3 F90,
    float2 lutE,
    float lutEavg,
    bool energyCompensation)
{
    return MicrofacetGGXEnergyCompensationScale(
        SchlickFresnelAverage(F0, F90), lutE.x, lutEavg, energyCompensation);
}

public float3 GlossyDirectionalReflectance(
    float3 F0,
    float3 F90,
    float2 lutE,
    float lutEavg,
    bool energyCompensation)
{
    return reflectance(F0, F90, lutE) *
        GlossyEnergyCompensationScale(F0, F90, lutE, lutEavg, energyCompensation);
}

public float3 CoatReflectance(float coatWeight, float coatRoughness, float3 wo, uint32_t ggxLutEIndex, uint32_t ggxLutEavgIndex, bool energyCompensation = true) {
    coatWeight = saturate(coatWeight);
    if (coatWeight <= EPS) return float3(0.0f);
    coatRoughness = max(8e-3f, coatRoughness);

    float3 coatF0, coatF90;
    CalcCoatFresnel(coatF0, coatF90);
    float alphaX, alphaY;
    CalcAnisotropicAlpha(coatRoughness, 0.0f, alphaX, alphaY);
    TrowbridgeReitzDistribution mfDistrib = TrowbridgeReitzDistribution(alphaX, alphaY);
    float2 lutE = GGXDirectionalAlbedoLut(ggxLutEIndex, wo, coatRoughness, mfDistrib);
    float lutEavg = GGXAverageAlbedo(ggxLutEavgIndex, coatRoughness, mfDistrib);
    return coatWeight * GlossyDirectionalReflectance(coatF0, coatF90, lutE, lutEavg, energyCompensation);
}

public float CoatEscape(float coatWeight, float coatRoughness, float3 wo, uint32_t ggxLutEIndex, uint32_t ggxLutEavgIndex, bool energyCompensation = true) {
    return max(1.0f - MaxComponent(CoatReflectance(coatWeight, coatRoughness, wo, ggxLutEIndex, ggxLutEavgIndex, energyCompensation)), 0.0f);
}

public void CalcAnisotropicAlpha(float roughness, float anisotropyStrength, out float alphaX, out float alphaY) {
    roughness = max(8e-3f, roughness);
    anisotropyStrength = clamp(anisotropyStrength, 0.0f, 1.0f);

    float alpha = roughness * roughness;
    alphaX = lerp(alpha, 1.0f, anisotropyStrength * anisotropyStrength);
    alphaY = alpha;
}
// BSDF Layering function
// https://academysoftwarefoundation.github.io/OpenPBR/#formalism/layering
public float layer(float base, float top, float topReflectance) {
    return top + base * (1.0f - topReflectance);
}
public float3 layer(float3 base, float3 top, float3 topReflectance) {
    return top + base * (1.0f - topReflectance);
}
// mix is simply mix/lerp

// Favg
// https://mos9527.com/posts/foundation/pt-6-path-tracing-adventures/#%E5%B9%B3%E5%9D%87%E5%8F%8D%E5%B0%84%E7%8E%87-f_avg
float3 SchlickFresnelAverage(float3 F0, float3 F90)
{
    return lerp(F0, F90, 1.0f / 21.0f);
}

// Fms from Revisiting Physically Based Shading at Imageworks.
// Fss is the hemispherical-average Fresnel term used for multiscatter color compensation.
float3 MicrofacetGGXEnergyCompensationScale(
    float3 Favg,
    float E,
    float Eavg,
    bool energyCompensation)
{
    if (!energyCompensation) return float3(1.0f);

    E = clamp(E, EPS, 1.0f);
    Eavg = clamp(Eavg, EPS, 1.0f);
    float3 Fms = (Favg * Eavg) /
        max(1.0f - Favg * (1.0f - Eavg), EPS);
    return 1.0f + Fms * (1.0f / E - 1.0f);
}

float3 MicrofacetGGXDielectricEnergyCompensationScale(
    float3 Fss,
    float E,
    float Eavg,
    bool energyCompensation)
{
    if (!energyCompensation) return float3(1.0f);

    E = clamp(E, EPS, 1.0f);
    Eavg = clamp(Eavg, EPS, 1.0f);
    Fss = saturate(Fss);
    float3 Fms = (Fss * Eavg) /
        max(1.0f - Fss * (1.0f - Eavg), EPS);
    return 1.0f + Fms * (1.0f / E - 1.0f);
}

// Torrance-Sparrow reflection with Schlick Fresnel + energy compensation
float3 TorranceSparrowReflection(
    float3 wo, float3 wi,
    float3 F0, float3 F90,
    float2 lutE,
    float lutEavg,
    TrowbridgeReitzDistribution mfDistrib,
    bool energyCompensation)
{
    float cosWo = AbsCosTheta(wo);
    float cosWi = AbsCosTheta(wi);
    if (cosWo < EPS || cosWi < EPS) return 0.0f;
    float3 wm = normalize(wo + wi);
    float3 Fss = SchlickFresnel(F0, F90, AbsDot(wo, wm));
    Fss *= GlossyEnergyCompensationScale(F0, F90, lutE, lutEavg, energyCompensation);
    return Fss * mfDistrib.D(wm) * mfDistrib.G(wo, wi) / (4.0f * cosWo * cosWi);
}

// Glossy reflection PDF (Torrance-Sparrow Jacobian)
public float PDFGlossy(float3 wo, float3 wi, TrowbridgeReitzDistribution mfDistrib) {
    float3 wm = normalize(wo + wi);
    float denom = 4.0f * AbsDot(wo, wm);
    if (denom < EPS) return 0.0f;
    return mfDistrib.PDF(wo, wm) / denom;
}

// Transmittance PDF (rough dielectric Jacobian)
public float PDFTransmittance(float3 wo, float3 wi, float eta, TrowbridgeReitzDistribution mfDistrib) {
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
    float3 dielF0;
    float3 dielF90;
    float  roughness;
    float  anisotropyStrength;
    bool   energyCompensation;
    TrowbridgeReitzDistribution mfDistrib;
    uint32_t ggxLutEIndex;
    uint32_t ggxLutEavgIndex;

    public __init(float3 baseColor, float3 dielF0, float3 dielF90, float roughness, float anisotropyStrength, bool energyCompensation, uint32_t ggxLutEIndex, uint32_t ggxLutEavgIndex) {
        this.baseColor = baseColor;
        this.dielF0    = dielF0;
        this.dielF90   = dielF90;
        this.roughness = roughness;
        this.anisotropyStrength = anisotropyStrength;
        this.energyCompensation = energyCompensation;
        this.ggxLutEIndex = ggxLutEIndex;
        this.ggxLutEavgIndex = ggxLutEavgIndex;
        float alphaX, alphaY;
        CalcAnisotropicAlpha(roughness, anisotropyStrength, alphaX, alphaY);
        this.mfDistrib = TrowbridgeReitzDistribution(alphaX, alphaY);
    }

    public BxDFFlags Flags() { return BxDFFlags::DiffuseReflection; }

    public SampledSpectrum f(float3 wo, float3 wi, TransportMode mode) {
        if (!SameHemisphere(wo, wi)) return 0.0f;
        float2 lutE = GGXDirectionalAlbedoLut(ggxLutEIndex, wo, roughness, mfDistrib);
        float lutEavg = GGXAverageAlbedo(ggxLutEavgIndex, roughness, mfDistrib);
        float3 specularReflectance = GlossyDirectionalReflectance(dielF0, dielF90, lutE, lutEavg, energyCompensation);
        return baseColor * InvPi * max(1.0f - MaxComponent(specularReflectance), 0.0f);
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
    float3 dielectricF0;
    float3 dielectricF90;
    float  roughness;
    float  anisotropyStrength;
    bool   energyCompensation;
    TrowbridgeReitzDistribution mfDistrib;
    uint32_t ggxLutEIndex;
    uint32_t ggxLutEavgIndex;
    uint32_t ggxLutEIORIndex;
    uint32_t ggxLutEIORavgIndex;
    uint32_t ggxLutEIORInvIndex;
    uint32_t ggxLutEIORInvavgIndex;

    public __init(float3 baseColor, float eta, float3 transmittanceF0, float3 transmittanceF90,
                  float roughness, float anisotropyStrength, bool energyCompensation, uint32_t ggxLutEIndex, uint32_t ggxLutEavgIndex, uint32_t ggxLutEIORIndex, uint32_t ggxLutEIORavgIndex, uint32_t ggxLutEIORInvIndex, uint32_t ggxLutEIORInvavgIndex) {
        this.baseColor          = baseColor;
        this.eta                = eta;
        this.dielectricF0       = transmittanceF0;
        this.dielectricF90      = transmittanceF90;
        this.roughness          = roughness;
        this.anisotropyStrength = anisotropyStrength;
        this.energyCompensation = energyCompensation;
        this.ggxLutEIndex       = ggxLutEIndex;
        this.ggxLutEavgIndex    = ggxLutEavgIndex;
        this.ggxLutEIORIndex    = ggxLutEIORIndex;
        this.ggxLutEIORavgIndex = ggxLutEIORavgIndex;
        this.ggxLutEIORInvIndex = ggxLutEIORInvIndex;
        this.ggxLutEIORInvavgIndex = ggxLutEIORInvavgIndex;
        float alphaX, alphaY;
        CalcAnisotropicAlpha(roughness, anisotropyStrength, alphaX, alphaY);
        this.mfDistrib = TrowbridgeReitzDistribution(alphaX, alphaY);
    }

    float RelativeEta(float3 wo) {
        return (CosTheta(wo) > 0.0f) ? eta : 1.0f / eta;
    }

    bool UseSpecularPath(float3 wo) {
        return mfDistrib.EffectivelySmooth() || abs(RelativeEta(wo) - 1.0f) <= 0.01f;
    }

    bool IsSpecularDirection(float3 a, float3 b) {
        return LengthSquared(a - b) <= 1e-6f;
    }

    float3 SpecularF(float3 wo, float3 wi, TransportMode mode) {
        float R = FrDielectric(CosTheta(wo), eta);
        float T = max(1.0f - R, 0.0f);

        if (SameHemisphere(wo, wi)) {
            float3 wr = float3(-wo.x, -wo.y, wo.z);
            float cosWi = AbsCosTheta(wi);
            if (!IsSpecularDirection(wi, wr) || cosWi < EPS) return float3(0.0f);
            return float3(R / cosWi);
        }

        float3 wt;
        float etap;
        if (!Refract(wo, float3(0.0f, 0.0f, 1.0f), eta, wt, etap)) return float3(0.0f);
        float cosWi = AbsCosTheta(wi);
        if (!IsSpecularDirection(wi, wt) || cosWi < EPS) return float3(0.0f);
        float3 ft = baseColor * (T / cosWi);
        if (mode == TransportMode::Radiance)
            ft *= Sqr(etap);
        return ft;
    }

    float SpecularPDF(float3 wo, float3 wi, BxDFReflTransFlags flags) {
        bool allowReflection = (flags & BxDFReflTransFlags::Reflection);
        bool allowTransmission = (flags & BxDFReflTransFlags::Transmission);
        float R = FrDielectric(CosTheta(wo), eta);
        float T = max(1.0f - R, 0.0f);
        float pr = allowReflection ? R : 0.0f;
        float pt = allowTransmission ? T : 0.0f;
        float pSum = pr + pt;
        if (pSum <= EPS) return 0.0f;

        if (SameHemisphere(wo, wi)) {
            float3 wr = float3(-wo.x, -wo.y, wo.z);
            return allowReflection && IsSpecularDirection(wi, wr) ? pr / pSum : 0.0f;
        }

        float3 wt;
        float etap;
        if (!Refract(wo, float3(0.0f, 0.0f, 1.0f), eta, wt, etap)) return 0.0f;
        return allowTransmission && IsSpecularDirection(wi, wt) ? pt / pSum : 0.0f;
    }

    float GlassDirectionalAlbedo(float3 wo) {
        float etap = RelativeEta(wo);
        if (abs(etap - 1.0f) <= 0.01f) return 1.0f;
        return SampleGGXLutEIOR(ggxLutEIORIndex, ggxLutEIORInvIndex, AbsCosTheta(wo), roughness, etap);
    }

    float GlassAverageAlbedo(float3 wo) {
        float etap = RelativeEta(wo);
        if (abs(etap - 1.0f) <= 0.01f) return 1.0f;
        return SampleGGXLutEIORavg(ggxLutEIORavgIndex, ggxLutEIORInvavgIndex, roughness, etap);
    }

    public BxDFFlags Flags() { return BxDFFlags::GlossyReflection | BxDFFlags::GlossyTransmission; }

    public SampledSpectrum f(float3 wo, float3 wi, TransportMode mode) {
        float cosWo = CosTheta(wo);
        float cosWi = CosTheta(wi);
        if (cosWo == 0.0f || cosWi == 0.0f) return 0.0f;
        if (UseSpecularPath(wo)) return SpecularF(wo, wi, mode);

        bool reflected = SameHemisphere(wo, wi);
        float etap = reflected ? 1.0f : RelativeEta(wo);
        float3 wmUnnormalized = reflected ? wo + wi : wo + wi * etap;
        if (LengthSquared(wmUnnormalized) == 0.0f) return 0.0f;
        float3 wm = FaceForward(normalize(wmUnnormalized), float3(0.0f, 0.0f, 1.0f));

        if (dot(wm, wi) * cosWi < 0.0f || dot(wm, wo) * cosWo < 0.0f) return 0.0f;

        float F = FrDielectric(dot(wo, wm), eta);
        float3 scale = MicrofacetGGXDielectricEnergyCompensationScale(baseColor, GlassDirectionalAlbedo(wo), GlassAverageAlbedo(wo), energyCompensation);

        if (reflected) {
            float cosWoAbs = AbsCosTheta(wo);
            float cosWiAbs = AbsCosTheta(wi);
            if (cosWoAbs < EPS || cosWiAbs < EPS) return 0.0f;
            return float3(F) * scale * mfDistrib.D(wm) * mfDistrib.G(wo, wi) /
                (4.0f * cosWoAbs * cosWiAbs);
        }

        float T = max(1.0f - F, 0.0f);
        float denom = Sqr(dot(wo, wm) + dot(wi, wm) * etap) * abs(cosWi * cosWo);
        if (denom < EPS) return 0.0f;

        float3 ft = baseColor * T * scale * mfDistrib.D(wm) * mfDistrib.G(wo, wi) *
            abs(dot(wi, wm) * dot(wo, wm) / denom);
        if (mode == TransportMode::Radiance)
            ft *= Sqr(etap);
        return ft;
    }

    public BSDFSample Sample_f(float3 wo, float uc, float2 u, TransportMode mode, BxDFReflTransFlags flags) {
        bool allowReflection = (flags & BxDFReflTransFlags::Reflection);
        bool allowTransmission = (flags & BxDFReflTransFlags::Transmission);
        if (!allowReflection && !allowTransmission) return BSDFSample();

        if (UseSpecularPath(wo)) {
            float R = FrDielectric(CosTheta(wo), eta);
            float T = max(1.0f - R, 0.0f);
            float pr = allowReflection ? R : 0.0f;
            float pt = allowTransmission ? T : 0.0f;
            float pSum = pr + pt;
            if (pSum <= EPS) return BSDFSample();

            if (uc < pr / pSum) {
                float3 wi = float3(-wo.x, -wo.y, wo.z);
                float cosWi = AbsCosTheta(wi);
                if (cosWi < EPS) return BSDFSample();
                return BSDFSample(float3(R / cosWi), wi, pr / pSum, BxDFFlags::SpecularReflection);
            }

            float3 wi;
            float etap;
            if (!Refract(wo, float3(0.0f, 0.0f, 1.0f), eta, wi, etap)) return BSDFSample();
            float cosWi = AbsCosTheta(wi);
            if (cosWi < EPS) return BSDFSample();
            float3 ft = baseColor * (T / cosWi);
            if (mode == TransportMode::Radiance)
                ft *= Sqr(etap);
            return BSDFSample(ft, wi, pt / pSum, BxDFFlags::SpecularTransmission, etap);
        }

        float3 wm = mfDistrib.Sample_wm(wo, u);
        float F = FrDielectric(dot(wo, wm), eta);
        float R = F;
        float T = max(1.0f - F, 0.0f);
        float pr = allowReflection ? R : 0.0f;
        float pt = allowTransmission ? T : 0.0f;
        float pSum = pr + pt;
        if (pSum <= EPS) return BSDFSample();

        if (uc < pr / pSum) {
            float3 wi = Reflect(wo, wm);
            if (!SameHemisphere(wo, wi)) return BSDFSample();
            float pdf = PDFGlossy(wo, wi, mfDistrib) * pr / pSum;
            if (pdf < 1e-7f) return BSDFSample();
            return BSDFSample(this.f(wo, wi, mode), wi, pdf, BxDFFlags::GlossyReflection);
        }

        float etap;
        float3 wi;
        bool tir = !Refract(wo, wm, eta, wi, etap);
        if (tir || SameHemisphere(wo, wi) || wi.z == 0) return BSDFSample();
        float pdf = PDFTransmittance(wo, wi, eta, mfDistrib) * pt / pSum;
        if (pdf < 1e-7f) return BSDFSample();
        return BSDFSample(this.f(wo, wi, mode), wi, pdf, BxDFFlags::GlossyTransmission, etap);
    }

    public float PDF(float3 wo, float3 wi, TransportMode mode, BxDFReflTransFlags flags) {
        bool allowReflection = (flags & BxDFReflTransFlags::Reflection);
        bool allowTransmission = (flags & BxDFReflTransFlags::Transmission);
        if (!allowReflection && !allowTransmission) return 0.0f;
        if (UseSpecularPath(wo)) return SpecularPDF(wo, wi, flags);

        bool reflected = SameHemisphere(wo, wi);
        float etap = reflected ? 1.0f : RelativeEta(wo);
        float3 wmUnnormalized = reflected ? wo + wi : wo + wi * etap;
        if (LengthSquared(wmUnnormalized) == 0.0f) return 0.0f;
        float3 wm = FaceForward(normalize(wmUnnormalized), float3(0.0f, 0.0f, 1.0f));

        if (dot(wm, wi) * CosTheta(wi) < 0.0f || dot(wm, wo) * CosTheta(wo) < 0.0f) return 0.0f;

        float F = FrDielectric(dot(wo, wm), eta);
        float pr = allowReflection ? F : 0.0f;
        float pt = allowTransmission ? max(1.0f - F, 0.0f) : 0.0f;
        float pSum = pr + pt;
        if (pSum <= EPS) return 0.0f;

        if (reflected)
            return allowReflection ? PDFGlossy(wo, wi, mfDistrib) * pr / pSum : 0.0f;
        return allowTransmission ? PDFTransmittance(wo, wi, eta, mfDistrib) * pt / pSum : 0.0f;
    }
};

public struct GlossyBSDF : IBxDF {
    float3 glossyF0;
    float3 glossyF90;
    float  roughness;
    float  anisotropyStrength;
    bool   energyCompensation;
    TrowbridgeReitzDistribution mfDistrib;
    uint32_t ggxLutEIndex;
    uint32_t ggxLutEavgIndex;

    public __init(float3 glossyF0, float3 glossyF90, float roughness, float anisotropyStrength, bool energyCompensation,
                  uint32_t ggxLutEIndex, uint32_t ggxLutEavgIndex) {
        this.glossyF0           = glossyF0;
        this.glossyF90          = glossyF90;
        this.roughness          = roughness;
        this.anisotropyStrength = anisotropyStrength;
        this.energyCompensation = energyCompensation;
        this.ggxLutEIndex       = ggxLutEIndex;
        this.ggxLutEavgIndex    = ggxLutEavgIndex;
        float alphaX, alphaY;
        CalcAnisotropicAlpha(roughness, anisotropyStrength, alphaX, alphaY);
        this.mfDistrib = TrowbridgeReitzDistribution(alphaX, alphaY);
    }

    bool IsSpecularDirection(float3 a, float3 b) {
        return LengthSquared(a - b) <= 1e-6f;
    }

    float3 SpecularF(float3 wo, float3 wi) {
        float3 wr = float3(-wo.x, -wo.y, wo.z);
        float cosWi = AbsCosTheta(wi);
        if (!IsSpecularDirection(wi, wr) || cosWi < EPS) return float3(0.0f);
        return SchlickFresnel(glossyF0, glossyF90, AbsCosTheta(wo)) / cosWi;
    }

    public BxDFFlags Flags() { return mfDistrib.EffectivelySmooth() ? BxDFFlags::SpecularReflection : BxDFFlags::GlossyReflection; }

    public SampledSpectrum f(float3 wo, float3 wi, TransportMode mode) {
        if (!SameHemisphere(wo, wi)) return 0.0f;
        if (mfDistrib.EffectivelySmooth()) return SpecularF(wo, wi);
        float2 lutE = GGXDirectionalAlbedoLut(ggxLutEIndex, wo, roughness, mfDistrib);
        float lutEavg = GGXAverageAlbedo(ggxLutEavgIndex, roughness, mfDistrib);
        return TorranceSparrowReflection(wo, wi, glossyF0, glossyF90, lutE, lutEavg, mfDistrib, energyCompensation);
    }

    public BSDFSample Sample_f(float3 wo, float uc, float2 u, TransportMode mode, BxDFReflTransFlags flags) {
        if (!(flags & BxDFReflTransFlags::Reflection)) return BSDFSample();
        if (mfDistrib.EffectivelySmooth()) {
            float3 wi = float3(-wo.x, -wo.y, wo.z);
            float cosWi = AbsCosTheta(wi);
            if (cosWi < EPS) return BSDFSample();
            float3 f = SchlickFresnel(glossyF0, glossyF90, AbsCosTheta(wo)) / cosWi;
            return BSDFSample(f, wi, 1.0f, BxDFFlags::SpecularReflection);
        }
        float3 wm = mfDistrib.Sample_wm(wo, u);
        float3 wi = Reflect(wo, wm);
        if (!SameHemisphere(wo, wi)) return BSDFSample();
        float pdf = PDFGlossy(wo, wi, mfDistrib);
        if (pdf < 1e-7f) return BSDFSample();
        return BSDFSample(this.f(wo, wi, mode), wi, pdf, BxDFFlags::GlossyReflection);
    }

    public float PDF(float3 wo, float3 wi, TransportMode mode, BxDFReflTransFlags flags) {
        if (!(flags & BxDFReflTransFlags::Reflection) || !SameHemisphere(wo, wi)) return 0.0f;
        if (mfDistrib.EffectivelySmooth()) {
            float3 wr = float3(-wo.x, -wo.y, wo.z);
            return IsSpecularDirection(wi, wr) ? 1.0f : 0.0f;
        }
        return PDFGlossy(wo, wi, mfDistrib);
    }
};

public struct MetallicBSDF : IBxDF {
    float3 metalF0;
    float  roughness;
    float  anisotropyStrength;
    bool   energyCompensation;
    TrowbridgeReitzDistribution mfDistrib;
    uint32_t ggxLutEIndex;
    uint32_t ggxLutEavgIndex;

    public __init(float3 metalF0, float roughness, float anisotropyStrength, bool energyCompensation,
                  uint32_t ggxLutEIndex, uint32_t ggxLutEavgIndex) {
        this.metalF0            = metalF0;
        this.roughness          = roughness;
        this.anisotropyStrength = anisotropyStrength;
        this.energyCompensation = energyCompensation;
        this.ggxLutEIndex       = ggxLutEIndex;
        this.ggxLutEavgIndex    = ggxLutEavgIndex;
        float alphaX, alphaY;
        CalcAnisotropicAlpha(roughness, anisotropyStrength, alphaX, alphaY);
        this.mfDistrib = TrowbridgeReitzDistribution(alphaX, alphaY);
    }

    bool IsSpecularDirection(float3 a, float3 b) {
        return LengthSquared(a - b) <= 1e-6f;
    }

    float3 SpecularF(float3 wo, float3 wi) {
        float3 wr = float3(-wo.x, -wo.y, wo.z);
        float cosWi = AbsCosTheta(wi);
        if (!IsSpecularDirection(wi, wr) || cosWi < EPS) return float3(0.0f);
        return SchlickFresnel(metalF0, float3(1.0f), AbsCosTheta(wo)) / cosWi;
    }

    public BxDFFlags Flags() { return mfDistrib.EffectivelySmooth() ? BxDFFlags::SpecularReflection : BxDFFlags::GlossyReflection; }

    public SampledSpectrum f(float3 wo, float3 wi, TransportMode mode) {
        if (!SameHemisphere(wo, wi)) return 0.0f;
        if (mfDistrib.EffectivelySmooth()) return SpecularF(wo, wi);
        float2 lutE = GGXDirectionalAlbedoLut(ggxLutEIndex, wo, roughness, mfDistrib);
        float lutEavg = GGXAverageAlbedo(ggxLutEavgIndex, roughness, mfDistrib);
        return TorranceSparrowReflection(wo, wi, metalF0, float3(1.0f), lutE, lutEavg, mfDistrib, energyCompensation);
    }

    public BSDFSample Sample_f(float3 wo, float uc, float2 u, TransportMode mode, BxDFReflTransFlags flags) {
        if (!(flags & BxDFReflTransFlags::Reflection)) return BSDFSample();
        if (mfDistrib.EffectivelySmooth()) {
            float3 wi = float3(-wo.x, -wo.y, wo.z);
            float cosWi = AbsCosTheta(wi);
            if (cosWi < EPS) return BSDFSample();
            float3 f = SchlickFresnel(metalF0, float3(1.0f), AbsCosTheta(wo)) / cosWi;
            return BSDFSample(f, wi, 1.0f, BxDFFlags::SpecularReflection);
        }
        float3 wm = mfDistrib.Sample_wm(wo, u);
        float3 wi = Reflect(wo, wm);
        if (!SameHemisphere(wo, wi)) return BSDFSample();
        float pdf = PDFGlossy(wo, wi, mfDistrib);
        if (pdf < 1e-7f) return BSDFSample();
        return BSDFSample(this.f(wo, wi, mode), wi, pdf, BxDFFlags::GlossyReflection);
    }

    public float PDF(float3 wo, float3 wi, TransportMode mode, BxDFReflTransFlags flags) {
        if (!(flags & BxDFReflTransFlags::Reflection) || !SameHemisphere(wo, wi)) return 0.0f;
        if (mfDistrib.EffectivelySmooth()) {
            float3 wr = float3(-wo.x, -wo.y, wo.z);
            return IsSpecularDirection(wi, wr) ? 1.0f : 0.0f;
        }
        return PDFGlossy(wo, wi, mfDistrib);
    }
};

public float SheenLTCDensity(float3 wi, float transformA, float transformB) {
    float lenSqr = Sqr(transformA * wi.x + transformB * wi.z) +
                   Sqr(transformA * wi.y) + Sqr(wi.z);
    if (lenSqr <= EPS) return 0.0f;
    return InvPi * max(wi.z, 0.0f) * Sqr(transformA / lenSqr);
}

public float SheenPhi(float3 w) {
    float p = atan2(w.y, w.x);
    return p < 0.0f ? p + 2.0f * Pi : p;
}

public float3 RotateAroundZ(float3 v, float angle) {
    float s = sin(angle);
    float c = cos(angle);
    return float3(c * v.x - s * v.y, s * v.x + c * v.y, v.z);
}

public struct SheenBSDF : IBxDF {
    float  weight;
    float  roughness;
    float3 tint;
    float  transformA;
    float  transformB;
    float  albedo;

    public __init(float weight, float roughness, float3 tint, uint32_t sheenLtcIndex, float3 wo = float3(0.0f, 0.0f, 1.0f)) {
        this.weight = saturate(weight);
        this.roughness = clamp(roughness, 1e-3f, 1.0f);
        this.tint = saturate(tint);
        float4 coeffs = SampleSheenLTC(sheenLtcIndex, AbsCosTheta(wo), this.roughness);
        this.transformA = coeffs.x;
        this.transformB = coeffs.y;
        this.albedo = coeffs.z;
    }

    public BxDFFlags Flags() { return BxDFFlags::DiffuseReflection; }

    public float3 Reflectance() {
        return weight * tint * albedo;
    }

    public bool IsValid() {
        return weight > EPS && MaxComponent(tint) > EPS && abs(transformA) > 1e-5f && albedo > 1e-5f;
    }

    public SampledSpectrum f(float3 wo, float3 wi, TransportMode mode) {
        if (!SameHemisphere(wo, wi) || !IsValid()) return 0.0f;
        float3 wiStd = RotateAroundZ(wi, -SheenPhi(wo));
        float pdf = SheenLTCDensity(wiStd, transformA, transformB);
        float cosWi = AbsCosTheta(wi);
        return cosWi > EPS ? weight * tint * albedo * pdf / cosWi : float3(0.0f);
    }

    public BSDFSample Sample_f(float3 wo, float uc, float2 u, TransportMode mode, BxDFReflTransFlags flags) {
        if (!(flags & BxDFReflTransFlags::Reflection) || !IsValid()) return BSDFSample();
        float2 disk = SampleConcentricDisk(u);
        float diskZ = sqrt(max(1.0f - dot(disk, disk), 0.0f));
        float3 wiStd = normalize(float3(disk.x - diskZ * transformB,
                                        disk.y,
                                        diskZ * transformA));
        float3 wi = RotateAroundZ(wiStd, SheenPhi(wo));
        wi = FaceForward(wi, float3(0.0f, 0.0f, 1.0f));
        if (!SameHemisphere(wo, wi)) return BSDFSample();
        float pdf = SheenLTCDensity(wiStd, transformA, transformB);
        if (pdf < 1e-7f) return BSDFSample();
        return BSDFSample(this.f(wo, wi, mode), wi, pdf, BxDFFlags::DiffuseReflection);
    }

    public float PDF(float3 wo, float3 wi, TransportMode mode, BxDFReflTransFlags flags) {
        if (!(flags & BxDFReflTransFlags::Reflection) || !SameHemisphere(wo, wi) || !IsValid()) return 0.0f;
        float3 wiStd = RotateAroundZ(wi, -SheenPhi(wo));
        return SheenLTCDensity(wiStd, transformA, transformB);
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

public interface IBSDF {
    public void f(float3 wo, float3 wi, TransportMode mode, out float3 fDiffuse, out float3 fSpecular);
    public SampledSpectrum f(float3 wo, float3 wi, TransportMode mode);
    public float PDF(float3 wo, float3 wi, TransportMode mode, BxDFReflTransFlags flags);
    public BSDFSample Sample_f(float3 wo, float uc, float2 u, TransportMode mode, BxDFReflTransFlags flags,
                               out AOVCategory category);
};

static const int HairPMax = 3;

public struct HairDirection
{
    // PBRT hair frame: x = strand tangent, y/z = fiber cross-section.
    float sinTheta;
    float cosTheta;
    float phi;
    float sinPhi;
    float cosPhi;
};

public HairDirection HairDirectionFromLocal(float3 w)
{
    HairDirection dir;
    dir.sinTheta = clamp(w.x, -1.0f, 1.0f);
    dir.cosTheta = SafeSqrt(1.0f - dir.sinTheta * dir.sinTheta);
    if (dir.cosTheta > 1e-7f)
    {
        dir.cosPhi = clamp(w.y / dir.cosTheta, -1.0f, 1.0f);
        dir.sinPhi = clamp(w.z / dir.cosTheta, -1.0f, 1.0f);
    }
    else
    {
        dir.sinPhi = 0.0f;
        dir.cosPhi = 1.0f;
    }
    dir.phi = atan2(dir.sinPhi, dir.cosPhi);
    return dir;
}

public float3 HairDirectionToLocal(HairDirection dir)
{
    return float3(dir.sinTheta, dir.cosTheta * dir.cosPhi, dir.cosTheta * dir.sinPhi);
}

float HairI0(float x)
{
    float sum = 1.0f;
    float term = 1.0f;
    float x2 = 0.25f * x * x;
    [unroll]
    for (int i = 1; i <= 9; ++i)
    {
        term *= x2 / float(i * i);
        sum += term;
    }
    return sum;
}

float HairLogI0(float x)
{
    return x > 12.0f ? x + 0.5f * (-log(2.0f * Pi * x) + 1.0f / (8.0f * x)) : log(max(HairI0(x), EPS));
}

float HairMp(float cosThetaI, float cosThetaO, float sinThetaI, float sinThetaO, float v)
{
    v = max(v, 1e-4f);
    float a = cosThetaI * cosThetaO / v;
    float b = sinThetaI * sinThetaO / v;
    if (v <= 0.1f)
        return exp(HairLogI0(a) - b - 1.0f / v + 0.6931f + log(1.0f / (2.0f * v)));

    float sinhInvV = 0.5f * (exp(1.0f / v) - exp(-1.0f / v));
    return exp(-b) * HairI0(a) / max(sinhInvV * 2.0f * v, EPS);
}

float HairSafeASin(float x)
{
    return asin(clamp(x, -1.0f, 1.0f));
}

float HairWrapPi(float phi)
{
    return phi - 2.0f * Pi * floor((phi + Pi) / (2.0f * Pi));
}

float HairLogistic(float x, float s)
{
    s = max(s, 1e-4f);
    x = abs(x);
    float e = exp(-x / s);
    return e / (s * Sqr(1.0f + e));
}

float HairLogisticCDF(float x, float s)
{
    return 1.0f / (1.0f + exp(-x / max(s, 1e-4f)));
}

float HairTrimmedLogistic(float x, float s, float a, float b)
{
    return HairLogistic(x, s) / max(HairLogisticCDF(b, s) - HairLogisticCDF(a, s), EPS);
}

float HairSampleTrimmedLogistic(float u, float s, float a, float b)
{
    u = clamp(u, 1e-6f, 1.0f - 1e-6f);
    float k = HairLogisticCDF(b, s) - HairLogisticCDF(a, s);
    float x = -s * log(1.0f / (u * k + HairLogisticCDF(a, s)) - 1.0f);
    return clamp(x, a, b);
}

float HairPhi(int p, float gammaO, float gammaT)
{
    return 2.0f * float(p) * gammaT - 2.0f * gammaO + float(p) * Pi;
}

float HairNp(float phi, int p, float s, float gammaO, float gammaT)
{
    float dphi = HairWrapPi(phi - HairPhi(p, gammaO, gammaT));
    return HairTrimmedLogistic(dphi, s, -Pi, Pi);
}

float3 HairSigmaAFromReflectance(float3 c, float betaN)
{
    c = clamp(c, float3(1e-3f), float3(0.999f));
    float denom = 5.969f - 0.215f * betaN + 2.532f * Sqr(betaN)
                - 10.73f * betaN * betaN * betaN
                + 5.574f * betaN * betaN * betaN * betaN
                + 0.245f * betaN * betaN * betaN * betaN * betaN;
    float3 l = log(c) / max(denom, 1e-3f);
    return l * l;
}

float HairSpectrumAverage(float3 v)
{
    return (v.x + v.y + v.z) * (1.0f / 3.0f);
}

// https://www.pbr-book.org/4ed/Reflection_Models/Scattering_from_Hair
// PBRT's implementation of A Practical and Controllable Hair and Fur Model for Production Path Tracing [Chiang et al. 2016]
// - https://sci-hub.sg/storage/2024/5766/a1c3a0a1d0aeccafa669d9c39f33341d/chiang2016.pdf
public struct HairBSDF : IBSDF
{
    float h;
    float eta;
    float3 sigmaA;
    float v0;
    float v1;
    float v2;
    float v3;
    float s;
    float sin2kAlpha0;
    float sin2kAlpha1;
    float sin2kAlpha2;
    float cos2kAlpha0;
    float cos2kAlpha1;
    float cos2kAlpha2;
    // h: the  offset along the curve width where the ray intersected the oriented ribbon.
    // eta: the index of refraction of the interior of the hair (typically, 1.55).    
    // beta_m: the longitudinal roughness of the hair, mapped to [0, 1].
    // beta_n: the azimuthal roughness, also mapped to [0, 1].
    // alpha: the angle at which the small scales on the surface of hair are offset from the base cylinder, expressed in degrees (typically, 2).
    public __init(float3 reflectance, float betaM, float betaN, float h = 0.0f, float eta = 1.55f, float alpha = 2.0f)
    {
        this.h = clamp(h, -1.0f, 1.0f);
        this.eta = max(eta, 1.0f + 5e-3f);

        betaM = clamp(betaM, 0.03f, 1.0f);
        betaN = clamp(betaN, 0.03f, 1.0f);
        this.sigmaA = HairSigmaAFromReflectance(reflectance, betaN);

        this.v0 = Sqr(0.726f * betaM + 0.812f * Sqr(betaM) + 3.7f * pow(betaM, 20.0f));
        this.v1 = 0.25f * this.v0;
        this.v2 = 4.0f * this.v0;
        this.v3 = this.v2;

        static const float SqrtPiOver8 = 0.626657069f;
        this.s = SqrtPiOver8 * (0.265f * betaN + 1.194f * Sqr(betaN) + 5.372f * pow(betaN, 22.0f));

        float alphaRadians = alpha * Pi / 180.0f;
        this.sin2kAlpha0 = sin(alphaRadians);
        this.cos2kAlpha0 = SafeSqrt(1.0f - Sqr(this.sin2kAlpha0));
        this.sin2kAlpha1 = 2.0f * this.cos2kAlpha0 * this.sin2kAlpha0;
        this.cos2kAlpha1 = Sqr(this.cos2kAlpha0) - Sqr(this.sin2kAlpha0);
        this.sin2kAlpha2 = 2.0f * this.cos2kAlpha1 * this.sin2kAlpha1;
        this.cos2kAlpha2 = Sqr(this.cos2kAlpha1) - Sqr(this.sin2kAlpha1);
    }

    public float V(int p)
    {
        if (p == 0) return v0;
        if (p == 1) return v1;
        if (p == 2) return v2;
        return v3;
    }

    public void AdjustThetaO(int p, HairDirection wo, out float sinThetaOP, out float cosThetaOP)
    {
        if (p == 0)
        {
            sinThetaOP = wo.sinTheta * cos2kAlpha1 - wo.cosTheta * sin2kAlpha1;
            cosThetaOP = wo.cosTheta * cos2kAlpha1 + wo.sinTheta * sin2kAlpha1;
        }
        else if (p == 1)
        {
            sinThetaOP = wo.sinTheta * cos2kAlpha0 + wo.cosTheta * sin2kAlpha0;
            cosThetaOP = wo.cosTheta * cos2kAlpha0 - wo.sinTheta * sin2kAlpha0;
        }
        else if (p == 2)
        {
            sinThetaOP = wo.sinTheta * cos2kAlpha2 + wo.cosTheta * sin2kAlpha2;
            cosThetaOP = wo.cosTheta * cos2kAlpha2 - wo.sinTheta * sin2kAlpha2;
        }
        else
        {
            sinThetaOP = wo.sinTheta;
            cosThetaOP = wo.cosTheta;
        }
        cosThetaOP = abs(cosThetaOP);
    }

    public float3 ApValue(int p, float cosThetaO, float3 T)
    {
        float cosGammaO = SafeSqrt(1.0f - Sqr(h));
        float f = FrDielectric(cosThetaO * cosGammaO, eta);

        float3 ap0 = float3(f);
        float3 ap1 = Sqr(1.0f - f) * T;
        float3 ap2 = ap1 * T * f;
        float3 ap3 = ap2 * f * T / max(float3(EPS), 1.0f - T * f);

        if (p == 0) return ap0;
        if (p == 1) return ap1;
        if (p == 2) return ap2;
        return ap3;
    }

    public void ApPDF(float cosThetaO, out float apPDF0, out float apPDF1, out float apPDF2, out float apPDF3)
    {
        float sinThetaO = SafeSqrt(1.0f - Sqr(cosThetaO));
        float sinThetaT = sinThetaO / eta;
        float cosThetaT = SafeSqrt(1.0f - Sqr(sinThetaT));
        float etaP = SafeSqrt(Sqr(eta) - Sqr(sinThetaO)) / max(cosThetaO, EPS);
        float sinGammaT = h / max(etaP, EPS);
        float cosGammaT = SafeSqrt(1.0f - Sqr(sinGammaT));
        float3 T = exp(-sigmaA * (2.0f * cosGammaT / max(cosThetaT, EPS)));

        float a0 = HairSpectrumAverage(ApValue(0, cosThetaO, T));
        float a1 = HairSpectrumAverage(ApValue(1, cosThetaO, T));
        float a2 = HairSpectrumAverage(ApValue(2, cosThetaO, T));
        float a3 = HairSpectrumAverage(ApValue(3, cosThetaO, T));
        float sumA = max(a0 + a1 + a2 + a3, EPS);
        apPDF0 = a0 / sumA;
        apPDF1 = a1 / sumA;
        apPDF2 = a2 / sumA;
        apPDF3 = a3 / sumA;
    }

    public float ApPDFValue(int p, float apPDF0, float apPDF1, float apPDF2, float apPDF3)
    {
        if (p == 0) return apPDF0;
        if (p == 1) return apPDF1;
        if (p == 2) return apPDF2;
        return apPDF3;
    }

    public int SampleLobe(float uc, float apPDF0, float apPDF1, float apPDF2, float apPDF3, out float remappedUc)
    {
        float cumulative = apPDF0;
        if (uc < cumulative)
        {
            remappedUc = apPDF0 > EPS ? uc / apPDF0 : 0.0f;
            return 0;
        }

        cumulative += apPDF1;
        if (uc < cumulative)
        {
            remappedUc = apPDF1 > EPS ? (uc - apPDF0) / apPDF1 : 0.0f;
            return 1;
        }

        cumulative += apPDF2;
        if (uc < cumulative)
        {
            remappedUc = apPDF2 > EPS ? (uc - apPDF0 - apPDF1) / apPDF2 : 0.0f;
            return 2;
        }

        remappedUc = apPDF3 > EPS ? (uc - apPDF0 - apPDF1 - apPDF2) / apPDF3 : 0.0f;
        return 3;
    }

    public SampledSpectrum f(float3 wo, float3 wi, TransportMode mode)
    {
        HairDirection woHair = HairDirectionFromLocal(wo);
        HairDirection wiHair = HairDirectionFromLocal(wi);

        float sinThetaT = woHair.sinTheta / eta;
        float cosThetaT = SafeSqrt(1.0f - Sqr(sinThetaT));
        float etaP = SafeSqrt(Sqr(eta) - Sqr(woHair.sinTheta)) / max(woHair.cosTheta, EPS);
        float sinGammaT = h / max(etaP, EPS);
        float cosGammaT = SafeSqrt(1.0f - Sqr(sinGammaT));
        float gammaT = HairSafeASin(sinGammaT);
        float gammaO = HairSafeASin(h);
        float3 T = exp(-sigmaA * (2.0f * cosGammaT / max(cosThetaT, EPS)));

        float phi = wiHair.phi - woHair.phi;
        float3 fsum = float3(0.0f);
        [unroll]
        for (int p = 0; p < HairPMax; ++p)
        {
            float sinThetaOP, cosThetaOP;
            AdjustThetaO(p, woHair, sinThetaOP, cosThetaOP);
            fsum += HairMp(wiHair.cosTheta, cosThetaOP, wiHair.sinTheta, sinThetaOP, V(p))
                  * ApValue(p, woHair.cosTheta, T)
                  * HairNp(phi, p, s, gammaO, gammaT);
        }
        fsum += HairMp(wiHair.cosTheta, woHair.cosTheta, wiHair.sinTheta, woHair.sinTheta, V(HairPMax))
              * ApValue(HairPMax, woHair.cosTheta, T) * (1.0f / (2.0f * Pi));

        float cosWi = AbsCosTheta(wi);
        return cosWi > EPS ? fsum / cosWi : float3(0.0f);
    }

    public void f(float3 wo, float3 wi, TransportMode mode, out float3 fDiffuse, out float3 fSpecular)
    {
        fDiffuse = float3(0.0f);
        fSpecular = this.f(wo, wi, mode);
    }

    public float PDF(float3 wo, float3 wi, TransportMode mode, BxDFReflTransFlags flags)
    {
        bool allowReflection = (flags & BxDFReflTransFlags::Reflection);
        bool allowTransmission = (flags & BxDFReflTransFlags::Transmission);
        if (!allowReflection && !allowTransmission)
            return 0.0f;

        HairDirection woHair = HairDirectionFromLocal(wo);
        HairDirection wiHair = HairDirectionFromLocal(wi);
        float etaP = SafeSqrt(Sqr(eta) - Sqr(woHair.sinTheta)) / max(woHair.cosTheta, EPS);
        float sinGammaT = h / max(etaP, EPS);
        float gammaT = HairSafeASin(sinGammaT);
        float gammaO = HairSafeASin(h);

        float apPDF0, apPDF1, apPDF2, apPDF3;
        ApPDF(woHair.cosTheta, apPDF0, apPDF1, apPDF2, apPDF3);
        if (!allowReflection)
            apPDF0 = 0.0f;
        if (!allowTransmission)
        {
            apPDF1 = 0.0f;
            apPDF2 = 0.0f;
            apPDF3 = 0.0f;
        }
        float allowedPDFSum = apPDF0 + apPDF1 + apPDF2 + apPDF3;
        if (allowedPDFSum <= EPS)
            return 0.0f;
        apPDF0 /= allowedPDFSum;
        apPDF1 /= allowedPDFSum;
        apPDF2 /= allowedPDFSum;
        apPDF3 /= allowedPDFSum;

        float phi = wiHair.phi - woHair.phi;
        float pdf = 0.0f;
        [unroll]
        for (int p = 0; p < HairPMax; ++p)
        {
            if ((p == 0 && !allowReflection) || (p > 0 && !allowTransmission))
                continue;

            float sinThetaOP, cosThetaOP;
            AdjustThetaO(p, woHair, sinThetaOP, cosThetaOP);
            pdf += HairMp(wiHair.cosTheta, cosThetaOP, wiHair.sinTheta, sinThetaOP, V(p))
                 * ApPDFValue(p, apPDF0, apPDF1, apPDF2, apPDF3)
                 * HairNp(phi, p, s, gammaO, gammaT);
        }
        if (allowTransmission)
        {
            pdf += HairMp(wiHair.cosTheta, woHair.cosTheta, wiHair.sinTheta, woHair.sinTheta, V(HairPMax))
                 * apPDF3 * (1.0f / (2.0f * Pi));
        }
        return pdf;
    }

    public BSDFSample Sample_f(float3 wo, float uc, float2 u, TransportMode mode, BxDFReflTransFlags flags,
                               out AOVCategory category)
    {
        category = AOVCategory::Specular;
        bool allowReflection = (flags & BxDFReflTransFlags::Reflection);
        bool allowTransmission = (flags & BxDFReflTransFlags::Transmission);
        if (!allowReflection && !allowTransmission)
            return BSDFSample();

        HairDirection woHair = HairDirectionFromLocal(wo);
        float apPDF0, apPDF1, apPDF2, apPDF3;
        ApPDF(woHair.cosTheta, apPDF0, apPDF1, apPDF2, apPDF3);
        if (!allowReflection)
            apPDF0 = 0.0f;
        if (!allowTransmission)
        {
            apPDF1 = 0.0f;
            apPDF2 = 0.0f;
            apPDF3 = 0.0f;
        }
        float allowedPDFSum = apPDF0 + apPDF1 + apPDF2 + apPDF3;
        if (allowedPDFSum <= EPS)
            return BSDFSample();
        apPDF0 /= allowedPDFSum;
        apPDF1 /= allowedPDFSum;
        apPDF2 /= allowedPDFSum;
        apPDF3 /= allowedPDFSum;

        float remappedUc;
        int p = SampleLobe(uc, apPDF0, apPDF1, apPDF2, apPDF3, remappedUc);

        float sinThetaOP, cosThetaOP;
        AdjustThetaO(p, woHair, sinThetaOP, cosThetaOP);

        float v = V(p);
        float cosTheta = 1.0f + v * log(max(u.x, 1e-5f) + (1.0f - u.x) * exp(-2.0f / max(v, 1e-4f)));
        float sinTheta = SafeSqrt(1.0f - Sqr(cosTheta));
        float cosPhi = cos(2.0f * Pi * u.y);
        float sinThetaI = -cosTheta * sinThetaOP + sinTheta * cosPhi * cosThetaOP;
        float cosThetaI = SafeSqrt(1.0f - Sqr(sinThetaI));

        float etaP = SafeSqrt(Sqr(eta) - Sqr(woHair.sinTheta)) / max(woHair.cosTheta, EPS);
        float sinGammaT = h / max(etaP, EPS);
        float gammaT = HairSafeASin(sinGammaT);
        float gammaO = HairSafeASin(h);

        float dphi = p < HairPMax
            ? HairPhi(p, gammaO, gammaT) + HairSampleTrimmedLogistic(remappedUc, s, -Pi, Pi)
            : 2.0f * Pi * remappedUc;
        float phiI = woHair.phi + dphi;
        float3 wi = float3(sinThetaI, cosThetaI * cos(phiI), cosThetaI * sin(phiI));

        float pdf = PDF(wo, wi, mode, flags);
        if (pdf <= EPS)
            return BSDFSample();

        BxDFFlags sampleFlags = p == 0 ? BxDFFlags::GlossyReflection : BxDFFlags::GlossyTransmission;
        return BSDFSample(f(wo, wi, mode), wi, pdf, sampleFlags);
    }
};

public struct PrincipledBSDF : IBSDF {
    // Lobe instances
    DiffuseBSDF   diffuse;
    GlassBSDF     glass;
    GlossyBSDF    glossy;
    MetallicBSDF  metallic;
    SheenBSDF     sheen;
    GlossyBSDF    coat;

    // Normalized weights
    float wDiffuse;
    float wTrans;
    float wDielSpec;
    float wMetal;
    float wSheen;
    float wCoat;

    // Structural weights
    float cDiffuse;
    float cTrans;
    float cDielSpec;
    float cMetal;
    float cBase;
    float cSheen;
    float cCoat;
    float cCoatWeight;
    float cUnderCoat;

    // Active flags
    bool hasDiffuse;
    bool hasTrans;
    bool hasDielSpec;
    bool hasMetal;
    bool hasSheen;
    bool hasCoat;

    float weightSum;

    public __init(float3 baseColor, float metallic, float roughness, float ior, float specularFactor = 1.0f,
                  float transmission = 0.0f, bool energyCompensation = true, float3 wo = float3(0,0,1),
                  float diffuseWeight = 1.0f, float anisotropyStrength = 0.0f,
                  float3 specularColorFactor = float3(1.0f), uint32_t ggxLutEIndex = ~0u, uint32_t ggxLutEavgIndex = ~0u, uint32_t ggxLutEIORIndex = ~0u, uint32_t ggxLutEIORavgIndex = ~0u, uint32_t ggxLutEIORInvIndex = ~0u, uint32_t ggxLutEIORInvavgIndex = ~0u,
                  float sheenWeight = 0.0f, float sheenRoughness = 0.0f, float3 sheenTint = float3(1.0f),
                  uint32_t sheenLtcIndex = ~0u,
                  float coatWeight = 0.0f, float coatRoughness = 0.0f) {
        float3 dielF0, dielF90;
        CalcGlossyFresnel(ior, specularFactor, specularColorFactor, dielF0, dielF90);

        float3 coatF0, coatF90;
        CalcCoatFresnel(coatF0, coatF90);
        this.cCoatWeight = saturate(coatWeight);
        coatRoughness = max(8e-3f, coatRoughness);
        float coatReflectance = MaxComponent(CoatReflectance(this.cCoatWeight, coatRoughness, wo, ggxLutEIndex, ggxLutEavgIndex, energyCompensation));
        this.cCoat = clamp(coatReflectance, 0.0f, 1.0f);
        this.cUnderCoat = max(1.0f - this.cCoat, 0.0f);

        this.sheen = SheenBSDF(sheenWeight, sheenRoughness, sheenTint, sheenLtcIndex, wo);
        bool sheenValid = this.sheen.IsValid();
        float sheenReflectance = sheenValid ? MaxComponent(this.sheen.Reflectance()) : 0.0f;
        this.cSheen = this.cUnderCoat * clamp(sheenReflectance, 0.0f, 1.0f);
        this.cBase = max(this.cUnderCoat - this.cSheen, 0.0f);

        metallic = saturate(metallic);
        transmission = saturate(transmission);
        float opaqueDielectric = this.cBase * (1.0f - metallic) * (1.0f - transmission);
        float translucentDielectric = this.cBase * (1.0f - metallic) * transmission;
        float glossyAlphaX, glossyAlphaY;
        CalcAnisotropicAlpha(roughness, anisotropyStrength, glossyAlphaX, glossyAlphaY);
        TrowbridgeReitzDistribution glossyDistrib = TrowbridgeReitzDistribution(glossyAlphaX, glossyAlphaY);
        float2 glossyLutE = GGXDirectionalAlbedoLut(ggxLutEIndex, wo, roughness, glossyDistrib);
        float glossyLutEavg = GGXAverageAlbedo(ggxLutEavgIndex, roughness, glossyDistrib);
        float sampleGlossy = MaxComponent(GlossyDirectionalReflectance(dielF0, dielF90, glossyLutE, glossyLutEavg, energyCompensation));
        float rawDiffuseFamily = opaqueDielectric * max(1.0f - sampleGlossy, 0.0f);
        float rawMetal = this.cBase * metallic;
        float rawDielSpec = opaqueDielectric * sampleGlossy;
        float rawTrans = translucentDielectric;
        float rawDiffuse = rawDiffuseFamily * diffuseWeight;
        float rawSheen = this.cSheen;
        float rawCoat = this.cCoat;

        this.hasDiffuse  = rawDiffuse  > EPS;
        this.hasTrans    = rawTrans    > EPS;
        this.hasDielSpec = rawDielSpec > EPS;
        this.hasMetal    = rawMetal    > EPS;
        this.hasSheen    = rawSheen    > EPS && sheenValid;
        this.hasCoat     = rawCoat     > EPS;

        this.weightSum = rawDiffuse + rawTrans + rawDielSpec + rawMetal + rawSheen + rawCoat;
        if (this.weightSum <= 0.0f) this.weightSum = 1.0f;

        this.wDiffuse  = rawDiffuse  / this.weightSum;
        this.wTrans    = rawTrans    / this.weightSum;
        this.wDielSpec = rawDielSpec / this.weightSum;
        this.wMetal    = rawMetal    / this.weightSum;
        this.wSheen    = rawSheen    / this.weightSum;
        this.wCoat     = rawCoat     / this.weightSum;

        this.cDiffuse  = opaqueDielectric * diffuseWeight;
        this.cTrans    = translucentDielectric;
        this.cDielSpec = opaqueDielectric;
        this.cMetal    = this.cBase * metallic;

        // Construct each lobe
        this.diffuse  = DiffuseBSDF(baseColor, dielF0, dielF90, roughness, anisotropyStrength, energyCompensation, ggxLutEIndex, ggxLutEavgIndex);
        this.glass     = GlassBSDF(baseColor, ior, dielF0, dielF90, roughness, anisotropyStrength, energyCompensation,
                                   ggxLutEIndex, ggxLutEavgIndex, ggxLutEIORIndex, ggxLutEIORavgIndex, ggxLutEIORInvIndex, ggxLutEIORInvavgIndex);
        this.glossy    = GlossyBSDF(dielF0, dielF90, roughness, anisotropyStrength, energyCompensation,
                                    ggxLutEIndex, ggxLutEavgIndex);
        this.metallic  = MetallicBSDF(baseColor, roughness, anisotropyStrength, energyCompensation, ggxLutEIndex, ggxLutEavgIndex);
        this.coat      = GlossyBSDF(coatF0, coatF90, coatRoughness, 0.0f, energyCompensation, ggxLutEIndex, ggxLutEavgIndex);
    }

    public void f(float3 wo, float3 wi, TransportMode mode,
                        out float3 fDiffuse, out float3 fSpecular) {
        fDiffuse  = float3(0);
        fSpecular = float3(0);
        if (hasDiffuse)  fDiffuse   = cDiffuse  * diffuse.f(wo, wi, mode);
        if (hasSheen)    fDiffuse  += cUnderCoat * sheen.f(wo, wi, mode);
        if (hasTrans)    fSpecular += cTrans    * glass.f(wo, wi, mode);
        if (hasDielSpec) fSpecular += cDielSpec * glossy.f(wo, wi, mode);
        if (hasMetal)    fSpecular += cMetal    * metallic.f(wo, wi, mode);
        if (hasCoat)     fSpecular += cCoatWeight * coat.f(wo, wi, mode);
    }

    public SampledSpectrum f(float3 wo, float3 wi, TransportMode mode) {
        float3 d, s;
        f(wo, wi, mode, d, s);
        return d + s;
    }

    public float PDF(float3 wo, float3 wi, TransportMode mode, BxDFReflTransFlags flags) {
        float result = 0.0f;
        if (hasDiffuse)  result += wDiffuse  * diffuse.PDF(wo, wi, mode, flags);
        if (hasSheen)    result += wSheen    * sheen.PDF(wo, wi, mode, flags);
        if (hasTrans)    result += wTrans    * glass.PDF(wo, wi, mode, flags);
        if (hasDielSpec) result += wDielSpec * glossy.PDF(wo, wi, mode, flags);
        if (hasMetal)    result += wMetal    * metallic.PDF(wo, wi, mode, flags);
        if (hasCoat)     result += wCoat     * coat.PDF(wo, wi, mode, flags);
        return result;
    }

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
        if (hasSheen) {
            cumulative += wSheen;
            if (!found && uc < cumulative) {
                float lo = cumulative - wSheen;
                uc = (cumulative > lo) ? clamp((uc - lo) / (cumulative - lo), 0.0f, 1.0f) : 0.0f;
                chosen = sheen;
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
        if (hasCoat) {
            cumulative += wCoat;
            if (!found && uc < cumulative) {
                float lo = cumulative - wCoat;
                uc = (cumulative > lo) ? clamp((uc - lo) / (cumulative - lo), 0.0f, 1.0f) : 0.0f;
                chosen = coat;
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