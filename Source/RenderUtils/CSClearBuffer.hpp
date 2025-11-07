#pragma once
#include <Math/Math.hpp>
#include <RenderCore/Renderer.hpp>
namespace Foundation::RenderUtils
{
    using namespace RenderCore;
#pragma pack(push,1)
    struct CSClearBufferData
    {
        Math::float4 color;
        uint32_t w, h;
    };
#pragma pack(pop)
    inline auto* createCSClearBackBuffer(Renderer* r, StringView name, Math::float4 clearColor = {})
    {
        return r->CreatePass(
            name, RHIDeviceQueueType::Graphics, 0u,
            [=](PassHandle self, Renderer* r)
            {
                r->BindBackbufferUAV(self, 0u);
                r->BindShader(self, RHIShaderStageBits::Compute, "debugText", "data/shaders/CSClearBuffer.spv");
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
