#include <Core/Paths.hpp>
#include "Renderer.hpp"

RendererResources CreateGPUSceneRendererResources(Renderer* renderer, GPUScene* scene)
{
    CHECK(renderer);
    CHECK(scene);
    RendererResources resources{
        .scene = scene,
        .primitiveBuffer = renderer->CreateResource("Primitive Buffer", scene->GetPrimitiveBuffer()),
        .dynamicPrimitiveBuffer =
            renderer->CreateResource("Dynamic Primitive Buffer", scene->GetDynamicPrimitiveBuffer()),
        .instanceBuffer = renderer->CreateResource("Instance Buffer", scene->GetInstanceBuffer()),
        .materialBuffer = renderer->CreateResource("Material Buffer", scene->GetMaterialBuffer()),
        .lightBuffer = renderer->CreateResource("Light Buffer", scene->GetLightBuffer()),
        .lightBVHNodeBuffer = renderer->CreateResource("Light BVH Nodes", scene->GetLightBVHNodeBuffer()),
        .lightBVHLightIndexBuffer =
            renderer->CreateResource("Light BVH Light Indices", scene->GetLightBVHLightIndexBuffer()),
        .lightBVHBitmaskBuffer = renderer->CreateResource("Light BVH Bitmasks", scene->GetLightBVHBitmaskBuffer()),
        .lightBVHNodeIndexBuffer =
            renderer->CreateResource("Light BVH Node Indices", scene->GetLightBVHNodeIndexBuffer()),
        .sobolMatricesBuffer = renderer->CreateResource("Sobol Matrices Buffer", scene->GetSobolMatricesBuffer()),
        .textures2D = scene->GetTexture2DPool(),
        .textures3D = scene->GetTexture3DPool(),
        .primitiveBufferRHI = scene->GetPrimitiveBuffer(),
        .dynamicPrimitiveBufferRHI = scene->GetDynamicPrimitiveBuffer(),
        .hasDynamicGeometry = scene->HasDynamicGeometry(),
        .hasCurveGeometry = scene->HasCurveGeometry(),
    };
    if (resources.hasDynamicGeometry)
        resources.dynamicStagingBuffer =
            renderer->CreateResource("Dynamic Primitive Staging", scene->GetDynamicStagingBuffer());
    if (scene->GetTLAS())
        resources.tlas = renderer->CreateResource("Scene TLAS", scene->GetTLAS());
    return resources;
}

void BuildGPUSceneUpdatePasses(Renderer* renderer, RendererResources const& resources,
                               ResourceHandle globalUBO, bool buildTLAS)
{
    GPUScene* scene = resources.scene;
    CHECK(renderer);
    CHECK(scene);
    if (resources.hasDynamicGeometry)
    {
        renderer->CreatePass(
            "Dynamic Geometry Upload", RHIDeviceQueueType::Graphics, 0u,
            [=](PassHandle self, Renderer* r)
            {
                r->BindBufferCopySrc(self, resources.dynamicStagingBuffer);
                r->BindBufferCopyDst(self, resources.dynamicPrimitiveBuffer);
            },
            [=](PassHandle, Renderer*, RHICommandList* cmd) { scene->UploadDynamicGeometry(cmd); });
    }
    if (buildTLAS && resources.tlas != kInvalidHandle)
    {
        renderer->CreatePass(
            "TLAS/BLAS Update", RHIDeviceQueueType::Graphics, 0u,
            [=](PassHandle self, Renderer* r)
            {
                r->BindAccelerationStructureWrite(self, resources.tlas);
                if (resources.hasDynamicGeometry)
                    r->BindBufferShaderRead(self, resources.dynamicPrimitiveBuffer,
                                            RHIPipelineStageBits::AccelerationBuild);
            },
            [=](PassHandle, Renderer*, RHICommandList* cmd)
            {
                if (resources.hasDynamicGeometry)
                    scene->BuildBLAS(cmd);
                (void)scene->BuildTLAS(cmd, true);
            });
    }

    struct LightBVHRefitPush
    {
        uint32_t firstNodeOffset;
        uint32_t nodeCount;
    };
    renderer->CreatePass(
        "Light BVH Refit Leaves", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindBufferUniform(self, globalUBO, RHIPipelineStageBits::ComputeShader, "globalParams");
            r->BindBufferStorageRead(self, resources.lightBuffer, RHIPipelineStageBits::ComputeShader, "lights");
            r->BindBufferUnordered(self, resources.lightBVHNodeBuffer, RHIPipelineStageBits::ComputeShader,
                                   "lightBVHNodes");
            r->BindBufferStorageRead(self, resources.lightBVHLightIndexBuffer, RHIPipelineStageBits::ComputeShader,
                                     "lightBVHLightIndices");
            r->BindBufferStorageRead(self, resources.lightBVHNodeIndexBuffer, RHIPipelineStageBits::ComputeShader,
                                     "gNodeIndices");
            r->BindShader(self, RHIShaderStageBits::Compute, "updateLeafNodes",
                          PathsResolve("Data/Shaders/ECSLightBVHRefit.spv"));
            r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(LightBVHRefitPush));
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            if (!scene->NeedsLightBVHRefit())
                return;
            uint32_t levels = scene->GetLightBVHRefitLevelCount();
            if (levels == 0u)
                return;
            uint32_t leafLevel = levels - 1u;
            uint32_t count = scene->GetLightBVHRefitLevelNodeCount(leafLevel);
            if (count == 0u)
                return;
            LightBVHRefitPush pc{
                scene->GetLightBVHFirstNodeIndex() + scene->GetLightBVHRefitLevelOffset(leafLevel), count};
            r->CmdSetPipeline(self, cmd);
            r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, pc);
            cmd->Dispatch((count + 255u) / 256u, 1, 1);
        });
    renderer->CreatePass(
        "Light BVH Refit Internals", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindBufferUniform(self, globalUBO, RHIPipelineStageBits::ComputeShader, "globalParams");
            r->BindBufferStorageRead(self, resources.lightBuffer, RHIPipelineStageBits::ComputeShader, "lights");
            r->BindBufferUnordered(self, resources.lightBVHNodeBuffer, RHIPipelineStageBits::ComputeShader,
                                   "lightBVHNodes");
            r->BindBufferStorageRead(self, resources.lightBVHLightIndexBuffer, RHIPipelineStageBits::ComputeShader,
                                     "lightBVHLightIndices");
            r->BindBufferStorageRead(self, resources.lightBVHNodeIndexBuffer, RHIPipelineStageBits::ComputeShader,
                                     "gNodeIndices");
            r->BindShader(self, RHIShaderStageBits::Compute, "updateInternalNodes",
                          PathsResolve("Data/Shaders/ECSLightBVHRefit.spv"));
            r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(LightBVHRefitPush));
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            if (!scene->NeedsLightBVHRefit())
                return;
            uint32_t levels = scene->GetLightBVHRefitLevelCount();
            if (levels < 2u)
                return;
            r->CmdSetPipeline(self, cmd);
            for (int level = static_cast<int>(levels) - 2; level >= 0; --level)
            {
                uint32_t count = scene->GetLightBVHRefitLevelNodeCount(static_cast<uint32_t>(level));
                if (count == 0u)
                    continue;
                LightBVHRefitPush pc{
                    scene->GetLightBVHFirstNodeIndex() +
                        scene->GetLightBVHRefitLevelOffset(static_cast<uint32_t>(level)),
                    count};
                r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, pc);
                cmd->Dispatch((count + 255u) / 256u, 1, 1);
                if (level > 0)
                {
                    cmd->BeginTransition();
                    cmd->SetBufferTransition(
                        scene->GetLightBVHNodeBuffer(),
                        {.srcAccess = RHIResourceAccessBits::ShaderWrite,
                         .dstAccess = RHIResourceAccessBits::ShaderRead | RHIResourceAccessBits::ShaderWrite,
                         .srcStage = RHIPipelineStageBits::ComputeShader,
                         .dstStage = RHIPipelineStageBits::ComputeShader});
                    cmd->EndTransition();
                }
            }
        });
}
