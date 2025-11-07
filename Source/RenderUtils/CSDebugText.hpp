#pragma once
#include <RenderCore/Renderer.hpp>
namespace Foundation::RenderUtils
{
    using namespace RenderCore;
#pragma pack(push,1)
    struct CSDebugTextData
    {
        int x, y, w, h;
        char text[64];
    };
#pragma pack(pop)
    inline auto* createCSDebugTextPassBackBuffer(Renderer* r, StringView name, CSDebugTextData* pData)
    {
        return r->CreatePass(
            name, RHIDeviceQueueType::Graphics, 0u,
            [=](PassHandle self, Renderer* r)
            {
                r->BindBackbufferUAV(self, 0u);
                r->BindShader(self, RHIShaderStageBits::Compute, "debugText", "data/shaders/CSDebugText.spv");
                r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(CSDebugTextData));
            },
            [=](PassHandle self, Renderer* r, RHICommandList* cmd)
            {
                r->CmdSetPipeline(self, cmd);
                r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, *pData);
                r->CmdDispatch(self, cmd, {pData->w,pData->h, 1});
            });
    }
} // namespace Foundation::RenderUtils
