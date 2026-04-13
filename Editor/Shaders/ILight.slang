/**
* PBRTv4-style interface for physically modeled light sources
*/
import IMath;
import IBxDF;
public enum class LightType {
    DeltaPosition,  // Point light
    DeltaDirection, // Directional light
    Area,           // Surface light with a geometric shape
    Infinite        // Ambient light for rays that left the scene
};
bool IsDeltaLight(LightType type) {
    return (type == LightType::DeltaPosition ||
            type == LightType::DeltaDirection);
}
public struct LightSampleContext {
    public float3 pi; // Surface interaction point
    public float3 n;  // Geometric normal at pi
    public float3 ns; // Shading normal at pi
    public __init(float3 pi, float3 n, float3 ns) {
        this.pi = pi;
        this.n = n;
        this.ns = ns;
    }
    public float3 p() { return pi; }
};
public struct LightLiSample {
    public SampledSpectrum Li; // Light leaving the source towards pi
    public float3 wi;          // Direction from pi to light source
    public float pdf;          // PDF for this sample
    public bool isDelta;
    public float dist;         // Distance from shading point to sampled light point (INF for infinite/distant)
    public __init(SampledSpectrum Li = 0, float3 wi = 0, float pdf = 0, bool isDelta = false, float dist = INF) {
        this.Li = Li;
        this.wi = wi;
        this.pdf = pdf;
        this.isDelta = isDelta;
        this.dist = dist;
    }
};
// The light interface
// There's _no_ spectrum sampling here - all spectra are fully sampled RGB for simplicity
public interface ILight {
    // Type of this light source
    public LightType Type();
    // Area light interaction - only defined for lights with geo (Area)
    public SampledSpectrum L(float3 p, float3 n, float3 uv, float3 w);
    // Infinite/environment light interaction - only defined for ambient lights (Infinite)
    public SampledSpectrum Le(float3 w);
    // PDF for Le - only defined for ambient lights (Infinite)
    public void PDF_Le(LightSampleContext ctx, float3 wi, out float pdfPos, out float pdfDir);
    // PDF for Li sampling - only defined for ones that make sense to be sampled
    public float PDF_Li(LightSampleContext ctx, float3 wi, bool allowIncompletePDF = false);
    // Sampled light
    public LightLiSample Sample_Li(LightSampleContext ctx, float2 u);
};
// -- Lights

// Module-level env map resources — bound by the integrator.
// When no env map is loaded a 1×1 black fallback is bound so sampling is always safe.
public Texture2D<float4> envMapTexture;
public SamplerState        envMapSampler;

// Direction → equirectangular UV (azimuth-elevation mapping)
// φ = atan2(d.z, d.x) ∈ [-π,π],  θ = acos(d.y) ∈ [0,π]
float2 EnvMapDirToUV(float3 d) {
    float phi   = atan2(d.z, d.x);
    float theta = acos(clamp(d.y, -1, 1));
    return float2(0.5f + phi * (0.5f * InvPi), theta * InvPi);
}

float3 SampleEnvMapTexture(float3 dir) {
    return envMapTexture.SampleLevel(envMapSampler, EnvMapDirToUV(dir), 0).xyz;
}

// https://pbr-book.org/4ed/Light_Sources/Infinite_Area_Lights
// Unified infinite light: when hasEnvMap is true, radiance comes from the
// equirectangular env map texture; otherwise it falls back to a constant color.
// Sampling is always uniform-sphere (importance sampling the map is a TODO).
public struct InfiniteLight : ILight {
    SampledSpectrum fallbackColor; // constant Lemit when no env map
    float scale;
    bool  hasEnvMap;

    public __init(SampledSpectrum fallbackColor, float scale, bool hasEnvMap = false) {
        this.fallbackColor = fallbackColor;
        this.scale         = scale;
        this.hasEnvMap     = hasEnvMap;
    }

    public LightType Type() {
        return LightType::Infinite;
    }
    // Not defined for infinite lights
    public SampledSpectrum L(float3 p, float3 n, float3 uv, float3 w) {
        return {};
    }
    public SampledSpectrum Le(float3 w) {
        if (hasEnvMap)
            return SampledSpectrum(SampleEnvMapTexture(w)) * scale;
        return fallbackColor * scale;
    }
    public void PDF_Le(LightSampleContext, float3, out float pdfPos, out float pdfDir) {
        pdfPos = 0.0f;
        pdfDir = UniformSpherePDF();
    }
    public LightLiSample Sample_Li(LightSampleContext ctx, float2 u) {
        float3 wi = SampleUniformSphere(u);
        return LightLiSample(Le(wi), wi, UniformSpherePDF(), false, INF);
    }
    public float PDF_Li(LightSampleContext ctx, float3 w, bool allowIncompletePDF) {
        return UniformSpherePDF();
    }
};
// https://pbr-book.org/4ed/Light_Sources/Distant_Lights#DistantLight
public struct DistantLight : ILight {
    SampledSpectrum Lemit;
    float3 direction;
    public __init(SampledSpectrum Lemit, float3 direction){
        this.Lemit = Lemit;
        this.direction = normalize(direction);
    }
    public LightType Type() {
        return LightType::DeltaDirection;
    }
    // Not defined
    public SampledSpectrum L(float3 p, float3 n, float3 uv, float3 w) {
        return {};
    }
    // Not defined
    public SampledSpectrum Le(float3 w) {
        return {};
    }
    // Not defined
    public void PDF_Le(LightSampleContext, float3, out float pdfPos, out float pdfDir){
        pdfPos = 0.0f;
        pdfDir = 0.0f; // Dirac
    }
    // Dirac
    public float PDF_Li(LightSampleContext, float3, bool allowIncompletePDF) {
        return 0.0f;
    }
    public LightLiSample Sample_Li(LightSampleContext ctx, float2 u) {
        return LightLiSample(Lemit, -direction, 1.0f, true, INF);
    }
};

public struct PointLight : ILight {
    SampledSpectrum I;
    float3 position;

    public __init(SampledSpectrum I, float3 position) {
        this.I = I;
        this.position = position;
    }

    public LightType Type() {
        return LightType::DeltaPosition;
    }

    public SampledSpectrum L(float3 p, float3 n, float3 uv, float3 w) {
        return {};
    }

    public SampledSpectrum Le(float3 w) {
        return {};
    }

    public void PDF_Le(LightSampleContext ctx, float3 wi, out float pdfPos, out float pdfDir) {
        pdfPos = 0.0f;
        pdfDir = 0.0f;
    }

    public float PDF_Li(LightSampleContext ctx, float3 wi, bool allowIncompletePDF) {
        return 0.0f;
    }

    public LightLiSample Sample_Li(LightSampleContext ctx, float2 u) {
        float3 wi = position - ctx.p();
        float dist2 = dot(wi, wi);
        if (dist2 == 0.0f) return LightLiSample();
        
        float dist = sqrt(dist2);
        wi /= dist;
        
        SampledSpectrum Li = I / dist2;
        return LightLiSample(Li, wi, 1.0f, true, dist);
    }
};

public struct SpotLight : ILight {
    SampledSpectrum I;
    float3 position;
    float3 direction;
    float cosTotalWidth;
    float cosFalloffStart;

    public __init(SampledSpectrum I, float3 position, float3 direction, float totalWidth, float falloffStart) {
        this.I = I;
        this.position = position;
        this.direction = normalize(direction);
        this.cosTotalWidth = cos(totalWidth);
        this.cosFalloffStart = cos(falloffStart);
    }

    public LightType Type() {
        return LightType::DeltaPosition;
    }

    public SampledSpectrum L(float3 p, float3 n, float3 uv, float3 w) {
        return {};
    }

    public SampledSpectrum Le(float3 w) {
        return {};
    }

    public void PDF_Le(LightSampleContext ctx, float3 wi, out float pdfPos, out float pdfDir) {
        pdfPos = 0.0f;
        pdfDir = 0.0f;
    }

    public float PDF_Li(LightSampleContext ctx, float3 wi, bool allowIncompletePDF) {
        return 0.0f;
    }

    float Falloff(float3 w) {
        float cosTheta = dot(direction, w);
        if (cosTheta < cosTotalWidth) return 0.0f;
        if (cosTheta > cosFalloffStart) return 1.0f;
        float delta = (cosTheta - cosTotalWidth) / (cosFalloffStart - cosTotalWidth);
        return (delta * delta) * (delta * delta);
    }

    public LightLiSample Sample_Li(LightSampleContext ctx, float2 u) {
        float3 wi = position - ctx.p();
        float dist2 = dot(wi, wi);
        if (dist2 == 0.0f) return LightLiSample();
        
        float dist = sqrt(dist2);
        wi /= dist;
        
        float falloff = Falloff(-wi);
        if (falloff == 0.0f) return LightLiSample();

        SampledSpectrum Li = I * falloff / dist2;
        return LightLiSample(Li, wi, 1.0f, true, dist);
    }
};

// https://pbr-book.org/4ed/Light_Sources/Area_Lights
public struct DiskLight : ILight {
    SampledSpectrum Lemit;
    float3 position;
    float3 normal;
    float3 dpdu;
    float3 dpdv;
    float radius;
    float area;
    bool twoSided;

    public __init(SampledSpectrum Lemit, float3 position, float3 normal, float3 dpdu, float radius, bool twoSided = false) {
        this.Lemit = Lemit;
        this.position = position;
        this.normal = normalize(normal);
        this.dpdu = normalize(dpdu);
        this.dpdv = cross(this.normal, this.dpdu);
        this.radius = radius;
        this.area = Pi * radius * radius;
        this.twoSided = twoSided;
    }

    public __init(SampledSpectrum Lemit, float3 position, float3 normal, float radius, bool twoSided = false) {
        this.Lemit = Lemit;
        this.position = position;
        this.normal = normalize(normal);
        buildOrthonormalBasis(this.normal, this.dpdu, this.dpdv);
        this.radius = radius;
        this.area = Pi * radius * radius;
        this.twoSided = twoSided;
    }

    public LightType Type() {
        return LightType::Area;
    }

    public SampledSpectrum L(float3 p, float3 n, float3 uv, float3 w) {
        if (!twoSided && dot(n, w) < 0.0f) return SampledSpectrum(0.0f);
        return Lemit;
    }

    public SampledSpectrum Le(float3 w) {
        return SampledSpectrum(0.0f);
    }

    public void PDF_Le(LightSampleContext ctx, float3 wi, out float pdfPos, out float pdfDir) {
        pdfPos = 0.0f;
        pdfDir = 0.0f;
    }

    public float PDF_Li(LightSampleContext ctx, float3 wi, bool allowIncompletePDF) {
        float cosTheta = dot(normal, wi);
        if (cosTheta == 0.0f) return 0.0f;
        
        // wi is direction from shading point to light.
        // Light normal should face opposite to wi.
        if (!twoSided && cosTheta > 0.0f) return 0.0f;

        // Ray-plane intersection
        float t = dot(normal, position - ctx.p()) / cosTheta;
        if (t <= 0.0f) return 0.0f;

        float3 pDisk = ctx.p() + t * wi;
        float dist2 = dot(pDisk - position, pDisk - position);
        if (dist2 > radius * radius) return 0.0f;

        float pdfArea = 1.0f / area;
        return pdfArea * (t * t) / abs(cosTheta);
    }

    public LightLiSample Sample_Li(LightSampleContext ctx, float2 u) {
        float2 pd = SampleConcentricDisk(u) * radius;
        float3 pDisk = position + pd.x * dpdu + pd.y * dpdv;
        
        float3 wi = pDisk - ctx.p();
        float dist2 = dot(wi, wi);
        if (dist2 == 0.0f) return LightLiSample();
        
        float dist = sqrt(dist2);
        wi /= dist;

        float cosTheta = dot(normal, -wi);
        if (!twoSided && cosTheta <= 0.0f) return LightLiSample();

        // Solid angle PDF
        float pdf = dist2 / (area * abs(cosTheta));
        return LightLiSample(Lemit, wi, pdf, false, dist);
    }
};

public struct RectLight : ILight {
    SampledSpectrum Lemit;
    float3 position;
    float3 normal;
    float3 dpdu;
    float3 dpdv;
    float area;
    bool twoSided;

    public __init(SampledSpectrum Lemit, float3 position, float3 dpdu, float3 dpdv, bool twoSided = false) {
        this.Lemit = Lemit;
        this.position = position;
        this.dpdu = dpdu;
        this.dpdv = dpdv;
        float3 crossProd = cross(dpdu, dpdv);
        this.area = length(crossProd) * 4.0f;
        this.normal = normalize(crossProd);
        this.twoSided = twoSided;
    }

    public LightType Type() {
        return LightType::Area;
    }

    public SampledSpectrum L(float3 p, float3 n, float3 uv, float3 w) {
        if (!twoSided && dot(n, w) < 0.0f) return SampledSpectrum(0.0f);
        return Lemit;
    }

    public SampledSpectrum Le(float3 w) {
        return SampledSpectrum(0.0f);
    }

    public void PDF_Le(LightSampleContext ctx, float3 wi, out float pdfPos, out float pdfDir) {
        pdfPos = 0.0f;
        pdfDir = 0.0f;
    }

    public float PDF_Li(LightSampleContext ctx, float3 wi, bool allowIncompletePDF) {
        float cosTheta = dot(normal, wi);
        if (cosTheta == 0.0f) return 0.0f;
        
        if (!twoSided && cosTheta > 0.0f) return 0.0f;

        // Ray-plane intersection
        float t = dot(normal, position - ctx.p()) / cosTheta;
        if (t <= 0.0f) return 0.0f;

        float3 pRect = ctx.p() + t * wi;
        float3 d = pRect - position;
        
        float l2_u = dot(dpdu, dpdu);
        float l2_v = dot(dpdv, dpdv);
        float u = dot(d, dpdu) / l2_u;
        float v = dot(d, dpdv) / l2_v;

        if (abs(u) > 1.0f || abs(v) > 1.0f) return 0.0f;

        float pdfArea = 1.0f / area;
        return pdfArea * (t * t) / abs(cosTheta);
    }

    public LightLiSample Sample_Li(LightSampleContext ctx, float2 u) {
        float2 pd = u * 2.0f - 1.0f;
        float3 pRect = position + pd.x * dpdu + pd.y * dpdv;
        
        float3 wi = pRect - ctx.p();
        float dist2 = dot(wi, wi);
        if (dist2 == 0.0f) return LightLiSample();
        
        float dist = sqrt(dist2);
        wi /= dist;

        float cosTheta = dot(normal, -wi);
        if (!twoSided && cosTheta <= 0.0f) return LightLiSample();

        // Solid angle PDF
        float pdf = dist2 / (area * abs(cosTheta));
        return LightLiSample(Lemit, wi, pdf, false, dist);
    }
};
