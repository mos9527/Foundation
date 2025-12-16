#include "ImGui.hpp"
#include "Renderer.hpp"
#include <RenderUtils/CSClearBuffer.hpp>
#include <RenderUtils/CSMipGeneration.hpp>
#include <RenderUtils/PSFullscreen.hpp>

void PathTracerSetup(FContext* context, RendererConfig cfg, RendererScene scene)
{
    auto* renderer = context->renderer = Construct<Renderer>(context->allocator,
                                                             RendererDesc{
                                                                 .asyncCompute = true,
                                                                 .pipelineCache = context->psoCache.Get(),
                                                             },
                                                             context->device, context->swapchain, context->allocator);
    auto* gpu = context->gpuScene;
    renderer->BeginSetup();
    auto TLAS = renderer->CreateResource("Scene TLAS", gpu->GetTLAS());
    renderer->CreatePass(
        "TLAS Update", RHIDeviceQueueType::Graphics, 0u, [=](PassHandle self, Renderer* r)
        {
            r->BindAccelerationStructureWrite(self, TLAS);
        }, [=](PassHandle, Renderer* r, RHICommandList* cmd)
        {
            gpu->BuildTLAS(cmd, *scene.gsInstances, *scene.gsBLASes, true);
        });
    renderer->CreatePass(
        "Trace", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindAccelerationStructureSRV(self, TLAS, RHIPipelineStageBits::RayTracingShader, "AS");
            r->BindBackbufferUAV(self, 1u);
            r->BindShader(self, RHIShaderStageBits::RayGeneration, "RayGen", "data/shaders/ERTPathTracer.spv");
        }, [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            RHIExtent2D wh = r->GetSwapchainExtent();
            r->CmdSetPipeline(self, cmd);
            cmd->TraceRays(wh.x, wh.y, 1);
        });
    ImGui_ImplFoundation_CreatePass(renderer, "ImGui", false, FSetupDefault{});
    renderer->EndSetup();
}
