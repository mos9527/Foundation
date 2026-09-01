// Progressive Path Tracer
// Reference tracer implementing all material models.
#include "IPathTracing.slang"
// Sobol sampler
StructuredBuffer<uint> gSobolMatrices;
// Adaptive sampling & accumalation
RWTexture2D<float4> adaptiveAux;
// "A Hierarchical Automatic Stopping Condition for Monte Carlo Global Illumination" Hanika 08
// Cycles does this: subsequent X-Y blur pass is used instead of actually going hierarchical...
void AccumulatePathRadiance(uint2 pix, PathRadiance radiance, float sampleCount)
{
    float4 outD = float4(radiance.diffuse, sampleCount);
    float4 outS = float4(radiance.specular, 1.0f);

    if (sampleCount == 1) {
        diffuse[pix] = outD;
        specular[pix] = outS;
    } else {
        float invN = 1.0f / sampleCount;
        float4 currentD = diffuse[pix];
        currentD.rgb += (outD.rgb - currentD.rgb) * invN;
        currentD.a = sampleCount;
        diffuse[pix] = currentD;
        specular[pix] += (outS - specular[pix]) * invN;
    }

    uint frameA = (uint(sampleCount) + 1) / 2;
    float3 totalRadiance = outD.rgb + outS.rgb;
    if (uint(sampleCount) % 2 == 1)
    {
        float3 currentA = adaptiveAux[pix].xyz;
        if (frameA == 1) {
            adaptiveAux[pix] = float4(totalRadiance, 0.0f);
        } else {
            float invNA = 1.0f / float(frameA);
            adaptiveAux[pix] = float4(currentA + (totalRadiance - currentA) * invNA, adaptiveAux[pix].w);
        }
    }

    if (sampleCount > globalParams.adaptiveMinSamples && globalParams.adaptiveThreshold > 0.0f)
    {
        float3 I = diffuse[pix].rgb + specular[pix].rgb;
        float3 A = adaptiveAux[pix].xyz;

        float error_difference = abs(I.r - A.r) + abs(I.g - A.g) + abs(I.b - A.b);
        float intensity = I.r + I.g + I.b;

        float error_normalize = intensity < 1.0f ? sqrt(intensity) : intensity;
        float error = error_difference / (0.0001f + error_normalize);

        bool did_converge = (error < globalParams.adaptiveThreshold);
        adaptiveAux[pix] = float4(adaptiveAux[pix].xyz, did_converge ? 1.0f : 0.0f);
    }
}

// Subsurface integrator
struct BSSRDFSurfaceSample
{
    bool valid;
    HitState surface;
    float3 Sp;
    float pdf;

    __init()
    {
        this.valid = false;
        this.surface = {};
        this.Sp = float3(0.0f);
        this.pdf = 0.0f;
    }
};

BSSRDFSurfaceInteraction MakeBSSRDFPo(HitState hit)
{
    return BSSRDFSurfaceInteraction(hit.position, hit.normal, hit.tangent, hit.bitangent);
}

BSSRDFSurfaceInteraction MakeBSSRDFPi(HitState hit)
{
    return BSSRDFSurfaceInteraction(hit.position, hit.geometryNormal, hit.tangent, hit.bitangent);
}

bool SeparableBSSRDF_SampleSp<S : ISampler>(
    uint2 pix,
    HitState surfaceHit,
    uint32_t sourceInstance,
    IBSSRDF bssrdf,
                                            inout S rng,
    out BSSRDFSurfaceSample result)
{
    result = BSSRDFSurfaceSample();

    BSSRDFSurfaceInteraction poBSSRDF = MakeBSSRDFPo(surfaceHit);
    BSSRDFSampleRaySegment segment =
        bssrdf.Sample_SpRay(poBSSRDF, rng.sample(), rng.sample(), rng.sample(), rng.sample());
    if (!segment.valid)
        return false;

    RayDesc queryRay;
    queryRay.Origin = segment.origin;
    queryRay.Direction = segment.direction;
    queryRay.TMin = 0.0f;
    queryRay.TMax = segment.tMax;

    WeightedReservoir<BSSRDFSurfaceCandidate> reservoir = WeightedReservoir<BSSRDFSurfaceCandidate>();
    RayQuery<RAY_FLAG_FORCE_NON_OPAQUE> q;
    q.TraceRayInline(TLAS, RAY_FLAG_FORCE_NON_OPAQUE, 0x01, queryRay);

    GSInstance surfaceInst = LoadSceneInstance(sourceInstance);

    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            uint instance = q.CandidateInstanceID();
            if (IsLightInstance(instance))
                continue;

            uint primitive = q.CandidatePrimitiveIndex();
            float2 bary = q.CandidateTriangleBarycentrics();

            PCG prng = PCG(pix, globalParams.frameNumber,
                           hash(uint3(instance, primitive, reservoir.sampleCount)));

            GSInstance inst = LoadSceneInstance(instance);

            if (instance == sourceInstance && inst.materialIndex == surfaceInst.materialIndex)
            {
                GSMaterial material = LoadMaterial(inst.materialIndex);
                float opacity = 1.0f;
                if (material.baseColorTexture != ~0u) {
                    float2 uv = GetHitUV(inst.resourceOffset, primitive, bary, inst);
                    float4 baseColor = material.baseColorFactor * gTextures2D[NonUniformResourceIndex(material.baseColorTexture)].SampleLevel(gTexSampler, uv, 0);
                    opacity = baseColor.a;
                } else {
                    opacity = material.baseColorFactor.a;
                }

                if (opacity == 1.0f || prng.sample() >= 1.0f - opacity) {
                    BSSRDFSurfaceCandidate candidate = BSSRDFSurfaceCandidate(instance, primitive, bary, q.CandidateTriangleRayT());
                    reservoir.Add(candidate, 1.0f, prng.sample());
                }
            }
        }
    }

    if (!reservoir.HasSample())
        return false;

    BSSRDFSurfaceCandidate selected = reservoir.selected;

    GSInstance piInst = LoadSceneInstance(selected.instance);
    HitState piHit = GetHitState(piInst.resourceOffset, selected.primitive, selected.bary, piInst, queryRay.Origin, queryRay.Direction, selected.hitT, 0.0f);

    float selectionPdf = reservoir.SelectionPDF();
    BSSRDFSurfaceInteraction piBSSRDF = MakeBSSRDFPi(piHit);
    float pdfSp = bssrdf.Pdf_Sp(poBSSRDF, piBSSRDF) * selectionPdf;
    float3 Sp = bssrdf.Sp(poBSSRDF, piBSSRDF);
    if (pdfSp <= EPS || MaxComponent(Sp) <= EPS)
        return false;

    result.valid = true;
    result.surface = piHit;
    result.Sp = Sp;
    result.pdf = pdfSp;
    return true;
}

// Raygen
void RayGenerationImpl(uint2 pix)
{
    if (pix.x >= uint(globalParams.fbWidth) || pix.y >= uint(globalParams.fbHeight))
        return;

    if (globalParams.ptAccumulatedFrames > globalParams.adaptiveMinSamples &&
        globalParams.adaptiveThreshold > 0.0f &&
        adaptiveAux[pix].w != 0.0f)
    {
        return;
    }

    for (uint subSample = 0; subSample < globalParams.ptSamplesPerPixel; subSample++)
    {
        uint sampleIndex = globalParams.ptAccumulatedFrames + subSample;

        // PCG or Sobol
        if ((kCompileOptions & uint32_t(PTCompileOptionsBits.SamplerPCG)) != 0)
        {
            PCG rng = PCG(pix, sampleIndex);
            PathTrace(pix, subSample, rng);
        }
        else
        {
            Sobol rng = Sobol(pix, sampleIndex, gSobolMatrices);
            PathTrace(pix, subSample, rng);
        }
    }
}

#ifdef USE_SER
[shader("raygeneration")]
void RayGeneration()
{
    RayGenerationImpl(DispatchRaysIndex().xy);
}
#else
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 tid : SV_DispatchThreadID)
{
    RayGenerationImpl(tid.xy);
}
#endif

void SampleMaterials(GSMaterial material, float2 uv, float2 uvDX, float2 uvDY,
    out float4 baseColor, out float4 normalTBN, out float4 pbrMetalRough, out float4 emissive,
    out float transmission, out float specularFactor, out float3 specularColorFactor,
    out float anisotropyStrength, out float2 anisotropyDirection,
    out float sheenWeight, out float sheenRoughness, out float3 sheenTint,
    out float clearcoatFactor, out float clearcoatRoughness)
{
    int materialFlags = globalParams.dbgMaterialFlags;
    baseColor = material.baseColorFactor;
    if (material.baseColorTexture != ~0u)
        baseColor *= SampleMaterialTexture(material.baseColorTexture, uv, uvDX, uvDY);
    if ((materialFlags & uint32_t(MaterialFlagsBits.DbgWhiteBaseColor)) != 0u)
        baseColor = float4(1.0f, 1.0f, 1.0f, baseColor.a);
    normalTBN = float4(0, 0, 1, 0);
    if (material.normalTexture != ~0u)
    {
        float2 xy = (SampleMaterialTexture(material.normalTexture, uv, uvDX, uvDY).xy * 2 - 1) * material.normalScale;
        normalTBN.xyz = float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
    }
    pbrMetalRough = float4(1, material.roughnessFactor, material.metallicFactor, 1);
    if (material.metallicRoughnessTexture != ~0u)
        pbrMetalRough *= SampleMaterialTexture(material.metallicRoughnessTexture, uv, uvDX, uvDY);
    emissive = float4(material.emissiveFactor, 0);
    if (material.emissiveTexture != ~0u)
        emissive *= SampleMaterialTexture(material.emissiveTexture, uv, uvDX, uvDY);

    transmission = material.transmissionFactor;
    if (transmission > EPS && material.transmissionTexture != ~0u)
        transmission *= SampleMaterialTexture(material.transmissionTexture, uv, uvDX, uvDY).r;

    specularFactor = material.specularFactor;
    if (specularFactor > EPS && material.specularTexture != ~0u)
        specularFactor *= SampleMaterialTexture(material.specularTexture, uv, uvDX, uvDY).a;

    specularColorFactor = material.specularColorFactor;
    if (MaxComponent(specularColorFactor) > EPS && material.specularColorTexture != ~0u)
        specularColorFactor *= SampleMaterialTexture(material.specularColorTexture, uv, uvDX, uvDY).rgb;

    float c = cos(material.anisotropyRotation);
    float s = sin(material.anisotropyRotation);
    anisotropyStrength = material.anisotropyStrength;
    anisotropyDirection = float2(c, s);
    if (anisotropyStrength > EPS && material.anisotropyTexture != ~0u)
    {
        float3 anisotropyTex = SampleMaterialTexture(material.anisotropyTexture, uv, uvDX, uvDY).rgb;
        float2 texDirection = anisotropyTex.rg * 2.0f - float2(1.0f);
        texDirection = LengthSquared(texDirection) > 1e-8f ? normalize(texDirection) : float2(1.0f, 0.0f);
        anisotropyDirection = float2(c * texDirection.x - s * texDirection.y,
                                     s * texDirection.x + c * texDirection.y);
        anisotropyStrength *= anisotropyTex.b;
    }

    float3 sheenColor = material.sheenColorFactor;
    if (MaxComponent(sheenColor) > EPS && material.sheenColorTexture != ~0u)
        sheenColor *= SampleMaterialTexture(material.sheenColorTexture, uv, uvDX, uvDY).rgb;

    sheenWeight = saturate(MaxComponent(sheenColor));
    sheenTint = sheenWeight > EPS ? saturate(sheenColor / sheenWeight) : float3(1.0f);

    sheenRoughness = material.sheenRoughnessFactor;
    if (sheenWeight > EPS && material.sheenRoughnessTexture != ~0u)
        sheenRoughness *= SampleMaterialTexture(material.sheenRoughnessTexture, uv, uvDX, uvDY).a;
    sheenRoughness = saturate(sheenRoughness);

    clearcoatFactor = material.clearcoatFactor;
    if (clearcoatFactor > EPS && material.clearcoatTexture != ~0u)
        clearcoatFactor *= SampleMaterialTexture(material.clearcoatTexture, uv, uvDX, uvDY).r;
    clearcoatFactor = saturate(clearcoatFactor);

    clearcoatRoughness = material.clearcoatRoughnessFactor;
    if (clearcoatFactor > EPS && material.clearcoatRoughnessTexture != ~0u)
        clearcoatRoughness *= SampleMaterialTexture(material.clearcoatRoughnessTexture, uv, uvDX, uvDY).g;
    clearcoatRoughness = saturate(clearcoatRoughness);
}

IMaterial MakeMaterial(HitPayload payload, inout HitState hit, float3 wo, out float3 outBaseColor, bool isCurve = false, bool foldSubsurfaceToDiffuse = false)
{
    GSInstance inst = LoadSceneInstance(payload.instance);
    GSMaterial material = LoadMaterial(inst.materialIndex);

    float4 baseColor4, normalTBN, pbrMetalRough, emissive4;
    float2 anisotropyDirection;
    float transmission, specularFactor, anisotropyStrength;
    float3 specularColorFactor;
    float sheenWeight, sheenRoughness;
    float3 sheenTint;
    float clearcoatFactor, clearcoatRoughness;

    SampleMaterials(material, hit.uv, hit.uvDX, hit.uvDY,
                    baseColor4, normalTBN, pbrMetalRough, emissive4,
                    transmission, specularFactor, specularColorFactor,
                    anisotropyStrength, anisotropyDirection,
                    sheenWeight, sheenRoughness, sheenTint,
                    clearcoatFactor, clearcoatRoughness);

    outBaseColor = baseColor4.xyz;

    SetHitShadingNormal(hit, hit.tangent * normalTBN.x + hit.bitangent * normalTBN.y + hit.normal * normalTBN.z);
    RotateHitTangentFrame(hit, anisotropyDirection);

    if (material.shaderBlockID == kMaterialShaderBlockHair)
    {
        float h = isCurve ? 2.0f * saturate(hit.uv.y) - 1.0f : 0.0f;
        return PrincipledHairMaterial(
            baseColor4.xyz, material.hairBetaM, material.hairBetaN, h, material.ior, material.hairAlpha,
            emissive4.xyz);
    }

    float subsurface = foldSubsurfaceToDiffuse ? 0.0f : material.subsurfaceFactor;
    return PrincipledMaterial(
        baseColor4.xyz, pbrMetalRough.z, pbrMetalRough.y,
        material.ior, specularFactor, transmission,
        (kCompileOptions & uint32_t(PTCompileOptionsBits.EnergyCompensation)) != 0u, wo,
        subsurface, material.subsurfaceColor, material.subsurfaceRadius, material.subsurfaceScale,
        anisotropyStrength, specularColorFactor, globalParams.ggxLutEIndex, globalParams.ggxLutEavgIndex, globalParams.ggxLutEIORIndex, globalParams.ggxLutEIORavgIndex, globalParams.ggxLutEIORInvIndex, globalParams.ggxLutEIORInvavgIndex,
        sheenWeight, sheenRoughness, sheenTint, globalParams.sheenLtcIndex,
        clearcoatFactor, clearcoatRoughness,
        emissive4.xyz);
}

float3 VisualizeTextureLOD(HitPayload payload, float2 uvDX, float2 uvDY)
{
    GSInstance inst = LoadSceneInstance(payload.instance);
    GSMaterial material = LoadMaterial(inst.materialIndex);
    if (material.baseColorTexture == ~0u)
        return float3(0.0f);

    if ((kCompileOptions & uint32_t(PTCompileOptionsBits.ForceTextureLOD0)) != 0)
        return VisualizeApproximateMipLevel(0.0f);

    uint width, height;
    gTextures2D[NonUniformResourceIndex(material.baseColorTexture)].GetDimensions(width, height);
    float mipLevel = ApproximateMipLevelFromUVGradient(uvDX, uvDY, width, height);
    return VisualizeApproximateMipLevel(mipLevel);
}

void PathTrace<S : ISampler>(uint2 pix, uint subSample, inout S rng)
{
    float3 beta = float3(1);
    float etaScale = 1.0f;
    PathRadiance radiance = PathRadiance();
    AOVCategory firstBounceCategory = AOVCategory::Undefined;
    bool resetAccumulation = globalParams.ptAccumulatedFrames == 0u && subSample == 0u;
    float sampleCount = resetAccumulation ? 1.0f : diffuse[pix].a + 1.0f;
    float lastSamplePDF = 0.0f;
    LightSampleContext lastLightCtx = LightSampleContext(float3(0.0f), float3(0.0f));
    const int supportDebugViewFlags = int(uint32_t(ViewFlagsBits.BaseColor) | uint32_t(ViewFlagsBits.Normal) |
                                          uint32_t(ViewFlagsBits.Position) | uint32_t(ViewFlagsBits.TextureLOD));
    bool hasAnyDebugView = (globalParams.dbgViewFlags & supportDebugViewFlags) != 0u;
    CameraRay cameraRay = GenerateCameraRay(pix, rng, hasAnyDebugView);
    RayDesc ray = cameraRay.ray;
    RayCone currentRayCone = cameraRay.rayCone;
    uint primaryInstance = INSTANCE_NO_HIT;
    float primaryDepth = 0.0f;
    uint bounce = 0u;
    uint analyticPasses = 0u;
    while (bounce <= globalParams.ptMaxBounces)
    {
        HitPayload payload;

        RayQuery<RAY_FLAG_NONE> q;
        q.TraceRayInline(TLAS, RAY_FLAG_NONE, 0xFF, ray);
        float2 bestProceduralBary = float2(0.0f);
        while (q.Proceed())
        {
            if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
            {
                uint instance = q.CandidateInstanceID();
                if (IsCurveInstance(instance))
                {
                    q.CommitNonOpaqueTriangleHit();
                    continue;
                }
                uint primitive = q.CandidatePrimitiveIndex();
                float2 bary = q.CandidateTriangleBarycentrics();
                if (!EvaluateOpacityAlphaTest(instance, primitive, bary, rng))
                {
                    q.CommitNonOpaqueTriangleHit();
                }
            }
            else if (IsLightInstance(q.CandidateInstanceID()))
            {
                if (bounce == 0u && globalParams.ptPrimaryLightVisibility == 0u)
                    continue;
                uint instance = q.CandidateInstanceID();
                GSLight gl = LoadSceneLight(GetLightIndex(instance));
                float3 ro = q.CandidateObjectRayOrigin();
                float3 rd = q.CandidateObjectRayDirection();
                float t;
                float2 localPos;
                if (EvaluateLightIntersection(GSLightType(gl), ro, rd, q.RayTMin(), q.CommittedRayT(), t, localPos))
                {
                    bestProceduralBary = localPos;
                    q.CommitProceduralPrimitiveHit(t);
                }
            }
        }

        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            payload.instance = q.CommittedInstanceID();
            payload.primitive = q.CommittedPrimitiveIndex();
            payload.bary = q.CommittedTriangleBarycentrics();
            payload.hitT = q.CommittedRayT();
        }
        else if (q.CommittedStatus() == COMMITTED_PROCEDURAL_PRIMITIVE_HIT)
        {
            payload.instance = q.CommittedInstanceID();
            payload.primitive = q.CommittedPrimitiveIndex();
            payload.bary = bestProceduralBary;
            payload.hitT = q.CommittedRayT();
        }
        else
        {
            payload.instance = INSTANCE_NO_HIT;
        }

        #ifdef USE_SER
            uint coherenceHint = BuildSERCoherenceHint(payload, bounce);
            ReorderThread(coherenceHint, kSERCoherenceHintBits);
        #endif

        // Outgoing direction
        float3 view = -ray.Direction;
        HitRecord hit = MakeHitRecord(payload, ray);

        float3 hitPoint;
        if (!hasAnyDebugView && AddAnalyticLightHitEmission(hit, ray.Direction, view, beta, bounce, firstBounceCategory,
                                        lastSamplePDF, lastLightCtx, radiance, hitPoint))
        {
            currentRayCone.width += currentRayCone.spreadSlope * hit.raw.hitT;
            if (++analyticPasses > 64u)
                break;
            ray.Origin = hitPoint + ray.Direction * EPS_TMin;
            continue;
        }
        analyticPasses = 0u;

        if (!hasAnyDebugView && hit.kind == HitKind::None)
        {
            AddInfiniteMiss(view, beta, bounce, firstBounceCategory, lastSamplePDF, lastLightCtx, radiance);
            break;
        }
        if (!IsMaterialSurfaceHit(hit.kind))
            break;

        currentRayCone.width += currentRayCone.spreadSlope * hit.raw.hitT;

        HitState hitState;
        if (hit.kind == HitKind::Curve)
        {
            GSInstance inst = LoadSceneInstance(hit.raw.instance);
            hitState = GetCurveHitState(inst.resourceOffset, hit.raw.primitive, hit.raw.bary, inst,
                               ray.Origin, ray.Direction, hit.raw.hitT);
        }
        else
        {
            GSInstance inst = LoadSceneInstance(hit.raw.instance);
            hitState = GetHitState(inst.resourceOffset, hit.raw.primitive, hit.raw.bary, inst, ray.Origin, ray.Direction, hit.raw.hitT, currentRayCone.width);
        }

        float3 wo = WorldToShadingLocal(hitState, view);
        bool foldSubsurfaceToDiffuse = bounce > 0;
        float3 baseColorOut;
        IMaterial material = MakeMaterial(hit.raw, hitState, wo, baseColorOut, hit.kind == HitKind::Curve, foldSubsurfaceToDiffuse);

        if (hasAnyDebugView && bounce == 0)
        {
            specular[pix] = diffuse[pix] = float4(0.0f);
            int flag = globalParams.dbgViewFlags;
            if ((flag & int(uint32_t(ViewFlagsBits.BaseColor))) != 0)
                diffuse[pix] = float4(baseColorOut, 1.0f);
            if ((flag & int(uint32_t(ViewFlagsBits.Normal))) != 0)
                diffuse[pix] = float4(hitState.normal * 0.5f + 0.5f, 1.0f);

            if ((flag & int(uint32_t(ViewFlagsBits.Position))) != 0)
                diffuse[pix] = float4(hitState.position, 1.0f);
            if ((flag & int(uint32_t(ViewFlagsBits.TextureLOD))) != 0)
                diffuse[pix] = float4(VisualizeTextureLOD(hit.raw, hitState.uvDX, hitState.uvDY), 1.0f);            
            // ...
            WritePrimaryInstanceID(pix, hit.raw.instance);
            return;
        }

        if (bounce == 0u)
        {
            primaryInstance = hit.raw.instance;
            primaryDepth = WorldToNDCDepth(hitState.position);
        }
        
        // Local frame setup
        // Subsurface events are non-local. The closure picks the event
        // the integrator performs the scene query.
        float pSampleSubsurface = material.ProbSampleSubsurface();
        float pSampleSurface = material.ProbSampleSurface();
        pSampleSurface = max(pSampleSurface, EPS);
        bool sampleSubsurface = rng.sample() < pSampleSubsurface;
        // TODO Volumetrics
        if (sampleSubsurface)
        {
            BSSRDFSurfaceSample sample;
            IBSSRDF bssrdf = material.GetSubsurface();
            if (!SeparableBSSRDF_SampleSp(pix, hitState, hit.raw.instance, bssrdf, rng, sample))
                break;

            float3 localBeta = beta * sample.Sp / max(pSampleSubsurface * sample.pdf, EPS);
            if (any(isnan(localBeta)) || any(isinf(localBeta)))
                break;

            HitState exitSurface = sample.surface;
            // Lambertian assumed for subsurface interactions
            const float3 exitF = float3(InvPi);
            LightSampleContext currentLightCtx =
                LightSampleContext(exitSurface.position, exitSurface.normal, true);
            lastLightCtx = currentLightCtx;
            AddDiffuseDirectLighting(exitSurface, exitF, localBeta,
                                     bounce, firstBounceCategory, AOVCategory::Diffuse,
                                     rng, currentLightCtx, radiance);

            if (bounce == globalParams.ptMaxBounces)
                break;

            float3 exitWi = SampleCosineHemisphere(rng.sample2D());
            exitWi = FaceForward(exitWi, float3(0, 0, 1));
            float exitPdf = CosineHemispherePDF(AbsCosTheta(exitWi));
            if (exitPdf <= EPS)
                break;

            beta = localBeta * exitF * AbsCosTheta(exitWi) / exitPdf;
            if (bounce == 0u)
                firstBounceCategory = AOVCategory::Diffuse;
            lastSamplePDF = exitPdf;
            if (!ContinueAfterRussianRoulette(beta, etaScale, bounce, rng))
                break;

            ray.Direction = normalize(ShadingLocalToWorld(exitSurface, exitWi));
            float3 rayNormalFF = dot(ray.Direction, exitSurface.geometryNormal) < 0.0f
                ? -exitSurface.geometryNormal
                : exitSurface.geometryNormal;
            ray.Origin = OffsetRay(exitSurface.position, rayNormalFF);
            ++bounce;
            continue;
        }

        float3 localBeta = beta / pSampleSurface;
        IBSDF bsdf = material.GetSurface();
        LightSampleContext previousLightCtx = lastLightCtx;
        LightSampleContext currentLightCtx =
            LightSampleContext(hitState.position, hitState.normal, !bsdf.HasTransmission());
        if (!AddEmissiveClusterHitEmission(hit.raw, hitState, ray.Direction, localBeta,
                                           bounce, firstBounceCategory, lastSamplePDF,
                                           previousLightCtx, radiance))
            AddSurfaceEmissive(material, wo, localBeta, bounce, firstBounceCategory, radiance);
        lastLightCtx = currentLightCtx;
        if (!bsdf.IsDirac())
            AddLocalDirectLighting(hitState, bsdf, wo, localBeta,
                                   bounce, firstBounceCategory, rng, currentLightCtx, radiance);

        if (bounce == globalParams.ptMaxBounces)
            break;

        AOVCategory sampleCategory;
        BSDFSample sample = BSDF_SampleF(bsdf, wo, rng.sample(), rng.sample2D(),
                                         TransportMode::Radiance, BxDFReflTransFlags::All,
                                         sampleCategory);
        if (!sample.IsValid())
            break;

        beta = localBeta * sample.f * AbsCosTheta(sample.wi) / sample.pdf;
        if (bounce == 0u)
            firstBounceCategory = sampleCategory;
        lastSamplePDF = sample.pdf;
        if (IsTransmission(sample.flags))
            etaScale *= sample.eta * sample.eta;
        if (!ContinueAfterRussianRoulette(beta, etaScale, bounce, rng))
            break;

        ray.Direction = normalize(ShadingLocalToWorld(hitState, sample.wi));
        float3 rayNormalFF = dot(ray.Direction, hitState.geometryNormal) < 0.0f
            ? -hitState.geometryNormal
            : hitState.geometryNormal;
        ray.Origin = OffsetRay(hitState.position, rayNormalFF);
        ++bounce;
    }
    WritePrimaryInstanceID(pix, primaryInstance);
    AccumulatePathRadiance(pix, radiance, sampleCount);
    depth[pix] = primaryDepth;
}
