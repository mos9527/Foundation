#include <../Source/RenderUtils/UploadContext.hpp>
#include <Bindings/ImGui.hpp>
#include <Rendering/CSMipGeneration.hpp>
#include <Rendering/Textureration.hpp>
#include "Examples.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
namespace Examples
{
    /**
     * @example MipGeneration.cpp
     * Generate mipmaps for a texture using compute shaders, with ImGui and image loading.
     */
    class MipGenerationApp : public RenderApplication
    {
    public:
        // Q: Why is a custom destructor needed?
        // A: TexturePool resource got imported into the @ref Renderer - see @ref Renderer::createResource
        //    We need to ensure that the TexturePool outlives the Renderer.
        UniquePtr<TexturePool> mTexturePool;
        ~MipGenerationApp() override
        {
            mRenderer.reset();
            mTexturePool.reset();
            // Follows Device destruction, etc.
        }
        TexturePoolHandle mSampleImage{kInvalidTexturePoolHandle};
        ResourceHandle mRenderedSRV{kInvalidHandle};
        float mBlur = 1.0f;
        void OnDeviceSetup() override
        {
            ImGui_ImplFoundation_SetupContextWithDefaultStyles();
            ImGui_ImplFoundation_Init(mDevice.Get(), GetNativeWindow(), mAlloc.Ptr());
            mTexturePool = ConstructUnique<TexturePool>(GetAllocator(), mDevice.Get(), GetAllocator());
            // Load into mip 0
            UploadContext upload(mDevice.Get(), GetAllocator());
            int x, y, n;
            stbi_uc* data = stbi_load("data/assets/cameraman.jpg", &x, &y, &n, 4);
            mSampleImage = mTexturePool->Allocate(
                RHITextureDesc{.usage = RHITextureUsageBits::SampledImage | RHITextureUsageBits::StorageImage |
                                   RHITextureUsageBits::TransferDestination,
                               .extent = {x, y, 1},
                               .format = RHIResourceFormat::R8G8B8A8Unorm,
                               .mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::min(x, y)))) + 1u});
            Span<char> span(reinterpret_cast<char*>(data), reinterpret_cast<char*>(data) + x * y * 4);
            upload.Upload(mTexturePool->GetTexture(mSampleImage), span);
            upload.SubmitAndWait();
        }
        void OnRendererSetup() override
        {
            auto* srcTex = mTexturePool->GetTexture(mSampleImage);
            ResourceHandle srcHandle = createResource(mRenderer.get(), "Image", srcTex);
            ResourceHandle renderedHandle =
                createResource(mRenderer.get(), "Rendered Image",
                               RHITextureDesc{
                                   .usage = RHITextureUsageBits::SampledImage | RHITextureUsageBits::RenderTarget,
                                   .extent = srcTex->mDesc.extent,
                                   .format = RHIResourceFormat::R8G8B8A8Unorm,
                               });
            ResourceHandle sampler =
                createSampler(mRenderer.get(),{});
            createCSMipGenerationPasses(mRenderer.get(), "Mip Gen", RHIDeviceQueueType::Compute, srcHandle, srcHandle,
                                        srcTex->mDesc.extent, RHITextureAspectFlagBits::Color,
                                        RHIResourceFormat::R8G8B8A8Unorm, RHITextureAspectFlagBits::Color,
                                        RHIResourceFormat::R8G8B8A8Unorm, 16, 0);
            createPass(
                mRenderer.get(), "Blur", RHIDeviceQueueType::Graphics,
                [=](PassHandle self, Renderer* r)
                {
                    r->BindShader(self, RHIShaderStageBits::Vertex, "vertMain", "data/shaders/VSFullscreen.spv");
                    r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", "data/shaders/MipGenerationBlur.spv");
                    r->BindTextureSRV(self, srcHandle, "srcTexture", RHIPipelineStageBits::FragmentShader,
                                      {.format = RHIResourceFormat::R8G8B8A8Unorm,
                                       .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color, 0,
                                                                                   srcTex->mDesc.mipLevels)});
                    r->BindTextureSampler(self, sampler, "srcSampler");
                    r->BindTextureRTV(
                        self, renderedHandle,
                        {.format = RHIResourceFormat::R8G8B8A8Unorm, .range = RHITextureSubresourceRange::Create()});
                    r->BindPushConstant(self, RHIShaderStageBits::Fragment, 0, sizeof(float));
                },
                [=, this](PassHandle self, Renderer* r, RHICommandList* cmd)
                {
                    auto const& img_wh = r->DerefResource(renderedHandle).Get<RHITexture*>()->mDesc.extent;
                    r->CmdBeginGraphics(self, cmd, img_wh, {}, {});
                    r->CmdSetPipeline(self, cmd);
                    r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Fragment, 0, mBlur);
                    cmd->SetViewport(0, 0, img_wh.x, img_wh.y).SetScissor(0, 0, img_wh.x, img_wh.y);
                    cmd->Draw(3);
                    cmd->EndGraphics();
                });
            ImGui_ImplFoundation_CreatePass(mRenderer.get(), "ImGui", true,
                                            [=, this](PassHandle self, Renderer* r)
                                            {
                                                // Add a dependency here to ensure the blur pass is executed.
                                                // Beware - ImGui_ImplFoundation_AddImage does not do this.
                                                mRenderedSRV =
                                                    r->BindTextureSRV(self, renderedHandle, kBindpointIgnored,
                                                                      RHIPipelineStageBits::FragmentShader,
                                                                      {
                                                                          .format = RHIResourceFormat::R8G8B8A8Unorm,
                                                                          .range = RHITextureSubresourceRange::Create(),
                                                                      });
                                            });
        }
        ImTextureID mRenderedImageID{0};
        void OnRendererPostSetup() override
        {
            if (mRenderedImageID != 0)
                ImGui_ImplFoundation_RemoveImage(mRenderedImageID);
            auto* srv = mRenderer->DerefTextureView(mRenderedSRV);
            mRenderedImageID = ImGui_ImplFoundation_AddImage(srv, ImGuiImplFoundationImageSamplerLinear);
        }
        void OnBeforeFrame() override
        {
            ImGui_ImplFoundation_NewFrame();
            ImGui::NewFrame();
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
            ImGui::Begin("Viewer");
            ImGui::Image(mRenderedImageID, ImVec2(512, 512));
            ImGui::SliderFloat("Blur", &mBlur, 0.0f, 10.0f);
            ImGui::End();
        }
    };

} // namespace Examples
int main(int argc, char** argv)
{
    Examples::MipGenerationApp app;
    app.Initialize<VulkanApplication>({.windowTitle = "Mipmap Generation"});
    app.RunForever();
    ImGui_ImplFoundation_Shutdown();
}
