/**
* PBRTv4-style interface for physically modeled light sources
*/
import IMath;
import IBxDF;
import IBindless2D;
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
    public bool useShadow;
    public __init(SampledSpectrum Li = 0, float3 wi = 0, float pdf = 0, bool isDelta = false, float dist = INF, bool useShadow = true) {
        this.Li = Li;
        this.wi = wi;
        this.pdf = pdf;
        this.isDelta = isDelta;
        this.dist = dist;
        this.useShadow = useShadow;
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

// Module-level env map resources. GPUScene binds tiny default resources when no HDRI is loaded.
public SamplerState envMapSampler;

// See also Render/Precompute.cpp
// Helper: Binary search on a 1D CDF texture
float LoadCDF(uint32_t cdfIndex, uint x, uint y)
{
    return textures[NonUniformResourceIndex(cdfIndex)].Load(int3(x, y, 0)).x;
}

uint FindInterval(uint32_t cdfIndex, uint baseIdx, uint numBins, float u) 
{
    uint first = 0;
    uint len = numBins;
    
    while (len > 0) {
        uint half_len = len >> 1;
        uint middle = first + half_len;
        
        if (LoadCDF(cdfIndex, baseIdx + middle, 0) <= u) {
            first = middle + 1;
            len -= half_len + 1;
        } else {
            len = half_len;
        }
    }
    
    return clamp(first, 0, numBins - 1);
}

// 1D Sampling Function
float Sample1D(uint32_t cdfIndex, uint baseIdx, uint numBins, float u, out float pdf, out uint offset) 
{
    offset = FindInterval(cdfIndex, baseIdx, numBins, u);
    
    float cdf0 = offset == 0 ? 0.0 : LoadCDF(cdfIndex, baseIdx + offset - 1, 0);
    float cdf1 = LoadCDF(cdfIndex, baseIdx + offset, 0);
    
    pdf = (cdf1 - cdf0) * numBins;
    
    float du = (cdf1 > cdf0) ? (u - cdf0) / (cdf1 - cdf0) : 0.0;
    
    return (offset + du) / numBins;
}

// 2D Sampling Function
float2 Sample2D(uint32_t marginalCDFIndex, uint sizeY, 
                uint32_t conditionalCDFIndex, uint sizeX, 
                float2 u, out float pdf, out uint2 offset) 
{
    float pdfY, pdfX;
    
    float v = Sample1D(marginalCDFIndex, 0, sizeY, u.y, pdfY, offset.y);
    
    // Binary search on the texture row
    uint first = 0;
    uint len = sizeX;
    
    while (len > 0) {
        uint half_len = len >> 1;
        uint middle = first + half_len;
        
        if (LoadCDF(conditionalCDFIndex, middle, offset.y) <= u.x) {
            first = middle + 1;
            len -= half_len + 1;
        } else {
            len = half_len;
        }
    }
    
    offset.x = clamp(first, 0, sizeX - 1);
    
    float cdfX0 = offset.x == 0 ? 0.0 : LoadCDF(conditionalCDFIndex, offset.x - 1, offset.y);
    float cdfX1 = LoadCDF(conditionalCDFIndex, offset.x, offset.y);
    
    pdfX = (cdfX1 - cdfX0) * sizeX;
    
    float du = (cdfX1 > cdfX0) ? (u.x - cdfX0) / (cdfX1 - cdfX0) : 0.0;
    float u_coord = (offset.x + du) / sizeX;
    
    pdf = pdfX * pdfY;
    
    return float2(u_coord, v);
}

float PDF2D(uint32_t marginalCDFIndex, uint sizeY, 
            uint32_t conditionalCDFIndex, uint sizeX, 
            float2 uv)
{
    uint2 offset = uint2(clamp(uint(uv.x * sizeX), 0, sizeX - 1), clamp(uint(uv.y * sizeY), 0, sizeY - 1));
    
    float cdfY0 = offset.y == 0 ? 0.0 : LoadCDF(marginalCDFIndex, offset.y - 1, 0);
    float cdfY1 = LoadCDF(marginalCDFIndex, offset.y, 0);
    float pdfY = (cdfY1 - cdfY0) * sizeY;
    
    float cdfX0 = offset.x == 0 ? 0.0 : LoadCDF(conditionalCDFIndex, offset.x - 1, offset.y);
    float cdfX1 = LoadCDF(conditionalCDFIndex, offset.x, offset.y);
    float pdfX = (cdfX1 - cdfX0) * sizeX;
    
    return pdfX * pdfY;
}

// https://pbr-book.org/4ed/Light_Sources/Infinite_Area_Lights
// https://projects.blender.org/blender/blender/src/branch/main/intern/cycles/kernel/light/background.h
// Infinite light backed either by a constant color or by the environment map resources.
public struct InfiniteLight : ILight {
    SampledSpectrum Lemit;
    bool hasEnvMap;
    float azimuthOffset; // For rotating the env map, in radians
    uint32_t envMapTextureIndex;
    uint32_t envMapMarginalCDFIndex;
    uint32_t envMapConditionalCDFIndex;

    public __init(SampledSpectrum Lemit, bool hasEnvMap = false, float azimuthOffset = 0.0f,
                  uint32_t envMapTextureIndex = ~0u, uint32_t envMapMarginalCDFIndex = ~0u,
                  uint32_t envMapConditionalCDFIndex = ~0u) {
        this.Lemit = Lemit;
        this.hasEnvMap = hasEnvMap;
        this.azimuthOffset = azimuthOffset / 180.0f * Pi; // radians
        this.envMapTextureIndex = envMapTextureIndex;
        this.envMapMarginalCDFIndex = envMapMarginalCDFIndex;
        this.envMapConditionalCDFIndex = envMapConditionalCDFIndex;
    }

    float3 SampleEnvMapTexture(float3 dir) {
        return textures[NonUniformResourceIndex(envMapTextureIndex)]
            .SampleLevel(envMapSampler, EnvMapDirToUV(dir, azimuthOffset), 0).xyz;
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
            return SampledSpectrum(SampleEnvMapTexture(w)) * Lemit;
        return Lemit;
    }
    public void PDF_Le(LightSampleContext, float3, out float pdfPos, out float pdfDir) {
        pdfPos = 0.0f;
        pdfDir = UniformSpherePDF();
    }
    public LightLiSample Sample_Li(LightSampleContext ctx, float2 u) {
        if (!hasEnvMap) {
            float3 wi = SampleUniformSphere(u);
            return LightLiSample(Le(wi), wi, UniformSpherePDF(), false, INF);
        }

        uint width, height;
        textures[NonUniformResourceIndex(envMapTextureIndex)].GetDimensions(width, height);
        float pdf;
        uint2 offset;
        float2 uv = Sample2D(envMapMarginalCDFIndex, height, envMapConditionalCDFIndex, width, u, pdf, offset);

        float3 wi = EnvMapUVToDir(uv, azimuthOffset);
        float theta = uv.y * Pi;
        float sinTheta = sin(theta);

        /**
        Jacobian change of var:
        phi = 2pi * u, theta = pi * v
        du = dphi/2pi, dv = dtheta/pi
        --
        p(phi, theta) dphi dtheta = p(u,v) du dv
        p(phi, theta) = p(u,v) / 2pi^2
        --
        PDFs are again measured w.r.t. solid angles. Recap..
        dω = sin(theta) dtheta dphi
        p(ω) dω = p(phi, theta) dphi dtheta
        p(ω) = p(phi, theta) / sin(theta) = p(u,v) / (2pi^2 * sin(theta))
        **/

        if (sinTheta == 0.0f) pdf = 0.0f;
        else pdf /= (2.0f * Pi * Pi * sinTheta);
        // ^^^ offset, no change in jacobian.
            
        if (pdf == 0.0f) return LightLiSample();
            
        return LightLiSample(Le(wi), wi, pdf, false, INF);
    }
    public float PDF_Li(LightSampleContext ctx, float3 w, bool allowIncompletePDF) {
        if (!hasEnvMap)
            return UniformSpherePDF();

        uint width, height;
        textures[NonUniformResourceIndex(envMapTextureIndex)].GetDimensions(width, height);
        float2 uv = EnvMapDirToUV(w, azimuthOffset);
        float pdf = PDF2D(envMapMarginalCDFIndex, height, envMapConditionalCDFIndex, width, uv);
            
        float theta = uv.y * Pi;
        float sinTheta = sin(theta);
            
        if (sinTheta == 0.0f) return 0.0f;
        return pdf / (2.0f * Pi * Pi * sinTheta);
    }
};
// https://pbr-book.org/4ed/Light_Sources/Distant_Lights#DistantLight
public struct DistantLight : ILight {
    SampledSpectrum LemitIrradiance;
    SampledSpectrum LemitRadiance;
    float3 direction;
    float cosThetaMax;
    float pdfDir;
    bool isDelta;

    public __init(SampledSpectrum Lemit, float3 direction, float angularDiameter = 0.0f) {
        this.LemitIrradiance = Lemit;
        this.direction = normalize(direction);

        if (angularDiameter <= 0.0f) {
            this.cosThetaMax = 1.0f;
            this.pdfDir = 0.0f;
            this.isDelta = true;
        } else {
            // https://en.wikipedia.org/wiki/Angular_diameter#Use_in_astronomy
            // Hello, penumbra and umbra
            this.isDelta = false;
            float angularRadius = clamp(angularDiameter * 0.5f, 1e-4f, Pi);
            this.cosThetaMax = cos(angularRadius);
            this.pdfDir = UniformConePDF(cosThetaMax);

            // We're no longer a delta light and actually has a defined PDF. So add it here. 
            this.LemitRadiance = LemitIrradiance * this.pdfDir;
        }
    }

    public LightType Type() {
        return isDelta ? LightType::DeltaDirection : LightType::Infinite;
    }

    public SampledSpectrum L(float3 p, float3 n, float3 uv, float3 w) {
        return {};
    }

    public SampledSpectrum Le(float3 w) {
        if (isDelta) return {};
        if (dot(w, -direction) >= cosThetaMax)
            return LemitRadiance;
        return {};
    }

    public void PDF_Le(LightSampleContext ctx, float3 wi, out float pdfPos, out float pdfDir) {
        pdfPos = 0.0f;
        pdfDir = 0.0f;
    }

    public float PDF_Li(LightSampleContext ctx, float3 wi, bool allowIncompletePDF) {
        if (isDelta) return 0.0f;
        if (dot(wi, -direction) >= cosThetaMax) {
            return pdfDir;
        }
        return 0.0f;
    }

    public LightLiSample Sample_Li(LightSampleContext ctx, float2 u) {
        if (isDelta) {
            return LightLiSample(LemitIrradiance, -direction, 1.0f, true, INF);
        }

        float3 wLocal = SampleUniformCone(cosThetaMax, u);
        float3 b1, b2;
        float3 dirToLight = -direction;
        CoordinateSystem(dirToLight, b1, b2);

        float3 wi = normalize(wLocal.x * b1 + wLocal.y * b2 + wLocal.z * dirToLight);
        return LightLiSample(LemitRadiance, wi, pdfDir, false, INF);
    }
};

bool IntersectSphereLight(float3 origin, float3 wi, float3 center, float radius,
                          out float distance, out float3 normal)
{
    float3 oc = origin - center;
    float b = dot(oc, wi);
    float c = dot(oc, oc) - radius * radius;
    float discriminant = b * b - c;
    if (discriminant <= 0.0f)
        return false;
    float root = sqrt(discriminant);
    distance = -b - root;
    if (distance <= 0.0f)
        distance = -b + root;
    if (distance <= 0.0f)
        return false;
    normal = (origin + distance * wi - center) / radius;
    return dot(normal, -wi) > 0.0f;
}

float VisibleSpherePDF(float radius, float distanceSquared, out float cosThetaMax)
{
    float sinThetaMaxSquared = clamp(radius * radius / distanceSquared, 0.0f, 1.0f);
    cosThetaMax = SafeSqrt(1.0f - sinThetaMaxSquared);
    float oneMinusCosThetaMax = sinThetaMaxSquared / max(1.0f + cosThetaMax, 1e-20f);
    return 1.0f / (2.0f * Pi * max(oneMinusCosThetaMax, 1e-20f));
}

bool SampleVisibleSphereLight(float3 origin, float3 center, float radius, float2 u,
                              out float3 wi, out float distance, out float pdf)
{
    float3 toCenter = center - origin;
    float distanceSquared = dot(toCenter, toCenter);
    if (distanceSquared <= radius * radius)
        return false;

    float cosThetaMax;
    pdf = VisibleSpherePDF(radius, distanceSquared, cosThetaMax);
    float3 localWi = SampleUniformCone(cosThetaMax, u);
    float3 axis = toCenter / sqrt(distanceSquared);
    float3 tangent, bitangent;
    CoordinateSystem(axis, tangent, bitangent);
    wi = normalize(localWi.x * tangent + localWi.y * bitangent + localWi.z * axis);

    float3 normal;
    return IntersectSphereLight(origin, wi, center, radius, distance, normal);
}

public struct PointLight : ILight {
    SampledSpectrum I;
    float3 position;
    float radius;

    public __init(SampledSpectrum I, float3 position, float radius = 0.0f) {
        this.I = I;
        this.position = position;
        this.radius = radius;
    }

    public LightType Type() {
        return radius > 0.0f ? LightType::Area : LightType::DeltaPosition;
    }

    public SampledSpectrum L(float3 p, float3 n, float3 uv, float3 w) {
        if (radius <= 0.0f || dot(n, w) <= 0.0f) return {};
        return I / (Pi * radius * radius);
    }

    public SampledSpectrum Le(float3 w) {
        return {};
    }

    public void PDF_Le(LightSampleContext ctx, float3 wi, out float pdfPos, out float pdfDir) {
        pdfPos = 0.0f;
        pdfDir = 0.0f;
    }

    public float PDF_Li(LightSampleContext ctx, float3 wi, bool allowIncompletePDF) {
        if (radius <= 0.0f) return 0.0f;

        float distanceSquared = dot(position - ctx.p(), position - ctx.p());
        if (distanceSquared <= radius * radius) return 0.0f;
        float distance;
        float3 normal;
        if (!IntersectSphereLight(ctx.p(), wi, position, radius, distance, normal)) return 0.0f;
        float cosThetaMax;
        return VisibleSpherePDF(radius, distanceSquared, cosThetaMax);
    }

    public LightLiSample Sample_Li(LightSampleContext ctx, float2 u) {
        bool isDelta = radius <= 0.0f;

        if (!isDelta) {
            float3 wi;
            float dist;
            float pdf;
            if (!SampleVisibleSphereLight(ctx.p(), position, radius, u, wi, dist, pdf))
                return LightLiSample();
            SampledSpectrum Li = I / (Pi * radius * radius);
            return LightLiSample(Li, wi, pdf, false, dist);
        }

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
    float radius;

    public __init(SampledSpectrum I, float3 position, float3 direction, float totalWidth, float falloffStart, float radius = 0.0f) {
        this.I = I;
        this.position = position;
        this.direction = normalize(direction);
        this.cosTotalWidth = cos(totalWidth);
        this.cosFalloffStart = cos(falloffStart);
        this.radius = radius;
    }

    public LightType Type() {
        return radius > 0.0f ? LightType::Area : LightType::DeltaPosition;
    }

    public SampledSpectrum L(float3 p, float3 n, float3 uv, float3 w) {
        if (radius <= 0.0f || dot(n, w) <= 0.0f) return {};
        return I * Falloff(w) / (Pi * radius * radius);
    }

    public SampledSpectrum Le(float3 w) {
        return {};
    }

    public void PDF_Le(LightSampleContext ctx, float3 wi, out float pdfPos, out float pdfDir) {
        pdfPos = 0.0f;
        pdfDir = 0.0f;
    }

    public float PDF_Li(LightSampleContext ctx, float3 wi, bool allowIncompletePDF) {
        if (radius <= 0.0f) return 0.0f;

        float distanceSquared = dot(position - ctx.p(), position - ctx.p());
        if (distanceSquared <= radius * radius) return 0.0f;
        float distance;
        float3 normal;
        if (!IntersectSphereLight(ctx.p(), wi, position, radius, distance, normal)) return 0.0f;
        float cosThetaMax;
        return VisibleSpherePDF(radius, distanceSquared, cosThetaMax);
    }

    float Falloff(float3 w) {
        float cosTheta = dot(direction, w);
        if (cosTheta < cosTotalWidth) return 0.0f;
        if (cosTheta > cosFalloffStart) return 1.0f;
        float delta = (cosTheta - cosTotalWidth) / (cosFalloffStart - cosTotalWidth);
        return (delta * delta) * (delta * delta);
    }

    public LightLiSample Sample_Li(LightSampleContext ctx, float2 u) {
        bool isDelta = radius <= 0.0f;

        if (!isDelta) {
            float3 wi;
            float dist;
            float pdf;
            if (!SampleVisibleSphereLight(ctx.p(), position, radius, u, wi, dist, pdf))
                return LightLiSample();
            float falloff = Falloff(-wi);
            if (falloff == 0.0f) return LightLiSample();

            SampledSpectrum Li = I * falloff / (Pi * radius * radius);
            return LightLiSample(Li, wi, pdf, false, dist);
        }

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
    float2 radius;
    float area;
    bool twoSided;

    public __init(SampledSpectrum Lemit, float3 position, float3 dpdu, float3 dpdv, float2 radius, bool twoSided = false) {
        this.Lemit = Lemit;
        this.position = position;
        this.dpdu = normalize(dpdu);
        this.dpdv = normalize(dpdv);
        this.normal = normalize(cross(this.dpdu, this.dpdv));
        this.radius = radius;
        this.area = Pi * radius.x * radius.y;
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
        if (area <= 0.0f || radius.x <= 0.0f || radius.y <= 0.0f) return 0.0f;
        float cosTheta = dot(normal, wi);
        if (cosTheta == 0.0f) return 0.0f;
        
        // wi is direction from shading point to light.
        // Light normal should face opposite to wi.
        if (!twoSided && cosTheta > 0.0f) return 0.0f;

        // Ray-plane intersection
        float t = dot(normal, position - ctx.p()) / cosTheta;
        if (t <= 0.0f) return 0.0f;

        float3 pDisk = ctx.p() + t * wi;
        float3 d = pDisk - position;
        float u = dot(d, dpdu) / radius.x;
        float v = dot(d, dpdv) / radius.y;
        if (u * u + v * v > 1.0f) return 0.0f;

        float pdfArea = 1.0f / area;
        return pdfArea * (t * t) / abs(cosTheta);
    }

    public LightLiSample Sample_Li(LightSampleContext ctx, float2 u) {
        if (area <= 0.0f || radius.x <= 0.0f || radius.y <= 0.0f) return LightLiSample();
        // Sample point on disk
        float2 pd = SampleConcentricDisk(u);
        float3 pLight = position + pd.x * radius.x * dpdu + pd.y * radius.y * dpdv;
        
        float3 wi = pLight - ctx.p();
        float dist2 = dot(wi, wi);
        if (dist2 == 0.0f) return LightLiSample();
        
        float dist = sqrt(dist2);
        wi /= dist;

        float cosTheta = dot(normal, -wi);
        if (abs(cosTheta) <= 1e-8f) return LightLiSample();
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
        if (area <= 0.0f) return 0.0f;
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
        if (l2_u <= 0.0f || l2_v <= 0.0f) return 0.0f;
        float u = dot(d, dpdu) / l2_u;
        float v = dot(d, dpdv) / l2_v;

        if (abs(u) > 1.0f || abs(v) > 1.0f) return 0.0f;

        float pdfArea = 1.0f / area;
        return pdfArea * (t * t) / abs(cosTheta);
    }

    public LightLiSample Sample_Li(LightSampleContext ctx, float2 u) {
        if (area <= 0.0f) return LightLiSample();
        float2 pd = u * 2.0f - 1.0f;
        float3 pRect = position + pd.x * dpdu + pd.y * dpdv;
        
        float3 wi = pRect - ctx.p();
        float dist2 = dot(wi, wi);
        if (dist2 == 0.0f) return LightLiSample();
        
        float dist = sqrt(dist2);
        wi /= dist;

        float cosTheta = dot(normal, -wi);
        if (abs(cosTheta) <= 1e-8f) return LightLiSample();
        if (!twoSided && cosTheta <= 0.0f) return LightLiSample();

        // Solid angle PDF
        float pdf = dist2 / (area * abs(cosTheta));
        return LightLiSample(Lemit, wi, pdf, false, dist);
    }
};

public struct TriangleLight : ILight {
    SampledSpectrum Lemit;
    float3 v0;
    float3 edge1;
    float3 edge2;
    float3 normal;
    float area;
    bool twoSided;

    public __init(SampledSpectrum Lemit, float3 v0, float3 edge1, float3 edge2, bool twoSided = false) {
        this.Lemit = Lemit;
        this.v0 = v0;
        this.edge1 = edge1;
        this.edge2 = edge2;
        float3 n = cross(edge1, edge2);
        float nLen = length(n);
        this.normal = nLen > 0.0f ? n / nLen : float3(0.0f);
        this.area = 0.5f * nLen;
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

    bool ContainsPoint(float3 p) {
        float3 d = p - v0;
        float d00 = dot(edge1, edge1);
        float d01 = dot(edge1, edge2);
        float d11 = dot(edge2, edge2);
        float d20 = dot(d, edge1);
        float d21 = dot(d, edge2);
        float denom = d00 * d11 - d01 * d01;
        if (abs(denom) <= 1e-12f) return false;

        float invDenom = 1.0f / denom;
        float b1 = (d11 * d20 - d01 * d21) * invDenom;
        float b2 = (d00 * d21 - d01 * d20) * invDenom;
        return b1 >= 0.0f && b2 >= 0.0f && (b1 + b2) <= 1.0f;
    }

    public float PDF_Li(LightSampleContext ctx, float3 wi, bool allowIncompletePDF) {
        if (area <= 0.0f) return 0.0f;

        float cosTheta = dot(normal, wi);
        if (cosTheta == 0.0f) return 0.0f;
        if (!twoSided && cosTheta > 0.0f) return 0.0f;

        float t = dot(normal, v0 - ctx.p()) / cosTheta;
        if (t <= 0.0f) return 0.0f;

        float3 pTriangle = ctx.p() + t * wi;
        if (!ContainsPoint(pTriangle)) return 0.0f;

        float pdfArea = 1.0f / area;
        return pdfArea * (t * t) / abs(cosTheta);
    }

    public LightLiSample Sample_Li(LightSampleContext ctx, float2 u) {
        if (area <= 0.0f) return LightLiSample();

        float sqrtU = sqrt(u.x);
        float b1 = sqrtU * (1.0f - u.y);
        float b2 = sqrtU * u.y;
        float3 pLight = v0 + b1 * edge1 + b2 * edge2;

        float3 wi = pLight - ctx.p();
        float dist2 = dot(wi, wi);
        if (dist2 == 0.0f) return LightLiSample();

        float dist = sqrt(dist2);
        wi /= dist;

        float cosTheta = dot(normal, -wi);
        if (abs(cosTheta) <= 1e-8f) return LightLiSample();
        if (!twoSided && cosTheta <= 0.0f) return LightLiSample();

        float pdf = dist2 / (area * abs(cosTheta));
        return LightLiSample(Lemit, wi, pdf, false, dist);
    }
};