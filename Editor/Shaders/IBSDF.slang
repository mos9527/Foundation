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
// https://open-shading-language.readthedocs.io/en/v1.13.12.0/stdlib.html#layering-and-signaling-closures
public float layer(float base, float top, float topReflectance /* from reflectance() */) {
    return top + base * (1.0f - topReflectance);
}
public float3 layer(float3 base, float3 top, float3 topReflectance /* from reflectance() */) {
    return top + base * (1.0f - topReflectance);
}

// Inspired by Blender's OSL implementation
//  https://projects.blender.org/blender/blender/src/commit/96d715c643888c78e5dbaa8bd3c3c79ce599c0a3/intern/cycles/kernel/osl/shaders/node_principled_bsdf.osl#L15
// The layered model is the following: [ [Diffuse (mix) Transmittance] (layer) Dielectric (Glass) Specular ] (mix) Metal
public struct PrincipledBSDF : IBxDF {
    const static float dielF90 = 1.0f;
    const static float metalF90 = 1.0f;

    float3 baseColor;
    float metallic;
    float roughness;
    float transmission;
    float eta;

    float dielF0;
    float3 metalF0;
    bool energyCompensation;

    TrowbridgeReitzDistribution mfDistrib;

    public __init(float3 baseColor, float metallic, float roughness, float ior /* incidence/exit */,float transmission = 0.0f,  bool energyCompensation = true) {
        this.baseColor = baseColor;
        this.metallic = metallic;
        this.transmission = transmission;

        this.eta = ior;
        this.dielF0 = F0FromIor(ior);
        this.metalF0 = baseColor;
        // Keep it simple - we don't need to deal with EffectivelySmooth this way
        roughness = max(EPS, roughness);

        this.roughness = roughness;
        this.energyCompensation = energyCompensation;

        float alpha = roughness * roughness;
        this.mfDistrib = TrowbridgeReitzDistribution(alpha, alpha);
    }

    public BxDFFlags Flags() {
        return BxDFFlags::Reflection | BxDFFlags::Glossy;
    }

    private float3 TorranceSparrowPreserveEnergyReflection(float3 wo, float3 wi, float3 F0, float3 F90, float2 lutE) {
        if (!SameHemisphere(wo,wi)) return 0;

        float3 wm = normalize(wo + wi);
        float3 Fss = SchlickFresnel(F0, F90, AbsDot(wo, wm));
        float3 Fms = 0.0f;
        if (energyCompensation) {
            float3 Epp = max(reflectance(F0, F90, lutE), EPS);
            Fms = F0 * (1.0f / Epp - 1.0f) * Fss;
        }
        // Jacobian from wm to wi. See ConductorBxDF in IBxDF.slang
        return (Fss + Fms) * mfDistrib.D(wm) * mfDistrib.G(wo, wi) / (4.0f * AbsCosTheta(wo) * AbsCosTheta(wi));
    }

    // NOTE: wo, wi must be in two hemispheres (both z > 0 -> reflection)
    private float3 TorranceSparrowPreserveEnergyTransmittance(float3 wo, float3 wi, float3 F0, float3 F90, float2 lutE) {
        if (SameHemisphere(wo,wi)) return 0;

        float etap = 1 / eta;
        float3 wm = normalize(wo + wi * eta);
        if (wm.z < 0) wm = -wm;

        float3 Fss = SchlickFresnel(F0, F90, AbsDot(wo, wm));
        float3 Fms = 0.0f;
        if (energyCompensation) {
            float3 Epp = max(reflectance(F0, F90, lutE), EPS);
            Fms = F0 * (1.0f / Epp - 1.0f) * Fss;
        }
        float3 T = 1 - (Fss + Fms);
        // Another jacobian for transmittance. See DielectricBxDF in IBxDF.slang
        float denom = Sqr(dot(wi, wm) + dot(wo,wm) / etap) * abs(CosTheta(wi) * CosTheta(wo));
        return T * mfDistrib.D(wm) * mfDistrib.G(wo, wi) * abs(dot(wi,wm) * dot(wo,wm) / denom);
    }

    public SampledSpectrum f(float3 wo, float3 wi, TransportMode mode) {
        bool reflected = SameHemisphere(wo, wi);

        float2 lutE = SampleGGXLutE(AbsCosTheta(wo), roughness);

        // Diffuse/transmittance
        float3 diffuseBSDF = baseColor * InvPi * (1.0f - reflectance(dielF0, dielF90, lutE));
        float3 transmittanceBTDF = reflected ? 0 : baseColor * TorranceSparrowPreserveEnergyTransmittance(wo, wi, dielF0, dielF90, lutE);

        float etap = 1 / eta;
        // Adjust for non-symmetric transport
        if (mode == TransportMode::Radiance)
            transmittanceBTDF *= Sqr(etap);

        float3 bsdf = mix(diffuseBSDF, transmittanceBTDF, transmission);

        // Dielectric specular
        float3 dielBSDF = reflected ? TorranceSparrowPreserveEnergyReflection(wo, wi, dielF0, dielF90, lutE) : 0;
        bsdf = dielBSDF + bsdf;

        // Metal
        // There's no diffuse lobe anymore (completely absorbed!)
        // Blender has a F82 tint model for modeling F0, but for convenience's sake
        // (since glTF never does that) we'll use baseColor for that.
        float3 metalBSDF = reflected ? TorranceSparrowPreserveEnergyReflection(wo, wi, metalF0, metalF90, lutE) : 0;
        bsdf = mix(bsdf, metalBSDF, metallic);

        return bsdf;
    }

    public float PDFGlossy(float3 wo, float3 wi) {
        float3 wm = normalize(wo + wi);
        // Jacobian for reflection
        return mfDistrib.PDF(wo, wm) / (4.0f * AbsDot(wo, wm));
    }

    public float PDFTransmittance(float3 wo, float3 wi) {
        float etap = 1 / eta;
        float3 wm = normalize(wo + wi * eta);
        wm = FaceForward(wm, float3(0,0,1));
        // Jacobian for transmission
        // https://www.pbr-book.org/4ed/Reflection_Models/Rough_Dielectric_BSDF#eq:mf-transmission-change-of-vars
        float denom = Sqr(dot(wi, wm) + dot(wo,wm) / etap);
        float dwm_dwi = AbsDot(wi, wm) / denom * Sqr(etap);
        return mfDistrib.PDF(wo, wm) * dwm_dwi;
    }

    public float PDF(float3 wo, float3 wi, TransportMode mode, BxDFReflTransFlags flags) {
        float3 wm = normalize(wo + wi);

        bool reflected = SameHemisphere(wo, wi);
        // Diffuse/transmittance
        float transmittancePDF = reflected ? 0.0f : PDFTransmittance(wo, wi);
        float diffusePDF = reflected ? CosineHemispherePDF(AbsCosTheta(wi)) : 0.0f;
        float pdf = mix(diffusePDF, transmittancePDF, transmission);

        // Dielectric specular
        float glossyPDF = reflected ? PDFGlossy(wo, wi) : 0.0f;
        float dielPDF = reflected ? glossyPDF : 0.0f;

        // Compensate for TIR in transmission PDF
        // Effectively this raises the probability of sampling reflection
        if (reflected && transmission > EPS) {
            bool tir;
            Refract(wo, wm, eta, tir);
            if (tir)
                pdf += transmission * glossyPDF;
        }
        // https://pbr-book.org/4ed/Monte_Carlo_Integration/Improving_Efficiency#MultipleImportanceSampling
        // ---
        // Two sampling methods (techniques) can arrive here, with p(f) of glossyPDF and diffusePDF
        // with probabilities of sampleGlossy and (1 - sampleGlossy) respectively.
        // Assuming we'd use Balance Heuristic, the single sample model would be:
        //  Fs = fGlossy/pdfGlossy * (1/sampleGlossy) * BalanceHeuristic(1, pdfGlossy * sampleGlossy, 1, pdfTransmit * (1 - sampleGlossy))
        // and for diffuse
        //  Fd = fDiffuse/pdfTransmit * (1/(1 - sampleGlossy)) * BalanceHeuristic(1, pdfTransmit * (1-sampleGlossy), 1, pdfGlossy * sampleGlossy)
        // ...Take the glossy path for example and simplify:
        //  Fs = fGlossy/pdfGlossy * (1/sampleGlossy) * ((pdfGlossy * sampleGlossy) / (pdfGlossy * sampleGlossy + pdfTransmit * (1 - sampleGlossy)))
        //     = fGlossy / (pdfGlossy * sampleGlossy + pdfTransmit * (1 - sampleGlossy))
        //  Fd = ...
        //     = fDiffuse / (pdfTransmit * (1 - sampleGlossy) + pdfGlossy * sampleGlossy)
        // Denominator is the same! Thus the final PDF is simply:
        // PDF = pdfTransmit * (1 - sampleGlossy) + pdfGlossy * sampleGlossy
        float sampleGlossy = reflectance(dielF0, dielF90, SampleGGXLutE(AbsCosTheta(wo), roughness));
        pdf = mix(pdf, dielPDF, sampleGlossy);

        // Metal (Linearly mixed)
        float metalPDF = reflected ? glossyPDF : 0.0f;
        pdf = mix(pdf, metalPDF, metallic);
        return pdf;
    }

    public BSDFSample Sample_f(float3 wo, float uc, float2 u, TransportMode mode, BxDFReflTransFlags flags) {
        float3 wi;
        BxDFFlags sampledFlag;

        float sampleGlossy = reflectance(dielF0, dielF90, SampleGGXLutE(AbsCosTheta(wo), roughness));
        // Select rescales uc
        if (Select(uc, metallic) || Select(uc, sampleGlossy)) {
            // Reflection path
            // Glossy (dielectric/metal) Sample
            float3 wm = mfDistrib.Sample_wm(wo, u);
            wi = Reflect(wo, wm);
            sampledFlag = BxDFFlags::GlossyReflection;
        } else {
            // Transmission path
            if (Select(uc, transmission)) {
                // Transmittance Sample
                float3 wm = mfDistrib.Sample_wm(wo, u);
                bool tir;// Total internal reflection
                // Drop the sample, or we'll break the PDF since it's
                // not expected to have a reflection ray sampled here.                
                wi = Refract(wo, wm, eta, tir);
                if (tir) {
                    wi = Reflect(wo, wm);
                    sampledFlag = BxDFFlags::GlossyReflection;
                } else
                    sampledFlag = BxDFFlags::Transmission;
            } else {
                // Diffuse Sample
                wi = SampleCosineHemisphere(u);
                wi = FaceForward(wi, float3(0,0,1));
                sampledFlag = BxDFFlags::DiffuseReflection;
            }
        }
        // Absorption
        if (!(SameHemisphere(wo, wi) ^ IsTransmission(sampledFlag)))
            return BSDFSample();
        SampledSpectrum val = this.f(wo, wi, mode);
        float pdf = this.PDF(wo, wi, mode, flags);
        if (IsTransmission(sampledFlag)){
            printf("--debug T PDF:%f wi:%f,%f,%f wo:%f,%f,%f\n", pdf, wi.x, wi.y, wi.z, wo.x, wo.y, wo.z);
        }
        return BSDFSample(val, wi, pdf, sampledFlag, eta);
    }
};
