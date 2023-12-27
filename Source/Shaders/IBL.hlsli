#ifndef IBL
#define IBL
#include "BRDF.hlsli"
// A port of https://github.com/KhronosGroup/glTF-IBL-Sampler/blob/master/lib/source/shaders/filter.frag
// Some nice to reads:
// * https://bruop.github.io/ibl/
// * https://github.com/KhronosGroup/glTF-Sample-Viewer/blob/main/source/Renderer/shaders/ibl.glsl
// * https://github.com/EpicGames/UnrealEngine/blob/release/Engine/Shaders/Private/BRDF.ush
// * https://github.com/KhronosGroup/glTF-IBL-Sampler/blob/master/lib/source/shaders/filter.frag
// * https://www.jcgt.org/published/0008/01/03/paper.pdf

// Hammersley Points on the Hemisphere
// CC BY 3.0 (Holger Dammertz)
// http://holger.dammertz.org/stuff/notes_HammersleyOnHemisphere.html
// with adapted interface
/* SAMPLING */
#ifdef IBL_SAMPLER
float radicalInverse_VdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10; // / 0x100000000
}

// hammersley2d describes a sequence of points in the 2d unit square [0,1)^2
// that can be used for quasi Monte Carlo integration
float2 hammersley2d(int i, int N)
{
    return float2(float(i) / float(N), radicalInverse_VdC(uint(i)));
}

// Hemisphere Sample

// TBN generates a tangent bitangent normal coordinate frame from the normal
// (the normal must be normalized)
float3x3 generateTBN(float3 normal)
{
    float3 bitangent = float3(0.0, 1.0, 0.0);

    float NdotUp = dot(normal, float3(0.0, 1.0, 0.0));
    float epsilon = 0.0000001;
    if (1.0 - abs(NdotUp) <= epsilon)
    {
        // Sampling +Y or -Y, so we need a more robust bitangent.
        if (NdotUp > 0.0)
        {
            bitangent = float3(0.0, 0.0, 1.0);
        }
        else
        {
            bitangent = float3(0.0, 0.0, -1.0);
        }
    }

    float3 tangent = normalize(cross(bitangent, normal));
    bitangent = cross(normal, tangent);

    return float3x3(tangent, bitangent, normal);
}

struct MicrofacetDistributionSample
{
    float pdf;
    float cosTheta;
    float sinTheta;
    float phi;
};
// GGX microfacet distribution
// https://www.cs.cornell.edu/~srm/publications/EGSR07-btdf.html
// This implementation is based on https://bruop.github.io/ibl/,
//  https://www.tobias-franke.eu/log/2014/03/30/notes_on_importance_sampling.html
// and https://developer.nvidia.com/gpugems/GPUGems3/gpugems3_ch20.html
MicrofacetDistributionSample GGX(float2 xi, float roughness)
{
    MicrofacetDistributionSample ggx;

    // evaluate sampling equations
    float alpha = roughness * roughness;
    ggx.cosTheta = saturate(sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y)));
    ggx.sinTheta = sqrt(1.0 - ggx.cosTheta * ggx.cosTheta);
    ggx.phi = 2.0 * M_PI * xi.x;

    // evaluate GGX pdf (for half floattor)
    ggx.pdf = D_GGX(ggx.cosTheta, alpha);

    // Apply the Jacobian to obtain a pdf that is parameterized by l
    // see https://bruop.github.io/ibl/
    // Typically you'd have the following:
    // float pdf = D_GGX(NoH, roughness) * NoH / (4.0 * VoH);
    // but since V = N => VoH == NoH
    ggx.pdf /= 4.0;

    return ggx;
}

MicrofacetDistributionSample Charlie(float2 xi, float roughness)
{
    MicrofacetDistributionSample charlie;

    float alpha = roughness * roughness;
    charlie.sinTheta = pow(xi.y, alpha / (2.0 * alpha + 1.0));
    charlie.cosTheta = sqrt(1.0 - charlie.sinTheta * charlie.sinTheta);
    charlie.phi = 2.0 * M_PI * xi.x;

    // evaluate Charlie pdf (for half floattor)
    charlie.pdf = D_Charlie(alpha, charlie.cosTheta);

    // Apply the Jacobian to obtain a pdf that is parameterized by l
    charlie.pdf /= 4.0;

    return charlie;
}

MicrofacetDistributionSample Lambertian(float2 xi, float roughness)
{
    MicrofacetDistributionSample lambertian;

    // Cosine weighted hemisphere sampling
    // http://www.pbr-book.org/3ed-2018/Monte_Carlo_Integration/2D_Sampling_with_Multidimensional_Transformations.html#Cosine-WeightedHemisphereSampling
    lambertian.cosTheta = sqrt(1.0 - xi.y);
    lambertian.sinTheta = sqrt(xi.y); // equivalent to `sqrt(1.0 - cosTheta*cosTheta)`;
    lambertian.phi = 2.0 * M_PI * xi.x;

    lambertian.pdf = lambertian.cosTheta / M_PI; // evaluation for solid angle, therefore drop the sinTheta

    return lambertian;
}

#define USE_LAMBERTIAN 0 // pdf = 1/pi
#define USE_GGX 1
#define USE_CHARLIE 2
// getImportanceSample returns an importance sample direction with pdf in the .w component
float4 getImportanceSample(int distribution, int sampleIndex, int sampleCount, float3 N, float roughness)
{
    // generate a quasi monte carlo point in the unit square [0.1)^2
    float2 xi = hammersley2d(sampleIndex, sampleCount);

    MicrofacetDistributionSample importanceSample;

    // generate the points on the hemisphere with a fitting mapping for
    // the distribution (e.g. lambertian uses a cosine importance)
    if (distribution == USE_LAMBERTIAN)
    {
        importanceSample = Lambertian(xi, roughness);
    }
    else if (distribution == USE_GGX)
    {
        // Trowbridge-Reitz / GGX microfacet model (Walter et al)
        // https://www.cs.cornell.edu/~srm/publications/EGSR07-btdf.html
        importanceSample = GGX(xi, roughness);
    }
    else if (distribution == USE_CHARLIE)
    {
        importanceSample = Charlie(xi, roughness);
    }

    // transform the hemisphere sample to the normal coordinate frame
    // i.e. rotate the hemisphere to the normal direction
    float3 localSpaceDirection = normalize(float3(
        importanceSample.sinTheta * cos(importanceSample.phi),
        importanceSample.sinTheta * sin(importanceSample.phi),
        importanceSample.cosTheta
    ));
    float3x3 TBN = generateTBN(N);
    float3 direction = mul(localSpaceDirection,TBN);

    return float4(direction, importanceSample.pdf);
}

// Mipmap Filtered Samples (GPU Gems 3, 20.4)
// https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-20-gpu-based-importance-sampling
// https://cgg.mff.cuni.cz/~jaroslav/papers/2007-sketch-fis/Final_sap_0073.pdf
float computeLod(float pdf, uint dimension, uint sampleCount)
{
    // // Solid angle of current sample -- bigger for less likely samples
    // float omegaS = 1.0 / (float(sampleCount) * pdf);
    // // Solid angle of texel
    // // note: the factor of 4.0 * UX3D_MATH_PI 
    // float omegaP = 4.0 * M_PI / (6.0 * float(dimension) * float(dimension));
    // // Mip level is determined by the ratio of our sample's solid angle to a texel's solid angle 
    // // note that 0.5 * log2 is equivalent to log4
    // float lod = 0.5 * log2(omegaS / omegaP);

    // babylon introduces a factor of K (=4) to the solid angle ratio
    // this helps to avoid undersampling the environment map
    // this does not appear in the original formulation by Jaroslav Krivanek and Mark Colbert
    // log4(4) == 1
    // lod += 1.0;

    // We achieved good results by using the original formulation from Krivanek & Colbert adapted to cubemaps

    // https://cgg.mff.cuni.cz/~jaroslav/papers/2007-sketch-fis/Final_sap_0073.pdf
    float lod = 0.5 * log2(6.0 * float(dimension) * float(dimension) / (float(sampleCount) * pdf));


    return lod;
}

float3 LUT(int distribution, float NdotV, float roughness, uint sampleCount)
{
    // Compute spherical view floattor: (sin(phi), 0, cos(phi))
    float3 V = float3(sqrt(1.0 - NdotV * NdotV), 0.0, NdotV);

    // The macro surface normal just points up.
    float3 N = float3(0.0, 0.0, 1.0);

    // To make the LUT independant from the material's F0, which is part of the Fresnel term
    // when substituted by Schlick's approximation, we factor it out of the integral,
    // yielding to the form: F0 * I1 + I2
    // I1 and I2 are slighlty different in the Fresnel term, but both only depend on
    // NoL and roughness, so they are both numerically integrated and written into two channels.
    float A = 0.0;
    float B = 0.0;
    float C = 0.0;

    for (int i = 0; i < sampleCount; ++i)
    {
        // Importance sampling, depending on the distribution.
        float4 importanceSample = getImportanceSample(distribution, i, sampleCount, N, roughness);
        float3 H = importanceSample.xyz;
        // float pdf = importanceSample.w;
        float3 L = normalize(reflect(-V, H));

        float NdotL = saturate(L.z);
        float NdotH = saturate(H.z);
        float VdotH = saturate(dot(V, H));
        if (NdotL > 0.0)
        {
            if (distribution == USE_GGX)
            {
                // LUT for GGX distribution.

                // Taken from: https://bruop.github.io/ibl
                // Shadertoy: https://www.shadertoy.com/view/3lXXDB
                // Terms besides V are from the GGX PDF we're dividing by.
                float V_pdf = Vis_GGX_SmithCorrelated(NdotV, NdotL, roughness) * VdotH * NdotL / NdotH;
                float Fc = pow(1.0 - VdotH, 5.0);
                A += (1.0 - Fc) * V_pdf;
                B += Fc * V_pdf;
                C += 0.0;
            }

            if (distribution == USE_CHARLIE)
            {
                // LUT for Charlie distribution.
                float sheenDistribution = D_Charlie(roughness, NdotH);
                float sheenVisibility = Vis_Ashikhmin(NdotL, NdotV);

                A += 0.0;
                B += 0.0;
                C += sheenVisibility * sheenDistribution * NdotL * VdotH;
            }
        }
    }

    // The PDF is simply pdf(v, h) -> NDF * <nh>.
    // To parametrize the PDF over l, use the Jacobian transform, yielding to: pdf(v, l) -> NDF * <nh> / 4<vh>
    // Since the BRDF divide through the PDF to be normalized, the 4 can be pulled out of the integral.
    return float3(4.0 * A, 4.0 * B, 4.0 * 2.0 * M_PI * C) / float(sampleCount);
}
#endif
/* SHADING */
#ifdef IBL_SHADING
float3 getIBLRadianceGGX(SceneIBLProbe probe, float3 n, float3 v, float roughness, float3 F0, float specularWeight)
{
    float NdotV = saturate(dot(n, v));
    float lod = roughness * float(probe.mips - 1);
    float3 reflection = normalize(reflect(-v, n));
    
    float2 f_ab = getLUT(probe, saturate(NdotV), saturate(roughness)).rg;
    float4 specularSample = getSpecularSample(probe, reflection, lod);

    float3 specularLight = specularSample.rgb;

    // see https://bruop.github.io/ibl/#single_scattering_results at Single Scattering Results
    // Roughness dependent fresnel, from Fdez-Aguera
    float3 Fr = max(splat3(1.0 - roughness), F0) - F0;
    float3 k_S = F0 + Fr * pow(1.0 - NdotV, 5.0);
    float3 FssEss = k_S * f_ab.x + f_ab.y;

    return specularWeight * specularLight * FssEss;
}
float3 getIBLRadianceLambertian(SceneIBLProbe probe, float3 n, float3 v, float roughness, float3 diffuseColor, float3 F0, float specularWeight)
{
    float NdotV = saturate(dot(n, v));
    float2 f_ab = getLUT(probe, saturate(NdotV), saturate(roughness)).rg;

    float3 irradiance = getDiffuseLight(probe, n).rgb;

    // see https://bruop.github.io/ibl/#single_scattering_results at Single Scattering Results
    // Roughness dependent fresnel, from Fdez-Aguera

    float3 Fr = max(splat3(1.0 - roughness), F0) - F0;
    float3 k_S = F0 + Fr * pow(1.0 - NdotV, 5.0);
    float3 FssEss = specularWeight * k_S * f_ab.x + f_ab.y; // <--- GGX / specular light contribution (scale it down if the specularWeight is low)

    // Multiple scattering, from Fdez-Aguera
    float Ems = (1.0 - (f_ab.x + f_ab.y));
    float3 F_avg = specularWeight * (F0 + (1.0 - F0) / 21.0);
    float3 FmsEms = Ems * FssEss * F_avg / (1.0 - F_avg * Ems);
    float3 k_D = diffuseColor * (1.0 - FssEss + FmsEms); // we use +FmsEms as indicated by the formula in the blog post (might be a typo in the implementation)

    return (FmsEms + k_D) * irradiance;
}
#endif
#endif