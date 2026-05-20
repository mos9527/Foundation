#pragma once
#include <Core/Paths.hpp>
#include <Math/Math.hpp>
#include <RenderCore/Renderer.hpp>
namespace Foundation::RenderUtils
{
    using namespace RenderCore;
#pragma pack(push,1)
    struct CSClearBufferData
    {
        float4 color;
        uint32_t w, h;
    };
#pragma pack(pop)
    inline PassHandle createCSClearBackBuffer(Renderer* r, StringView name, float4 clearColor = {})
    {
        return r->CreatePass(
            name, RHIDeviceQueueType::Graphics, 0u,
            [=](PassHandle self, Renderer* r)
            {
                r->BindBackbufferUAV(self, 0u);
                r->BindShader(self, RHIShaderStageBits::Compute, "main", Foundation::Core::PathsResolve("Data/Shaders/CSClearBuffer.spv"));
                r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(CSClearBufferData));
            },
            [=](PassHandle self, Renderer* r, RHICommandList* cmd)
            {
                RHIExtent2D wh = r->GetSwapchainExtent();
                CSClearBufferData cdata{clearColor, wh.x, wh.y};
                r->CmdSetPipeline(self, cmd);
                r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, cdata);
                r->CmdDispatch(self, cmd, {cdata.w, cdata.h, 1});
            });
    }
    inline PassHandle createCSClearTexture(Renderer* r, StringView name, ResourceHandle resource, RHITextureViewDesc viewDesc, float4 clearColor)
    {
        return r->CreatePass(
            name, RHIDeviceQueueType::Graphics, 0u,
            [=](PassHandle self, Renderer* r)
            {
                r->BindTextureUAV(self, resource, "texture", RHIPipelineStageBits::ComputeShader, viewDesc);
                r->BindShader(self, RHIShaderStageBits::Compute, "main", Foundation::Core::PathsResolve("Data/Shaders/CSClearBuffer.spv"));
                r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(CSClearBufferData));
            },
            [=](PassHandle self, Renderer* r, RHICommandList* cmd)
            {
                RHIExtent2D wh = r->GetSwapchainExtent();
                CSClearBufferData cdata{clearColor, wh.x, wh.y};
                r->CmdSetPipeline(self, cmd);
                r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, cdata);
                r->CmdDispatch(self, cmd, {cdata.w, cdata.h, 1});
            });
    }
} // namespace Foundation::RenderUtils
