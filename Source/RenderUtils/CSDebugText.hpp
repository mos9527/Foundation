#pragma once
#include <RenderCore/Renderer.hpp>
namespace Foundation::RenderUtils
{
    using namespace RenderCore;
#pragma pack(push,1)
    struct CSDebugTextData
    {
        int x = 0u, y = 0u, scale = 2u, col32 = ~0u;
        union
        {
            uint32_t u[28]; // (128-4*3)/4
            char ch[28 * 4];
        } text{};
    };
#pragma pack(pop)
    /**
     * @breif Draw debug text overlay on top of the existing backbuffer content
     *        This is a port of https://github.com/zeux/niagara/blob/master/src/shaders/debugtext.comp.glsl
     */
    inline auto* createCSDebugTextPassBackBuffer(Renderer* r, StringView name, Span<const CSDebugTextData> lines)
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
                for (auto const& line : lines)
                {
                    if (size_t len = strlen(line.text.ch))
                    {
                        r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, line);
                        cmd->Dispatch(len,1,1);
                    }
                }
            });
    }
} // namespace Foundation::RenderUtils
