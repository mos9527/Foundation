import IBSDF;
import IBSSRDF;
import IBxDF;
import IMath;

public interface IMaterial {
    public IBSDF GetSurface();
    public IBSSRDF GetSubsurface();
    public float ProbSampleSurface();
    public float ProbSampleSubsurface();
};

public struct PrincipledMaterial : IMaterial {
    PrincipledBSDF surface;
    DisneyBurleyBSSRDF subsurface;
    float pSurface;
    float pSubsurface;

    public __init(
        float3 baseColor,
        float metallic,
        float roughness,
        float ior,
        float specularFactor = 1.0f,
        float transmission = 0.0f,
        bool energyCompensation = true,
        float3 wo = float3(0.0f, 0.0f, 1.0f),
        float subsurfaceWeight = 0.0f,
        float3 subsurfaceColor = float3(1.0f),
        float3 subsurfaceRadius = float3(1.0f, 0.2f, 0.1f),
        float subsurfaceScale = 0.05f,
        float anisotropyStrength = 0.0f,
        float3 specularColorFactor = float3(1.0f))
    {
        bool hasSubsurfaceProfile = MaxComponent(subsurfaceRadius * subsurfaceScale) > EPS &&
                                    MaxComponent(baseColor * subsurfaceColor) > EPS;
        float activeSubsurface = hasSubsurfaceProfile ? clamp(subsurfaceWeight, 0.0f, 1.0f) : 0.0f;
        float diffuseWeight = 1.0f - activeSubsurface;

        roughness = max(8e-3f, roughness);
        ior = max(1.0f + 5e-3f, ior);

        this.surface = PrincipledBSDF(
            baseColor, metallic, roughness, ior, specularFactor, transmission,
            energyCompensation, wo, diffuseWeight, anisotropyStrength, specularColorFactor);
        this.subsurface = DisneyBurleyBSSRDF(baseColor * subsurfaceColor, subsurfaceRadius * subsurfaceScale);

        // Weigh surface/subsurface events by energy
        float3 dielF0, dielF90;
        CalcGlossyFresnel(ior, specularFactor, specularColorFactor, dielF0, dielF90);
        float sampleGlossy = MaxComponent(reflectance(dielF0, dielF90, SampleGGXLutE(AbsCosTheta(wo), roughness)));

        float rawDiffuseFamily = (1.0f - metallic) * (1.0f - sampleGlossy) * (1.0f - transmission);
        float rawMetal = metallic;
        float rawDielSpec = (1.0f - metallic) * sampleGlossy;
        float rawTrans = (1.0f - metallic) * (1.0f - sampleGlossy) * transmission;
        float rawSubsurface = rawDiffuseFamily * activeSubsurface;
        float eventWeightSum = rawDiffuseFamily + rawTrans + rawDielSpec + rawMetal;

        this.pSubsurface = clamp(eventWeightSum > 0.0f ? rawSubsurface / eventWeightSum : 0.0f, 0.0f, 1.0f);
        this.pSurface = 1.0f - this.pSubsurface;
    }

    public IBSDF GetSurface() {
        return surface;
    }

    public IBSSRDF GetSubsurface() {
        return subsurface;
    }

    public float ProbSampleSurface() {
        return pSurface;
    }

    public float ProbSampleSubsurface() {
        return pSubsurface;
    }
};

public struct HairMaterial : IMaterial {
    HairBSDF surface;
    StubBSSRDF subsurface;

    public __init(
        float3 baseColor,
        float roughness,
        float specularFactor = 1.0f,
        float3 specularColorFactor = float3(1.0f))
    {
        this.surface = HairBSDF(baseColor, roughness, specularFactor, specularColorFactor);
        this.subsurface = StubBSSRDF();
    }

    public IBSDF GetSurface() {
        return surface;
    }

    public IBSSRDF GetSubsurface() {
        return subsurface;
    }

    public float ProbSampleSurface() {
        return 1.0f;
    }

    public float ProbSampleSubsurface() {
        return 0.0f;
    }
};
