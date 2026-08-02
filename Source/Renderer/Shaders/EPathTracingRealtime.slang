// "Realtime" Path Tracer
// TODO:
//  [] ReSTIR DI
//  [] Denoiser
#include "IPathTracing.slang"
#ifndef SHARC_QUERY
#define SHARC_QUERY 0
#endif
#if SHARC_QUERY || SHARC_UPDATE
#include "ISharcIntegration.slang"
#endif

// Raygen
void RayGenerationImpl(uint2 launchPixel)
{
    uint2 pix = launchPixel;
#if SHARC_UPDATE
    uint downscale = max(globalParams.sharcUpdateDownscale, 1u);
    uint sample = globalParams.frameNumber % (downscale * downscale);
    pix = launchPixel * downscale + uint2(sample % downscale, sample / downscale);
#endif
    if (pix.x >= uint(globalParams.fbWidth) || pix.y >= uint(globalParams.fbHeight))
        return;

    PCG rng = PCG(pix, globalParams.frameNumber);
    PathTrace(pix, rng);
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
                          out float4 baseColor, out float4 normalTBN,
                          out float4 pbrMetalRough, out float4 emissive, out float transmission)
{
    baseColor = material.baseColorFactor;
    if (material.baseColorTexture != ~0u)
        baseColor *= SampleMaterialTexture(material.baseColorTexture, uv, uvDX, uvDY);
    if ((globalParams.dbgMaterialFlags & uint32_t(MaterialFlagsBits.DbgWhiteBaseColor)) != 0u)
        baseColor = float4(1.0f, 1.0f, 1.0f, baseColor.a);

    normalTBN = float4(0.0f, 0.0f, 1.0f, 0.0f);
    if (material.normalTexture != ~0u)
    {
        float2 xy = (SampleMaterialTexture(material.normalTexture, uv, uvDX, uvDY).xy * 2.0f - 1.0f)
                  * material.normalScale;
        normalTBN.xyz = float3(xy, sqrt(saturate(1.0f - dot(xy, xy))));
    }

    pbrMetalRough = float4(1.0f, material.roughnessFactor, material.metallicFactor, 1.0f);
    if (material.metallicRoughnessTexture != ~0u)
        pbrMetalRough *= SampleMaterialTexture(material.metallicRoughnessTexture, uv, uvDX, uvDY);

    emissive = float4(material.emissiveFactor, 0.0f);
    if (material.emissiveTexture != ~0u)
        emissive *= SampleMaterialTexture(material.emissiveTexture, uv, uvDX, uvDY);

    transmission = material.transmissionFactor;
    if (material.transmissionTexture != ~0u)
        transmission *= SampleMaterialTexture(material.transmissionTexture, uv, uvDX, uvDY).r;
}

IMaterial MakeMaterial(HitPayload payload, inout HitState hit, float3 wo, out float3 outBaseColor,
                       out float3 sharcMaterialDemodulation, out float sharcRoughness)
{
    float4 baseColor4, normalTBN, pbrMetalRough, emissive4;
    float transmission;
    GSInstance inst = LoadSceneInstance(payload.instance);
    GSMaterial material = LoadMaterial(inst.materialIndex);
    SampleMaterials(material, hit.uv, hit.uvDX, hit.uvDY,
                             baseColor4, normalTBN, pbrMetalRough, emissive4, transmission);
    outBaseColor = baseColor4.xyz;
    SetHitShadingNormal(hit, hit.tangent * normalTBN.x + hit.bitangent * normalTBN.y + hit.normal * normalTBN.z);

#if SHARC_QUERY || SHARC_UPDATE
    sharcRoughness = pbrMetalRough.y;
    float3 diffuseAlbedo = baseColor4.xyz * (1.0f - pbrMetalRough.z);
    float3 specularF0 = lerp(float3(transmission > 0.0f ? 0.08f : 0.04f), baseColor4.xyz, pbrMetalRough.z);
    float3 specularFAvg = specularF0 + (1.0f - specularF0) * (1.0f / 21.0f);
    sharcMaterialDemodulation =
        max(diffuseAlbedo, float3(0.05f)) +
        max(specularF0, float3(0.02f)) * dot(specularFAvg, float3(0.2126f, 0.7152f, 0.0722f));
#else
    sharcRoughness = 0.0f;
    sharcMaterialDemodulation = float3(0.0f);
#endif
#if SHARC_UPDATE
    pbrMetalRough.y = max(globalParams.sharcRoughnessThreshold, pbrMetalRough.y);
#endif
    return BasicPBRMaterial(baseColor4.xyz, pbrMetalRough.z, pbrMetalRough.y, emissive4.xyz, transmission);
}

float3 VisualizeTextureLOD(HitPayload payload, float2 uvDX, float2 uvDY)
{
    GSInstance inst = LoadSceneInstance(payload.instance);
    GSMaterial material = LoadMaterial(inst.materialIndex);
    if (material.baseColorTexture == ~0u)
        return float3(0.0f);

    if ((kCompileOptions & uint32_t(PTCompileOptionsBits.ForceTextureLOD0)) != 0u)
        return VisualizeApproximateMipLevel(0.0f);

    uint width, height;
    gTextures2D[NonUniformResourceIndex(material.baseColorTexture)].GetDimensions(width, height);
    float mipLevel = ApproximateMipLevelFromUVGradient(uvDX, uvDY, width, height);
    return VisualizeApproximateMipLevel(mipLevel);
}

#if SHARC_QUERY || SHARC_UPDATE
SharcHitData MakeSharcHitData(HitState hit, float3 materialDemodulation)
{
    SharcHitData data;
    data.positionWorld = hit.position;
    data.normalWorld = hit.geometryNormal;
    data.materialDemodulation = materialDemodulation;
    return data;
}
#endif

void PathTrace<S : ISampler>(uint2 pix, inout S rng)
{
    float3 beta = float3(1);
#if !SHARC_UPDATE
    float etaScale = 1.0f;
#endif
    PathRadiance radiance = PathRadiance();
    AOVCategory firstBounceCategory = AOVCategory::Undefined;
    float lastSamplePDF = 0.0f;
    LightSampleContext lastLightCtx = LightSampleContext(float3(0.0f), float3(0.0f));
#if SHARC_QUERY || SHARC_UPDATE
    SharcParameters sharcParameters = MakeSharcParameters();
#endif
#if SHARC_UPDATE
    SharcState sharcState;
    SharcInit(sharcState);
#endif
#if SHARC_QUERY
    float sharcPathRoughness = 0.0f;
#endif
    bool hasAnyDebugView = false;
#if !SHARC_UPDATE
    const uint supportedDebugViewFlags =
        uint32_t(ViewFlagsBits.BaseColor) | uint32_t(ViewFlagsBits.Normal) |
        uint32_t(ViewFlagsBits.Position) | uint32_t(ViewFlagsBits.TextureLOD);
    hasAnyDebugView = (globalParams.dbgViewFlags & supportedDebugViewFlags) != 0u;
#if SHARC_QUERY
    if ((globalParams.dbgViewFlags & uint32_t(ViewFlagsBits.SHARCOccupancy)) != 0u)
    {
        uint2 screenSize = uint2(globalParams.fbWidth, globalParams.fbHeight);
        float3 color = HashGridDebugOccupancy(pix, screenSize, sharcParameters.hashGridData);
        WritePrimaryInstanceID(pix, INSTANCE_NO_HIT);
        diffuse[pix] = float4(color, 1.0f);
        specular[pix] = float4(0.0f);
        depth[pix] = 0.0f;
        return;
    }
#endif
#endif
    // Raygen
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
        if (!hasAnyDebugView &&
            AddAnalyticLightHitEmission(hit, ray.Direction, view, beta, bounce, firstBounceCategory,
                                        lastSamplePDF, lastLightCtx, radiance, hitPoint))
        {
            currentRayCone.width += currentRayCone.spreadAngle * hit.raw.hitT;
            if (++analyticPasses > 64u)
                break;
            ray.Origin = hitPoint + ray.Direction * EPS_TMin;
            continue;
        }
        analyticPasses = 0u;

        if (hit.kind == HitKind::None)
        {
#if SHARC_UPDATE
            InfiniteLight env = MakeEnvironmentLight(LoadSceneLight(0u));
            SharcUpdateMiss(sharcParameters, sharcState, EvaluateInfiniteMissRadiance(-view, env));
#else
            if (!hasAnyDebugView)
                AddInfiniteMiss(view, beta, bounce, firstBounceCategory, lastSamplePDF, lastLightCtx, radiance);
#endif
            break;
        }

        if (!IsMaterialSurfaceHit(hit.kind))
            break;

        currentRayCone.width += currentRayCone.spreadAngle * hit.raw.hitT;

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
        float3 baseColorOut;
        float3 sharcMaterialDemodulation;
        float sharcRoughness;
        IMaterial material = MakeMaterial(
            hit.raw, hitState, wo, baseColorOut, sharcMaterialDemodulation, sharcRoughness);

#if !SHARC_UPDATE
        if (hasAnyDebugView && bounce == 0)
        {
            specular[pix] = diffuse[pix] = float4(0.0f);
            uint flags = globalParams.dbgViewFlags;
            if ((flags & uint32_t(ViewFlagsBits.BaseColor)) != 0u)
                diffuse[pix] = float4(baseColorOut, 1.0f);
            if ((flags & uint32_t(ViewFlagsBits.Normal)) != 0u)
                diffuse[pix] = float4(hitState.normal * 0.5f + 0.5f, 1.0f);
            if ((flags & uint32_t(ViewFlagsBits.Position)) != 0u)
                diffuse[pix] = float4(hitState.position, 1.0f);
            if ((flags & uint32_t(ViewFlagsBits.TextureLOD)) != 0u)
                diffuse[pix] = float4(VisualizeTextureLOD(hit.raw, hitState.uvDX, hitState.uvDY), 1.0f);
            WritePrimaryInstanceID(pix, hit.raw.instance);
            depth[pix] = WorldToNDCDepth(hitState.position);
            return;
        }
#endif

        if (bounce == 0u)
        {
            primaryInstance = hit.raw.instance;
            primaryDepth = WorldToNDCDepth(hitState.position);
        }

#if !SHARC_UPDATE
#if SHARC_QUERY
        if (bounce == 0 &&
            (globalParams.dbgViewFlags & uint32_t(ViewFlagsBits.SHARCRadiance)) != 0u)
        {
            SharcHitData sharcHit = MakeSharcHitData(hitState, sharcMaterialDemodulation);
            float3 cachedRadiance = float3(0.0f);
            SharcGetCachedRadiance(sharcParameters, sharcHit, cachedRadiance, false);
            WritePrimaryInstanceID(pix, hit.raw.instance);
            diffuse[pix] = float4(cachedRadiance, 1.0f);
            specular[pix] = float4(0.0f);
            depth[pix] = WorldToNDCDepth(hitState.position);
            return;
        }
        if (bounce == 0 &&
            (globalParams.dbgViewFlags & uint32_t(ViewFlagsBits.SHARCGrid)) != 0u)
        {
            float3 color = HashGridDebugColoredHash(
                hitState.position, hitState.geometryNormal, sharcParameters.hashGridParameters);
            WritePrimaryInstanceID(pix, hit.raw.instance);
            diffuse[pix] = float4(color, 1.0f);
            specular[pix] = float4(0.0f);
            depth[pix] = WorldToNDCDepth(hitState.position);
            return;
        }
#endif
#endif

        float3 localBeta = beta;
        IBSDF bsdf = material.GetSurface();

#if SHARC_QUERY
        if (globalParams.sharcEnabled != 0u && bounce > 0)
        {
            SharcHitData sharcHit = MakeSharcHitData(hitState, sharcMaterialDemodulation);
            uint gridLevel = HashGridGetLevel(hitState.position, sharcParameters.hashGridParameters);
            float voxelSize = HashGridGetVoxelSize(gridLevel, sharcParameters.hashGridParameters);
            float roughness = min(sharcPathRoughness, 0.99f);
            float alpha = roughness * roughness;
            float footprint = hit.raw.hitT * sqrt(0.5f * alpha * alpha / max(1.0f - alpha * alpha, EPS));
            bool validHit = hit.raw.hitT > voxelSize * sqrt(3.0f) && footprint > voxelSize;
            float3 cachedRadiance;
            if (validHit && SharcGetCachedRadiance(sharcParameters, sharcHit, cachedRadiance, false))
            {
                AddRadianceByCategory(radiance, beta * cachedRadiance, bounce,
                                      firstBounceCategory, AOVCategory::Diffuse, false);
                break;
            }
        }
#endif

        LightSampleContext previousLightCtx = lastLightCtx;
        LightSampleContext currentLightCtx =
            LightSampleContext(hitState.position, hitState.normal, !bsdf.HasTransmission());
#if SHARC_UPDATE
        PathRadiance localRadiance = PathRadiance();
        if (!AddEmissiveClusterHitEmission(hit.raw, hitState, ray.Direction, float3(1.0f),
                                           0u, AOVCategory::Undefined, lastSamplePDF,
                                           previousLightCtx, localRadiance))
            AddSurfaceEmissive(
                material, wo, float3(1.0f), 0u, AOVCategory::Undefined, localRadiance);
        lastLightCtx = currentLightCtx;
        if (!bsdf.IsDirac())
            AddLocalDirectLighting(hitState, bsdf, wo, float3(1.0f),
                                   0u, AOVCategory::Undefined, rng, currentLightCtx, localRadiance);
        SharcHitData sharcHit = MakeSharcHitData(hitState, sharcMaterialDemodulation);
        if (!SharcUpdateHit(
                sharcParameters, sharcState, sharcHit,
                localRadiance.diffuse + localRadiance.specular, rng.sample()))
            break;
#else
        if (!AddEmissiveClusterHitEmission(hit.raw, hitState, ray.Direction, localBeta,
                                           bounce, firstBounceCategory, lastSamplePDF,
                                           previousLightCtx, radiance))
            AddSurfaceEmissive(material, wo, localBeta, bounce, firstBounceCategory, radiance);
        lastLightCtx = currentLightCtx;
        if (!bsdf.IsDirac())
            AddLocalDirectLighting(hitState, bsdf, wo, localBeta,
                                   bounce, firstBounceCategory, rng, currentLightCtx, radiance);
#endif

        if (bounce == globalParams.ptMaxBounces)
            break;

        AOVCategory sampleCategory;
        BSDFSample sample = BSDF_SampleF(bsdf, wo, rng.sample(), rng.sample2D(),
                                         TransportMode::Radiance, BxDFReflTransFlags::All,
                                         sampleCategory);
        if (!sample.IsValid())
            break;

        float3 nextBeta = localBeta * sample.f * AbsCosTheta(sample.wi) / sample.pdf;
        if (bounce == 0u)
            firstBounceCategory = sampleCategory;

        lastSamplePDF = sample.pdf;
#if SHARC_UPDATE
        SharcSetThroughput(sharcState, nextBeta);
        beta = float3(1.0f);
#else
        beta = nextBeta;
        if (IsTransmission(sample.flags))
            etaScale *= sample.eta * sample.eta;
        if (!ContinueAfterRussianRoulette(beta, etaScale, bounce, rng))
            break;
#endif

#if SHARC_QUERY
        sharcPathRoughness += IsDiffuse(sample.flags) ? 1.0f : sharcRoughness;
#endif
        ray.Direction = normalize(ShadingLocalToWorld(hitState, sample.wi));
        float3 rayNormalFF = dot(ray.Direction, hitState.geometryNormal) < 0.0f
            ? -hitState.geometryNormal
            : hitState.geometryNormal;
        ray.Origin = OffsetRay(hitState.position, rayNormalFF);
        ++bounce;
    }
#if !SHARC_UPDATE
    WritePrimaryInstanceID(pix, primaryInstance);
    diffuse[pix] = float4(radiance.diffuse, 1.0f);
    specular[pix] = float4(radiance.specular, 1.0f);
    depth[pix] = primaryDepth;
#endif
}
