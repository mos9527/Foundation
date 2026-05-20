#pragma once
#include <Core/Paths.hpp>
#include <RenderCore/Renderer.hpp>
namespace Foundation::RenderUtils
{
    using namespace RenderCore;
#pragma pack(push,1)
    struct CSDebugTextData
    {
        int x = 0u, y = 0u, scale = 2u, col32 = -1;
        char szText[28 * 4]; // Zero terminated

        void SetText(StringView str)
        {
            size_t len = std::min(str.size(), sizeof(szText) - 1);
            std::memcpy(szText, str.data(), len);
            szText[len] = '\0';
        }
        int SetColor(int r, int g, int b, int a = 255)
        {
            return col32 = a << 24 | b << 16 | g << 8 | r;
        }
    };
#pragma pack(pop)
    /**
     * @breif Draw debug text overlay on top of the existing backbuffer content
     *        This is a port of https://github.com/zeux/niagara/blob/master/src/shaders/debugtext.comp.glsl
     */
    inline PassHandle createCSDebugTextPassBackBuffer(Renderer* r, StringView name, Span<const CSDebugTextData> lines)
    {
        return r->CreatePass(
            name, RHIDeviceQueueType::Graphics, 0u,
            [=](PassHandle self, Renderer* r)
            {
                r->BindBackbufferUAV(self, 0u);
                r->BindShader(self, RHIShaderStageBits::Compute, "debugText", Foundation::Core::PathsResolve("Data/Shaders/CSDebugText.spv"));
                r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(CSDebugTextData));
            },
            [=](PassHandle self, Renderer* r, RHICommandList* cmd)
            {
                r->CmdSetPipeline(self, cmd);
                for (auto const& line : lines)
                {
                    if (size_t len = strlen(line.szText))
                    {
                        r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, line);
                        cmd->Dispatch(len,1,1);
                    }
                }
            });
    }
} // namespace Foundation::RenderUtils
