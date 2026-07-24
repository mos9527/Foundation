// "Realtime" Path Tracer
// TODO:
//  [] SHARC
//  [] ReSTIR DI
//  [] Denoiser
#include "IPathTracing.slang"

// Raygen
void RayGenerationImpl(uint2 pix)
{
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

IMaterial MakeMaterial(HitPayload payload, inout HitState hit, float3 wo)
{
    float4 baseColor4, normalTBN, pbrMetalRough, emissive4;
    float transmission;
    GSInstance inst = LoadSceneInstance(payload.instance);
    GSMaterial material = LoadMaterial(inst.materialIndex);
    SampleMaterials(material, hit.uv, hit.uvDX, hit.uvDY,
                             baseColor4, normalTBN, pbrMetalRough, emissive4, transmission);
    SetHitShadingNormal(hit, hit.tangent * normalTBN.x + hit.bitangent * normalTBN.y + hit.normal * normalTBN.z);
    return BasicPBRMaterial(baseColor4.xyz, pbrMetalRough.z, pbrMetalRough.y, emissive4.xyz, transmission);
}

void PathTrace<S : ISampler>(uint2 pix, inout S rng)
{
    float3 beta = float3(1);
    PathRadiance radiance = PathRadiance();
    AOVCategory firstBounceCategory = AOVCategory::Undefined;
    float lastSamplePDF = 0.0f;
    // Serialized scenes keep the environment light as the first scene light.
    GSLight envLight = LoadSceneLight(0u);
    InfiniteLight env = MakeEnvironmentLight(envLight);
    DirectLightingState lighting = DirectLightingState(env);
    // Raygen
    CameraRay cameraRay = GenerateCameraRay(pix, rng, false);
    RayDesc ray = cameraRay.ray;
    RayCone currentRayCone = cameraRay.rayCone;
    HitRecord chit;
    HitState chitState;
    for (uint bounce = 0; bounce <= globalParams.ptMaxBounces; bounce++) {
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
            } else if (IsLightInstance(q.CandidateInstanceID()))
            {
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
        if (AddAnalyticLightHitEmission(hit, ray.Direction, view, beta, bounce, firstBounceCategory,
                                        lastSamplePDF, lighting, radiance, hitPoint))
        {
            ray.Origin = hitPoint + ray.Direction * EPS_TMin;
            bounce--; // Don't count this as a bounce
            continue;
        }

        if (hit.kind == HitKind::None)
        {
            AddEnvironmentMiss(view, beta, bounce, firstBounceCategory, lastSamplePDF, lighting, radiance);
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
        IMaterial material = MakeMaterial(hit.raw, hitState, wo);

        if (bounce == 0)
            chit = hit, chitState = hitState;

        PathSample nextSample = PathSample();
        float3 localBeta = beta;
        IBSDF bsdf = material.GetSurface();

        // Dirac surfaces do not get direct light contribution
        if (!bsdf.IsDirac())
            AddLocalDirectLighting(hitState, bsdf, wo, localBeta,
                                bounce, firstBounceCategory, rng, lighting, radiance);
        AddSurfaceEmissive(material, wo, localBeta, bounce, firstBounceCategory, radiance);

        AOVCategory sampleCategory;
        BSDFSample sample = BSDF_SampleF(bsdf, wo, rng.sample(), rng.sample2D(),
                                            TransportMode::Radiance, BxDFReflTransFlags::All,
                                            sampleCategory);
        if (!sample.IsValid()) {
            break;
        }

        nextSample.valid = true;
        nextSample.beta = localBeta * sample.f * AbsCosTheta(sample.wi) / sample.pdf;
        nextSample.wi = sample.wi;
        nextSample.pdf = sample.pdf;
        nextSample.flags = sample.flags;
        nextSample.category = sampleCategory;
        nextSample.hit = hitState;

        if (!nextSample.valid)
            break;

        if (bounce == 0)
            firstBounceCategory = nextSample.category;

        lastSamplePDF = nextSample.pdf;
        beta = nextSample.beta;
        if (any(isnan(beta)) || any(isinf(beta)))
            break;

        ray.Direction = normalize(ShadingLocalToWorld(nextSample.hit, nextSample.wi));
        float3 rayNormalFF = dot(ray.Direction, nextSample.hit.geometryNormal) < 0.0f ? -nextSample.hit.geometryNormal : nextSample.hit.geometryNormal;
        ray.Origin = OffsetRay(nextSample.hit.position, rayNormalFF);

        // Russian Roulette (after 2nd bounce)
        if (bounce >= 2) {
            float rrPCount = min(dot(beta, float3(0.2126f, 0.7152f, 0.0722f)) + 0.01f, 0.95f);
            if (rng.sample() > rrPCount) // Contribution is low, may terminate early
                break;
            beta /= rrPCount; // Boost non-terminated paths
        }
    }
    WritePrimaryInstanceID(pix, chit.raw.instance);
    diffuse[pix] = float4(radiance.diffuse, 1.0f);
    specular[pix] = float4(radiance.specular, 1.0f);
    depth[pix] = WorldToNDCDepth(chitState.position);
}
