#include <Math/Math.hpp>
#include "Examples.hpp"
using namespace Foundation::Math;
constexpr size_t kNumTextures = 16;
/*-- Getting some good-looking images on the CPU --*/
static uint32_t to_rgba8_unorm(vec4 c)
{
    union
    {
        uint32_t col;
        char rgba[4];
    } color;
    color.rgba[0] = clamp(c.x * 255.0f, 0.f, 255.f);
    color.rgba[1] = clamp(c.y * 255.0f, 0.f, 255.f);
    color.rgba[2] = clamp(c.z * 255.0f, 0.f, 255.f);
    color.rgba[3] = clamp(c.w * 255.0f, 0.f, 255.f);
    return color.col;
};
// http://en.wikipedia.org/wiki/HSL_and_HSV#HSL_to_RGB_alternative
static vec4 hsl_to_rgb(float h /* [0,360] */, float s /* [0,1] */, float l /* [0,1] */)
{
    float a = s * min(l, 1 - l);
    auto f = [=](float n)
    {
        int k = static_cast<int>(n + h / 30) % 12;
        return l - a * std::max(-1, std::min({k - 3, 9 - k, 1}));
    };
    return { f(0),f(8),f(4), 1};
}
// GitHub Identicon from https://github.com/dgraham/identicon ported by LLM
Array<uint32_t, 8*8> identicon(Array<unsigned char, 16> const& hash)
{
    Array<uint32_t, 8*8> img;
    uint32_t background = to_rgba8_unorm({240.0/255,240.0/255,240.0/255,1});
    // The Rust code uses the last 4 bytes for color.
    // Use last 28 bits to determine HSL values.
    uint16_t h1 = (static_cast<uint16_t>(hash[12]) & 0x0F) << 8;
    uint16_t h2 = static_cast<uint16_t>(hash[13]);

    uint32_t h_val = h1 | h2; // 12-bit value for hue
    uint32_t s_val = hash[14];
    uint32_t l_val = hash[15];
    auto map = [](float value, float vmin, float vmax, float dmin, float dmax) {
        if (vmax - vmin == 0) return dmin; // Avoid division by zero
        return dmin + (dmax - dmin) * ((value - vmin) / (vmax - vmin));
    };
    // Map hash values to HSL ranges
    float hue = map(h_val, 0, 4095, 0, 360);
    float sat_modifier = map(s_val, 0, 255, 0, 20);
    float lum_modifier = map(l_val, 0, 255, 0, 20);

    // Create the foreground color from HSL values, then convert to RGB
    // The ranges (65-sat, 75-lum) ensure good contrast and pleasant colors.
    uint32_t foreground = to_rgba8_unorm(hsl_to_rgb(hue, (65.0f - sat_modifier) / 100.0f, (75.0f - lum_modifier) / 100.0f));
    for (int y = 0; y < 8; ++y) {
        // Mirrored
        for (int x = 0; x < 4; ++x) {
            int i = y * 4 + x;
            const uint8_t byte = hash[i / 2];
            const uint8_t nibble = (i % 2 == 0)
                                   ? (byte >> 4)
                                   : (byte & 0x0F);
            bool margin = y <= 0 || y >= 7 || x <= 0;
            bool paint = (nibble % 2 == 0) && !margin;
            uint32_t color = paint ? foreground : background;
            img[y * 8 + x] = color;
            img[y * 8 + (7 - x)] = color;
        }
    }
    return img;
}
/*-- Actual example --*/
#include <../Source/RenderUtils/TexturePool.hpp>
#include <../Source/RenderUtils/UploadContext.hpp>
#include <random>
namespace Examples
{
    /**
     * @example TexturePool.cpp
     * Bindless texture pool example
     * @example Shaders/TexturePool.slang
     * Simple shader to display the texture pool contents with animation.
     */
    class TexturePoolApp : public RenderApplication
    {
        UniquePtr<TexturePool> mTexturePool;
        Vector<TexturePoolHandle> mHandles;
        struct PushConstant
        {
            float time;
            uint32_t num_textures;
            uint32_t first_texture;
        };
    public:
        TexturePoolApp() : mHandles(GetAllocator()) {};
        void OnDeviceSetup() override
        {
            mTexturePool = ConstructUnique<TexturePool>(GetAllocator(), mDevice.Get(), GetAllocator());
            // Perform immediate uploads
            UploadContext upload(mDevice.Get(), GetAllocator());
            for (size_t i = 0; i < kNumTextures; ++i)
            {
                mHandles.push_back(mTexturePool->Allocate(RHITextureDesc{
                    .usage = RHITextureUsageBits::SampledImage | RHITextureUsageBits::TransferDestination,
                    .extent = { 8, 8, 1 },
                    .format = RHIResourceFormat::R8G8B8A8Unorm
                }));
                auto texture = mTexturePool->GetTexture(mHandles.back());
                Array<unsigned char, 16> hash;
                // Randomly fill the hash
                std::mt19937 rng{ static_cast<unsigned int>(i) };
                std::uniform_int_distribution<> dist(0, 255);
                for (int j = 0; j < 16; ++j) hash[j] = static_cast<unsigned char>(dist(rng));
                auto art = identicon(hash);
                // This won't block - the transfers are waited on @ref UploadContext destruction
                upload.Upload(texture, Span<uint32_t>(art).AsBytes());
            }
        }
        void OnRendererSetup() override
        {
            ResourceHandle sampler = createSampler(mRenderer.get(), {
                .filter = {
                    .minFilter = RHIDeviceSampler::SamplerDesc::Filter::NearestNeighbor,
                    .magFilter = RHIDeviceSampler::SamplerDesc::Filter::NearestNeighbor
                }
            });
            createPSFullscreenPass(
                mRenderer.get(), "Texture Pool Atlas",
                [=, this](PassHandle self, Renderer* r)
                {
                    r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", "data/shaders/TexturePool.spv");
                    r->BindPushConstant(self, RHIShaderStageBits::Fragment, 0, sizeof(PushConstant));
                    r->BindTextureSampler(self, sampler, "sampler");
                    r->BindDescriptorSet(self, "textures" , mTexturePool->GetDescriptorSet(), mTexturePool->GetDescriptorSetLayout());
                },
                [=, this](PassHandle self, Renderer* r, RHICommandList* cmd)
                {
                    r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Fragment, 0, PushConstant{
                        .time = GetApplicationTime(),
                        .num_textures = kNumTextures,
                        .first_texture = 1 // 0 is reserved for the default 'missing' texture
                    });
                });
        }
    };

} // namespace Examples
int main(int argc, char** argv)
{
    Examples::TexturePoolApp app;
    app.Initialize<VulkanApplication>({.windowTitle = "Texture Pool"});
    app.RunForever();
}
