#include <Core/Paths.hpp>
#include <Core/JobGraph.hpp>
#include <algorithm>
#include <filesystem>
#include <cctype>
#include <limits>
#include <numbers>

#include "EditorState.hpp"
#include <Fonts/PlexSansIcon.h>
#include <Renderer/Mesh.hpp>
#include <Renderer/Animation.hpp>
#include "Renderer/GPUScene.hpp"
#include "Renderer/Postprocess.hpp"
#include <Math/Decompose.hpp>
#include <imgui.h>
#include <tracy/Tracy.hpp>
#include <cmath>
static constexpr const char* kTempScenePath = "Cache/Last.fscn";

static String sDeferredScenePath;
static String sDeferredEnvMapPath;

static bool PathsReferToSameFile(StringView a, StringView b)
{
    std::error_code ec;
    if (std::filesystem::equivalent(a.data(), b.data(), ec))
        return true;
    ec.clear();
    auto const canonA = std::filesystem::weakly_canonical(a.data(), ec);
    if (ec)
        return false;
    ec.clear();
    auto const canonB = std::filesystem::weakly_canonical(b.data(), ec);
    if (ec)
        return false;
    return canonA == canonB;
}

// Drop the editor's read-only mapping when we need to rewrite the same payload file (e.g. the
// glTF import temp cache while a previous import still has it open).
static void ReleaseScenePayloadFileForRewrite(StringView payloadPath)
{
    if (!GEditor.sceneFile.has_value())
        return;
    if (!PathsReferToSameFile(GEditor.currentSavePath, payloadPath))
        return;
    GEditor.scene.reset();
    GEditor.sceneFile.reset();
}
static constexpr size_t kDefaultSceneLoadScratchBudget = 64ull * (1ull << 20);
// Accounts for alignment, etc...
static constexpr size_t kBudgetSlack = 1ull * (1ull << 20);
static constexpr size_t kStagingBudgetSlack = 32ull * (1ull << 20);

// Fills the GPUScene-owned instance/material/light table spans from the editor scene
// (geometry handles + texture remap live in EditorState), then commits them.
static GPUScene::UpdateResult BuildSceneTables(GPUScene* gpu, FImportedScene& scene);

static GPUScene::UpdateResult CommitSceneToGPU(GPUScene* gpu, FImportedScene& scene, RendererUBO& globals,
                                               bool resetAccumulation)
{
    CHECK(gpu);

    auto res = BuildSceneTables(gpu, scene);
    gpu->BuildUBO(globals);
    if (resetAccumulation)
        globals.ptAccumulatedFrames = 0;
    return res;
}

void CommitSceneToGPU(bool resetAccumulation)
{
    if (!GContext->gpuScene || !GEditor.HasScene())
        return;

    CommitSceneToGPU(GContext->gpuScene, GEditor.Scene(), GEditor.shaderGlobals, resetAccumulation);
}

static FTexture LoadViewLUT(StringView path, Allocator* alloc = GLOBAL_ALLOC)
{
    CHECK(alloc != nullptr);
    FTexture texture(alloc);
    LoadDDS(texture, PathsResolve(path));
    return texture;
}

static bool HasDDSExtension(StringView path)
{
    String ext = std::filesystem::path(path.data()).extension().string();
    for (auto& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".dds";
}

static FTexture LoadMatcapTexture(StringView path, Allocator* alloc = GLOBAL_ALLOC)
{
    CHECK(alloc != nullptr);
    FTexture texture(alloc);
    String resolvedPath = PathsResolve(path);
    if (HasDDSExtension(resolvedPath))
        LoadDDS(texture, resolvedPath);
    else
        LoadRGBA8(texture, resolvedPath, false);
    return texture;
}

static void LoadSelectedViewLUTs(FTexture& sdr, FTexture& hdr, int& sdrIndex, int& hdrIndex,
                                 String const& sdrExternalPath, String const& hdrExternalPath,
                                 Allocator* alloc = GLOBAL_ALLOC)
{
    CHECK(alloc != nullptr);
    String sdrPath = Postprocess::ResolveSelectedViewLUTPath(Postprocess::ViewLUTDomain::SDR,
                                                             sdrIndex, sdrExternalPath);
    String hdrPath = Postprocess::ResolveSelectedViewLUTPath(Postprocess::ViewLUTDomain::HDR,
                                                             hdrIndex, hdrExternalPath);

    sdr = LoadViewLUT(sdrPath, alloc);
    hdr = LoadViewLUT(hdrPath, alloc);
}

static void LoadSelectedViewLUTs(FTexture& sdr, FTexture& hdr)
{
    LoadSelectedViewLUTs(sdr, hdr, GEditor.viewLUTSdrIndex, GEditor.viewLUTHdrIndex,
                         GEditor.viewLUTSdrExternalPath, GEditor.viewLUTHdrExternalPath);
}

static void UploadEditorViewLUTs(GPUScene* gpu, FTexture const& sdr, FTexture const& hdr)
{
    CHECK(gpu != nullptr);
    GPUScene::Result sdrResult = gpu->Upload(sdr, GEditor.viewLUTSdrHandle, "View LUT SDR", true);
    GPUScene::Result hdrResult = gpu->Upload(hdr, GEditor.viewLUTHdrHandle, "View LUT HDR", true);
    CHECK_MSG(sdrResult == GPUScene::Result::Ready, "Failed to upload SDR view LUT ({})", static_cast<int>(sdrResult));
    CHECK_MSG(hdrResult == GPUScene::Result::Ready, "Failed to upload HDR view LUT ({})", static_cast<int>(hdrResult));
}

String ResolveMatcapPath(int& index, String const& externalPath)
{
    int const externalIndex = kMatcapCount; // Last entry reserved for external path
    if (index == externalIndex && !externalPath.empty())
        return externalPath;

    if (index < 0 || index >= externalIndex)
        index = 0;
    return kMatcaps[index].path;
}

static void LoadSelectedMatcap(FTexture& matcap, int& matcapIndex, String const& externalPath,
                               Allocator* alloc = GLOBAL_ALLOC)
{
    CHECK(alloc != nullptr);
    CHECK(matcapIndex >= 0 && matcapIndex <= kMatcapCount + 1);
    String matcapPath = ResolveMatcapPath(matcapIndex, externalPath);
    matcap = LoadMatcapTexture(matcapPath, alloc);
}

static void LoadSelectedMatcap(FTexture& matcap)
{
    LoadSelectedMatcap(matcap, GEditor.matcapIndex, GEditor.matcapExternalPath);
}

static void UploadEditorMatcap(GPUScene* gpu, FTexture const& matcap)
{
    CHECK(gpu != nullptr);
    GPUScene::Result matcapResult = gpu->Upload(matcap, GEditor.matcapHandle, "Matcap", true);
    CHECK_MSG(matcapResult == GPUScene::Result::Ready, "Failed to upload matcap ({})", static_cast<int>(matcapResult));
    GEditor.shaderGlobals.matcapTextureIndex = GEditor.matcapHandle.index;
}

static double MillisecondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

static bool IsSceneEnvironmentTexture(FImportedScene const& scene, size_t textureIndex)
{
    FLight const* environment = scene.GetEnvironmentLight();
    return environment != nullptr &&
        environment->environmentMap &&
        environment->environmentTexture != kInvalidTexture &&
        textureIndex == environment->environmentTexture;
}

static size_t SceneTextureReadBudget(FSerializedTexture const& source)
{
    CHECK_MSG(source.IsValid(), "Serialized texture is invalid");
    size_t budget = static_cast<size_t>(source.GetSize());
    for (FBlobRef const& blob : source.subresources)
    {
        if (blob.codec != FBlobCodec::None)
            budget += static_cast<size_t>(blob.decodedSize);
    }
    return std::max<size_t>(AlignUp(budget + kBudgetSlack, alignof(std::max_align_t)), alignof(std::max_align_t));
}

static FTexture ReadSceneTexture(FImportedScene const& scene, FSerializedTexture const& source, Allocator* alloc)
{
    CHECK(alloc != nullptr);
    CHECK_MSG(source.IsValid(), "Serialized texture is invalid");

    FTexture texture(alloc);
    texture.magic = source.magic;
    texture.header = source.header;
    texture.header10 = source.header10;
    texture.bytes.resize(texture.GetSize());
    for (uint32_t layer = 0; layer < source.GetNumLayers(); ++layer)
    {
        for (uint32_t mip = 0; mip < source.GetNumMips(); ++mip)
        {
            Span<unsigned char> dst = texture.GetSubresource(mip, layer);
            FBlobRef const& blob = source.GetSubresourceBlob(layer, mip);
            CHECK_MSG(blob.decodedSize == dst.size_bytes(),
                      "Serialized texture subresource size mismatch: layer {}, mip {}, blob {}, expected {}",
                      layer, mip, blob.decodedSize, dst.size_bytes());
            CHECK(scene.ReadBlob(blob, dst.data(), dst.size_bytes(), alloc));
        }
    }
    return texture;
}

bool ApplyViewLUTSelection()
{
    if (!GContext->gpuScene)
    {
        LOG(Editor, LogWarn, "Ignoring view LUT selection before a scene is loaded");
        return false;
    }
    try
    {
        FTexture sdr(GLOBAL_ALLOC);
        FTexture hdr(GLOBAL_ALLOC);
        LoadSelectedViewLUTs(sdr, hdr);
        UploadEditorViewLUTs(GContext->gpuScene, sdr, hdr);
    }
    catch (std::exception const& e)
    {
        LOG(Editor, LogError, "Failed to apply view LUT selection: {}", e.what());
        return false;
    }
    catch (...)
    {
        LOG(Editor, LogError, "Failed to apply view LUT selection");
        return false;
    }

    return true;
}

bool ApplyMatcapSelection()
{
    if (!GContext->gpuScene)
    {
        LOG(Editor, LogWarn, "Ignoring matcap selection before a scene is loaded");
        return false;
    }
    try
    {
        FTexture matcap(GLOBAL_ALLOC);
        LoadSelectedMatcap(matcap);
        UploadEditorMatcap(GContext->gpuScene, matcap);
    }
    catch (std::exception const& e)
    {
        LOG(Editor, LogError, "Failed to apply matcap selection: {}", e.what());
        return false;
    }
    catch (...)
    {
        LOG(Editor, LogError, "Failed to apply matcap selection");
        return false;
    }
    return true;
}

FTexture ReadbackRenderTexture(RHITexture* source, Allocator* alloc)
{
    CHECK(source != nullptr);
    CHECK(alloc != nullptr);
    RHITextureDesc const& desc = source->mDesc;
    CHECK_MSG(desc.format != RHIResourceFormat::Undefined, "Cannot read back texture with undefined format");
    CHECK_MSG(desc.sampleCount == RHIMultisampleCount::E1, "Cannot read back multisampled texture");

    FTexture texture(alloc);
    texture.Initialize(desc.format, desc.dimension, desc.extent.x, desc.extent.y, desc.extent.z,
                       desc.mipLevels, desc.arrayLayers);
    texture.bytes.resize(texture.GetSize());

    uint32_t const alignment = std::max(texture.GetBpp() / 8, texture.GetBlockSize());
    CHECK_MSG(alignment != 0, "Unsupported texture format {}", texture.GetFormat());
    CHECK_MSG(texture.bytes.size() <= std::numeric_limits<size_t>::max() - (alignment - 1u),
              "Texture readback staging capacity exceeds addressable range");
    size_t const readbackCapacity = texture.bytes.size() + alignment - 1u;

    ImmediateReadback readback(GContext->device.Get(), readbackCapacity);
    readback.Begin();
    auto* cmd = readback.ctx.Get();
    auto range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color, 0, desc.mipLevels,
                                                    0, desc.arrayLayers);
    cmd->BeginTransition();
    cmd->SetImageTransition(source,
                            {.srcAccess = RHIResourceAccessBits::ShaderRead,
                             .dstAccess = RHIResourceAccessBits::TransferRead,
                             .srcStage = RHIPipelineStageBits::FragmentShader,
                             .dstStage = RHIPipelineStageBits::Transfer,
                             .srcImgLayout = RHITextureLayout::ShaderReadOnly,
                             .dstImgLayout = RHITextureLayout::TransferSrc,
                             .srcImgRange = range});
    cmd->EndTransition();

    struct ReadbackSlice
    {
        char* data;
        size_t offset;
        size_t size;
    };
    Vector<ReadbackSlice> slices(alloc);
    slices.reserve(static_cast<size_t>(desc.arrayLayers) * desc.mipLevels);
    for (uint32_t layer = 0; layer < desc.arrayLayers; ++layer)
    {
        for (uint32_t mip = 0; mip < desc.mipLevels; ++mip)
        {
            if (!readback.Align(alignment))
                CHECK_MSG(false, "Readback staging alignment failed");
            Span<unsigned char> subresource = texture.GetSubresource(mip, layer);
            RHIExtent3D mipExtent = texture.GetMipExtent(mip);
            char* data = readback.Readback(source, subresource.size(),
                                           {.aspect = RHITextureAspectFlagBits::Color,
                                            .mipLevel = mip,
                                            .baseArrayLayer = layer,
                                            .layerCount = 1},
                                           {}, mipExtent);
            CHECK_MSG(data != nullptr, "Readback staging buffer exhausted");
            slices.push_back({data, static_cast<size_t>(subresource.data() - texture.bytes.data()), subresource.size()});
        }
    }

    cmd->BeginTransition();
    cmd->SetImageTransition(source,
                            {.srcAccess = RHIResourceAccessBits::TransferRead,
                             .dstAccess = RHIResourceAccessBits::ShaderRead,
                             .srcStage = RHIPipelineStageBits::Transfer,
                             .dstStage = RHIPipelineStageBits::FragmentShader,
                             .srcImgLayout = RHITextureLayout::TransferSrc,
                             .dstImgLayout = RHITextureLayout::ShaderReadOnly,
                             .srcImgRange = range});
    cmd->EndTransition();
    readback.End();
    readback.WaitIdle();

    for (ReadbackSlice const& slice : slices)
        std::memcpy(texture.bytes.data() + slice.offset, slice.data, slice.size);
    return texture;
}

static Vector<float> CombineRenderTextures(Span<const FTexture> textures, uint32_t& outWidth, uint32_t& outHeight,
                                         Allocator* alloc = GLOBAL_ALLOC)
{
    CHECK(alloc != nullptr);
    CHECK_MSG(!textures.empty(), "Invalid render texture count");    
    for (FTexture const& texture : textures)
    {
        uint32_t width = texture.GetWidth();
        uint32_t height = texture.GetHeight();
        CHECK_MSG(texture.GetFormat() == RHIResourceFormat::R32G32B32A32SignedFloat, "Invalid render texture format for readback combine (got {}). RGBA32F is expected.", texture.GetFormat());
        CHECK_MSG(width == outWidth && height == outHeight, "Mismatched render readback texture extents (got {}x{}, expected {}x{})", width, height, outWidth, outHeight);
    }
    size_t const pixelCount = static_cast<size_t>(outWidth) * outHeight;
    size_t const componentCount = pixelCount * 4;
    Vector<float> combined(componentCount, alloc);
    for (FTexture const& texture : textures)
    {
        auto const* rgba = reinterpret_cast<const float*>(texture.bytes.data());
        for (size_t i = 0; i < componentCount; ++i)
            combined[i] += rgba[i];
    }
    for (size_t i = 0; i < pixelCount; ++i)
        combined[i * 4 + 3] = 1.0f;
    return combined;
}

void DoRenderReadback(RendererOutputs const& outputs)
{
    auto* renderer = GContext->renderer;
    uint32_t w = 0;
    uint32_t h = 0;

    if (GEditor.renderTask.format == ERenderFormat::HDR)
    {
        Vector<FTexture> hdrTextures(GLOBAL_ALLOC);
        auto PushHDR = [&](ResourceHandle handle)
        {
            if (handle == kInvalidHandle)
                return;
            auto* texture = renderer->DerefResource(handle).Get<RHITexture*>();
            hdrTextures.push_back(ReadbackRenderTexture(texture, GLOBAL_ALLOC));
        };
        PushHDR(outputs.diffuse);
        if (outputs.specular != kInvalidHandle && outputs.specular != outputs.diffuse)
            PushHDR(outputs.specular);
        CHECK_MSG(!hdrTextures.empty(), "Invalid HDR readback outputs");
        // AOVs are provided (without alpha), combine them into one HDR buffer
        w = hdrTextures[0].GetWidth(), h = hdrTextures[0].GetHeight();
        const Vector<float> combined = CombineRenderTextures(hdrTextures, w, h);
        const char* hdrPath = GEditor.renderTask.outputPath.c_str();
        SaveHDR(combined.data(), static_cast<int>(w), static_cast<int>(h), hdrPath);
        LOG(Editor, LogInfo, "{} HDR image saved to {} ({}x{}, {} frames)",
            GEditor.rendererMode == ERendererMode::PathTracer ? "Path tracer" : "Raster", hdrPath, w, h,
            GEditor.shaderGlobals.ptAccumulatedFrames);
    } else
    {
        CHECK_MSG(GEditor.postprocessOutput != kInvalidHandle, "Invalid SDR readback texture");
        auto sdrTexture = renderer->DerefResource(GEditor.postprocessOutput).Get<RHITexture*>();
        const FTexture sdr = ReadbackRenderTexture(sdrTexture, GLOBAL_ALLOC);        
        const char* sdrPath = GEditor.renderTask.outputPath.c_str();
        // Output as is
        CHECK_MSG(sdr.GetFormat() == RHIResourceFormat::R8G8B8A8Unorm, "Invalid SDR readback texture format. Is HDR currently enabled?");
        w = sdr.GetWidth(), h = sdr.GetHeight();
        SavePNG(sdr.bytes.data(), static_cast<int>(w), static_cast<int>(h), sdrPath);
        LOG(Editor, LogInfo, "{} SDR image saved to {} ({}x{}, {} frames)",
            GEditor.rendererMode == ERendererMode::PathTracer ? "Path tracer" : "Raster", sdrPath, w, h,
            GEditor.shaderGlobals.ptAccumulatedFrames);
    }
}

static float SpotLightImportanceSolidAngle(float cosInner, float cosOuter)
{
    float cosFalloffStart = std::clamp(std::max(cosInner, cosOuter), -1.0f, 1.0f);
    float cosTotalWidth = std::clamp(std::min(cosInner, cosOuter), -1.0f, cosFalloffStart);
    // Matches the shader's quartic falloff integral over the penumbra.
    return 2.0f * std::numbers::pi_v<float> *
           ((1.0f - cosFalloffStart) + (cosFalloffStart - cosTotalWidth) / 5.0f);
}

static float LightColorImportance(float3 color)
{
    return std::max(0.0f, std::max(color.x, std::max(color.y, color.z)));
}

static float AreaLightFluxImportance(GSLight const& light)
{
    float area = 1.0f;
    if (light.type == static_cast<uint32_t>(FLightType::Disk))
        area = std::numbers::pi_v<float> * light.radius.x * light.radius.y;
    else if (light.type == static_cast<uint32_t>(FLightType::Rect))
        area = 4.0f * cross(light.dpdu, light.dpdv).length();

    float sides = light.twoSided != 0 ? 2.0f : 1.0f;
    return light.power * area * std::numbers::pi_v<float> * sides;
}

static void FLightToGSLight(FLight const& src, GSLight& dst, GPUScene const* gpu,
                            GPUScene::LightSamplerType sampler)
{
    dst.type = static_cast<uint32_t>(src.type);
    dst.color = src.color;
    dst.power = src.power;
    dst.range = src.range;
    // Position from transform
    dst.position = src.transform.transform;
    // Direction from rotation (default forward is (0,0,-1))
    dst.direction = normalize(src.transform.rotation * float3(0, 0, -1));
    dst.angularDiameter = src.angularDiameter;
    dst.spotInnerCosAngle = std::cos(src.spotInnerConeAngle);
    dst.spotOuterCosAngle = std::cos(src.spotOuterConeAngle);    
    // Area light fields
    float areaWidth = std::max(src.width, 1e-6f);
    float areaHeight = std::max(src.height, 1e-6f);
    dst.radius = float2(areaWidth, areaHeight);
    dst.twoSided = src.twoSided ? 1u : 0u;
    // Build tangent frame from direction for area lights
    if (src.type == FLightType::Disk || src.type == FLightType::Rect)
    {
        float3 n = dst.direction;
        float3 u, v;
        CoordinateSystem(n, u, v);
        if (src.type == FLightType::Disk)
        {
            dst.dpdu = u; // Unit tangent; radius is separate
            dst.dpdv = v;
        }
        else // Rect
        {
            dst.dpdu = u * areaWidth;  // half-extent along u
            dst.dpdv = v * areaHeight; // half-extent along v
        }

        if (src.normalize)
        {
            float area;
            if (src.type == FLightType::Disk)
                area = std::numbers::pi_v<float> * areaWidth * areaHeight;
            else
                area = 4.0f * areaWidth * areaHeight;

            float totalArea = src.twoSided ? (2.0f * area) : area;
            // For a Lambertian emitter, total flux Phi = L * A * pi per emitting side.
            dst.power = src.power / (totalArea * std::numbers::pi_v<float>);
        }
    }
    if (src.type == FLightType::Environment)
    {
        bool const hasEnvMap = src.environmentMap && gpu && gpu->HasEnvMap();
        dst.color = hasEnvMap ? float3(1.0f) : src.color;
        dst.power = src.power;
        dst.twoSided = hasEnvMap ? 1u : 0u;
        dst.envAzimuthOffset = src.environmentAzimuthOffset;
    }
    // Importance used by the light alias table.
    float importance = 1.0f;
    if (sampler == GPUScene::LightSamplerType::Importance)
    {
        /* proj into area? still hopeful nontheless. let's get to light bvh one day... for now make these
         * light weight more just because it _may_ contribute to most of the scene. */
        constexpr float kEnvDirectionalImportance = 10.0f;
        switch (src.type)
        {
        case FLightType::Environment:
            importance = LightColorImportance(dst.color) * dst.power * kEnvDirectionalImportance;
			break;
        case FLightType::Directional:
            importance = dst.power * kEnvDirectionalImportance;
            break;
        case FLightType::Point:
            importance = dst.power * 4.0f * std::numbers::pi_v<float>;
            break;
        case FLightType::Spot:
            importance = dst.power * SpotLightImportanceSolidAngle(dst.spotInnerCosAngle, dst.spotOuterCosAngle);
            break;
        case FLightType::Disk:
        case FLightType::Rect:
            importance = AreaLightFluxImportance(dst);
            break;
        }
        importance *= LightColorImportance(dst.color);
    }
    dst.importance = std::max(0.0f, importance);
}

static void FillGSMaterial(GSMaterial& dst, FMaterial const& src, Vector<TextureHandle> const& textureIDMap,
                           GPUScene* gpu)
{
    // While the scene streams in, textures that aren't resident yet remap to UINT32_MAX (the
    // same "no texture" sentinel unset slots use), so shaders sample defaults until the real
    // image lands. Re-committing each frame (PumpSceneLoad) swaps in textures as they pop in.
    auto RemapTextureIndex = [&](uint32_t index) -> uint32_t {
        if (index == UINT32_MAX || index >= textureIDMap.size())
            return UINT32_MAX;
        TextureHandle const handle = textureIDMap[index];
        if (!handle.IsValid() || gpu->Query(handle) != GPUScene::Result::Ready)
            return UINT32_MAX;
        return handle.index;
    };
    dst.baseColorFactor = src.baseColorFactor;
    // emissiveFactor.w is the editor's emissive intensity; bake it into the GPU RGB.
    dst.emissiveFactor = float3(src.emissiveFactor) * src.emissiveFactor.w;
    dst.metallicFactor = src.metallicFactor;
    dst.roughnessFactor = src.roughnessFactor;
    dst.baseColorTexture = RemapTextureIndex(src.baseColorTexture);
    dst.emissiveTexture = RemapTextureIndex(src.emissiveTexture);
    dst.metallicRoughnessTexture = RemapTextureIndex(src.metallicRoughnessTexture);
    dst.normalTexture = RemapTextureIndex(src.normalTexture);
    dst.normalScale = src.normalScale;
    dst.transmissionTexture = RemapTextureIndex(src.transmissionTexture);
    dst.specularTexture = RemapTextureIndex(src.specularTexture);
    dst.specularColorTexture = RemapTextureIndex(src.specularColorTexture);
    dst.anisotropyTexture = RemapTextureIndex(src.anisotropyTexture);
    dst.sheenColorTexture = RemapTextureIndex(src.sheenColorTexture);
    dst.sheenRoughnessTexture = RemapTextureIndex(src.sheenRoughnessTexture);
    dst.clearcoatTexture = RemapTextureIndex(src.clearcoatTexture);
    dst.clearcoatRoughnessTexture = RemapTextureIndex(src.clearcoatRoughnessTexture);
    dst.transmissionFactor = src.transmissionFactor;
    dst.ior = src.ior;
    dst.specularFactor = src.specularFactor;
    dst.specularColorFactor = src.specularColorFactor;
    dst.anisotropyStrength = src.anisotropyStrength;
    dst.anisotropyRotation = src.anisotropyRotation;
    dst.sheenColorFactor = src.sheenColorFactor;
    dst.sheenRoughnessFactor = src.sheenRoughnessFactor;
    dst.clearcoatFactor = src.clearcoatFactor;
    dst.clearcoatRoughnessFactor = src.clearcoatRoughnessFactor;
    dst.subsurfaceFactor = src.subsurfaceFactor;
    dst.subsurfaceScale = src.subsurfaceScale;
    dst.subsurfaceColor = src.subsurfaceColor;
    dst.subsurfaceRadius = src.subsurfaceRadius;
    dst.shaderBlockID = static_cast<uint32_t>(src.shaderBlockID);
    dst.hairBetaM = src.hairBetaM;
    dst.hairBetaN = src.hairBetaN;
    dst.hairAlpha = src.hairAlpha;
}

// Fills the GPUScene-owned mapped table spans directly from the editor scene. Scene
// semantics (geometry handle binding, material/texture remap, light conversion) stay
// in the editor; GPUScene only owns the mapped ring memory and commit.
static GPUScene::UpdateResult BuildSceneTables(GPUScene* gpu, FImportedScene& scene)
{
    auto instances = scene.GetInstances();
    auto materials = scene.GetMaterials();
    scene.EnsureEnvironmentLight();
    auto lights = scene.GetLights();
    CHECK_MSG(instances.size() <= UINT32_MAX && materials.size() <= UINT32_MAX && lights.size() <= UINT32_MAX,
              "Scene table exceeds uint32_t range");
    // The serialized environment light is always the first scene light. PT samples it through
    // the same alias table as authored lights; raster treats it as simple ambient fallback.
    auto tables = gpu->BeginScene(static_cast<uint32_t>(instances.size()),
                                  static_cast<uint32_t>(materials.size()),
                                  static_cast<uint32_t>(lights.size()));
    for (size_t i = 0; i < instances.size(); ++i)
    {
        auto const& src = instances[i];
        GeometryHandle geometry;
        if (src.type == FInstanceType::Mesh)
        {
            CHECK_MSG(src.resourceIndex < GEditor.meshGeometry.size(),
                      "Mesh instance references invalid mesh {}", src.resourceIndex);
            geometry = GEditor.meshGeometry[src.resourceIndex];
        }
        else if (src.type == FInstanceType::Curve)
        {
            CHECK_MSG(src.resourceIndex < GEditor.curveGeometry.size(),
                      "Curve instance references invalid curve {}", src.resourceIndex);
            geometry = GEditor.curveGeometry[src.resourceIndex];
        }
        else
            CHECK_MSG(false, "Unknown scene instance type {}", static_cast<uint32_t>(src.type));
        tables.instances[i] = InstanceDesc{
            .geometry = geometry,
            .transform = src.transform.transform,
            .rotation = src.transform.rotation,
            .scale = src.transform.scale,
            .materialIndex = src.materialIndex,
        };
    }
    for (size_t i = 0; i < materials.size(); ++i)
        FillGSMaterial(tables.materials[i], materials[i], GEditor.textureIDMap, gpu);
    for (size_t i = 0; i < lights.size(); ++i)
        FLightToGSLight(lights[i], tables.lights[i], gpu, gpu->mLightSamplerType);
    return gpu->EndScene(tables);
}

void UpdateSceneLights()
{
    if (!GEditor.HasScene() || !GContext->gpuScene)
        return;
    CommitSceneToGPU(false);
}

void DeleteSelectedInstance()
{
    if (!GContext->gpuScene || !GEditor.HasScene())
        return;
    int const idx = GEditor.selectedInstance;
    auto& sceneInstances = GEditor.Scene().mTables.instances;
    if (idx < 0 || idx >= static_cast<int>(sceneInstances.size()))
        return;

    sceneInstances.erase(sceneInstances.begin() + idx);
    GEditor.selectedInstance = -1;
    GEditor.selectedMaterial = -1;
    // Recommit the (now smaller) instance table, then reclaim geometry no longer
    // referenced. Wait for the GPU to finish before reclaiming resident resources.
    CommitSceneToGPU(true);
    GContext->device->WaitIdle();
    GContext->gpuScene->Collect();
}

static void ApplySceneCamera(FImportedScene const& scene, FArcballCamera& cameraState,
                             CameraApertureState& apertureState, RendererUBO& globals)
{
    auto cameras = scene.GetCameras();
    if (cameras.empty())
        return;

    auto& camera = cameras.front();
    cameraState.rot = camera.transform.rotation;
    cameraState.radius = length(camera.transform.transform);
    cameraState.center = {};
    cameraState.fovY = camera.fovY;
    apertureState.dofEnabled = camera.lensEnabled;
    if (camera.lensEnabled)
    {
        apertureState.sensorHeightMm = camera.sensorHeightMm;
        apertureState.fStop = camera.fStop;
        globals.focalDistance = camera.focusDistance;
        globals.apertureBlades = camera.apertureBlades;
        globals.apertureRotation = camera.apertureRotation;
        globals.apertureRatio = camera.apertureRatio;
    }
}

static size_t GPUSceneBudgetBytes(GPUSceneDesc const& desc)
{
    return size_t(desc.primitiveBudget) +
           size_t(desc.curveAABBBudget) +
           size_t(desc.instanceBudget) * sizeof(GSInstance) +
           size_t(desc.materialBudget) * sizeof(GSMaterial) +
           size_t(desc.lightBudget) * sizeof(GSLight) +
           size_t(desc.lightBudget) * sizeof(GSAlias) +
           size_t(desc.tlasInstanceBudget) * GContext->device->WriteAccelerationStructureInstanceData({}, nullptr) +
           size_t(desc.tlasBudget) +
           size_t(desc.tlasScratchBudget);
}

static GPUScene* CreateGPUScene(FImportedScene const& scene, size_t& outBudgetBytes)
{
    auto estimatedBudget = scene.CalculateGPUSceneDesc(GContext->device->GetCapabilities());
    // Some more lights to play with
    estimatedBudget.lightBudget = std::max(estimatedBudget.lightBudget, 1024u);
    LOG(Editor, LogDebug,
        "Estimated GPUScene budget: primitive {} MB, curve AABB {} MB, instances {}, TLAS instances {}, materials {}, lights {}, textures {}",
        estimatedBudget.primitiveBudget / (1u << 20),
        estimatedBudget.curveAABBBudget / (1u << 20),
        estimatedBudget.instanceBudget,
        estimatedBudget.tlasInstanceBudget,
        estimatedBudget.materialBudget,
        estimatedBudget.lightBudget,
        estimatedBudget.texturesBudget);

    outBudgetBytes = GPUSceneBudgetBytes(estimatedBudget);

    auto lightSamplerType = GPUScene::LightSamplerType::Importance;
    if (GContext->gpuScene)
        lightSamplerType = GContext->gpuScene->mLightSamplerType;

    auto* gpu = Construct<GPUScene>(GContext->allocator, GContext->device.Get(), GContext->allocator,
                                    estimatedBudget, GContext->editorFrameScratch.get());
    gpu->mLightSamplerType = lightSamplerType;
    return gpu;
}

static void DestroyGPUScene(GPUScene*& gpu)
{
    if (!gpu)
        return;
    if (gpu == GContext->gpuScene)
        ClearMaterialTexturePreviewCache();
    Destruct(GContext->allocator, gpu);
    gpu = nullptr;
}

static String PrepareScenePayloadFile(StringView path)
{
    auto ext = std::filesystem::path(path.data()).extension().string();
    for (auto& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".fscn")
        return String(path);

    // GLTF import would commit to a file first too.
    // In this way we don't need to have the whole scene allocated in memory
    // explicitly - and works the same way as loading from FSCN.
    String scenePayloadPath = PathsResolve(kTempScenePath);
    ReleaseScenePayloadFileForRewrite(scenePayloadPath);
    std::filesystem::path scenePayloadDir = std::filesystem::path(scenePayloadPath).parent_path();
    if (!scenePayloadDir.empty())
        std::filesystem::create_directories(scenePayloadDir);
    Allocator* importScratch = GLOBAL_ALLOC;
    MemoryMappedFile sceneFile(scenePayloadPath, 64ull * 1024ull * 1024ull /* grows on demand */);
    FImportedScene writeScene(sceneFile, importScratch);
    LoadScene(path, writeScene, importScratch);
    return scenePayloadPath;
}

struct SceneLoadStats
{
    size_t sceneMeshCount = 0;
    size_t sceneInstanceCount = 0;
    size_t sceneCurveCount = 0;
    size_t sceneMaterialCount = 0;
    size_t sceneGpuBudget = 0;
    double uploadMs = 0.0;
};

// Applies the new scene's editor globals (exposure, ambient, env, view LUT selection).
// Deferred to install time so it doesn't perturb the currently-rendering scene while a
// background load is in flight.
static void ApplySceneGlobals(FImportedScene const& scene)
{
    auto const& sceneGlobals = scene.GetSceneGlobals();
    GEditor.shaderGlobals.camEV = sceneGlobals.postExposure;
    GEditor.viewLUTSdrIndex = static_cast<int>(sceneGlobals.viewLutSdrIndex);
    GEditor.viewLUTHdrIndex = static_cast<int>(sceneGlobals.viewLutHdrIndex);
    GEditor.viewLUTSdrExternalPath.clear();
    GEditor.viewLUTHdrExternalPath.clear();
    GEditor.viewLUTSdrHandle = {};
    GEditor.viewLUTHdrHandle = {};
    GEditor.matcapIndex = {};
    GEditor.matcapExternalPath.clear();
    GEditor.matcapHandle = {};
    GEditor.shaderGlobals.matcapTextureIndex = UINT32_MAX;
}

static void InitializeSceneLoad(FImportedScene& scene, SceneLoadStats& stats, GPUScene*& newGPUScene)
{
    stats.sceneMeshCount = scene.GetMeshes().size();
    stats.sceneInstanceCount = scene.GetInstances().size();
    stats.sceneCurveCount = scene.GetCurves().size();
    stats.sceneMaterialCount = scene.GetMaterials().size();
    newGPUScene = CreateGPUScene(scene, stats.sceneGpuBudget);
}

// Submits every scene geometry/texture to GPUScene's work queue. The queue drains on a
// background thread (kicked by the first Poll() in PumpSceneLoad) while the editor renders
// the (already installed) scene: instances stream into the TLAS as their geometry becomes
// resident, and materials fall back to default textures until theirs land.
static void BeginSceneUpload(FImportedScene& scene, GPUScene* gpu, Vector<GeometryHandle>& meshGeometry,
                             Vector<GeometryHandle>& curveGeometry, Vector<TextureHandle>& textureIDMap)
{
    LOG(Editor, LogInfo, "Uploading new scene data to GPU");
    FBlobDeserializer blobs = scene.GetBlobDeserializer();

    // Geometry uploads: each returns a resident handle, recorded by resource index so
    // BuildSceneTables can bind instances to geometry. These maps stay in the pending
    // load (not GEditor) until install, so edits to the still-current scene aren't
    // mismatched against the loading one.
    meshGeometry.clear();
    curveGeometry.clear();
    meshGeometry.reserve(scene.GetMeshes().size());
    curveGeometry.reserve(scene.GetCurves().size());
    for (FSerializedMesh const& mesh : scene.GetMeshes())
    {
        GeometryHandle handle;
        // Deforming meshes (skinned or morph-animated) upload as dynamic (CPU-updateable) geometry
        // (Ready synchronously); the rest stream in via the background worker (InProgress).
        bool const dynamic = mesh.skinBinding.count != 0 || mesh.morphTrack >= 0;
        GPUScene::Result r = dynamic ? gpu->UploadDynamic(&blobs, mesh, handle) : gpu->Upload(&blobs, mesh, handle);
        CHECK_MSG(r == GPUScene::Result::InProgress || r == GPUScene::Result::Ready,
                  "Mesh upload rejected ({})", static_cast<int>(r));
        meshGeometry.push_back(handle);
    }
    for (FSerializedCurve const& curve : scene.GetCurves())
    {
        GeometryHandle handle;
        GPUScene::Result r = gpu->Upload(&blobs, curve, handle);
        CHECK_MSG(r == GPUScene::Result::InProgress, "Curve upload rejected ({})", static_cast<int>(r));
        curveGeometry.push_back(handle);
    }

    // Texture uploads (the environment map is handled separately in finalize).
    textureIDMap.assign(scene.GetTextures().size(), TextureHandle{});
    for (size_t textureIndex = 0; textureIndex < scene.GetTextures().size(); ++textureIndex)
    {
        FSerializedTexture const& srcDesc = scene.GetTextures()[textureIndex];
        if (!srcDesc.IsValid() || IsSceneEnvironmentTexture(scene, textureIndex))
            continue;
        TextureHandle handle;
        GPUScene::Result r = gpu->Upload(&blobs, srcDesc, handle);
        CHECK_MSG(r == GPUScene::Result::InProgress, "Texture {} upload rejected ({})", textureIndex,
                  static_cast<int>(r));
        textureIDMap[textureIndex] = handle;
    }
    // The queued geometry/textures drain lazily on the first Poll() (PumpSceneLoad),
    // on a background thread, while the editor renders the freshly installed scene.
}

// Applies the scene's globals + camera and uploads the environment map + view LUTs. These
// are small and synchronous (UploadEnvMap / Upload(FTexture) for LUTs), so they run
// BEFORE the heavy geometry/textures are queued: that keeps the synchronous drain limited
// to env + LUTs and leaves the bulk of the scene to stream in on the background worker.
static void PrepareSceneGlobals(FImportedScene& scene, GPUScene* gpu, AllocatorStack& sceneAlloc)
{
    ApplySceneGlobals(scene);
    FLight const* environment = scene.GetEnvironmentLight();
    bool const hasEnvironmentTexture = environment != nullptr &&
        environment->environmentMap &&
        environment->environmentTexture != kInvalidTexture;

    if (hasEnvironmentTexture)
    {
        CHECK_MSG(environment->environmentTexture < scene.GetTextures().size(),
                  "Scene environment texture index out of range");
        FSerializedTexture const& environmentTextureDesc = scene.GetTextures()[environment->environmentTexture];
        ScopedArena environmentArena(GLOBAL_ALLOC, SceneTextureReadBudget(environmentTextureDesc));
        CHECK(environmentArena);
        AllocatorStack environmentAlloc(environmentArena);
        FTexture environmentTexture = ReadSceneTexture(scene, environmentTextureDesc, &environmentAlloc);
        CHECK_MSG(environmentTexture.GetFormat() == RHIResourceFormat::R32G32B32A32SignedFloat,
                  "Scene environment texture must be RGBA32F, got {}", environmentTexture.GetFormat());
        gpu->UploadEnvMap(environmentTexture);
    }
    {
        FTexture sdr(&sceneAlloc);
        FTexture hdr(&sceneAlloc);
        LoadSelectedViewLUTs(sdr, hdr, GEditor.viewLUTSdrIndex, GEditor.viewLUTHdrIndex,
                             GEditor.viewLUTSdrExternalPath, GEditor.viewLUTHdrExternalPath, &sceneAlloc);
        UploadEditorViewLUTs(gpu, sdr, hdr);
    }
    {
        FTexture matcap(&sceneAlloc);
        LoadSelectedMatcap(matcap, GEditor.matcapIndex, GEditor.matcapExternalPath, &sceneAlloc);
        UploadEditorMatcap(gpu, matcap);
    }

    ApplySceneCamera(scene, GEditor.camera, GEditor.aperture, GEditor.shaderGlobals);
    gpu->BuildUBO(GEditor.shaderGlobals);
}


namespace { void ClearAnimationRuntime(); } // defined with the skinning runtime below

static void InstallLoadedScene(String const& scenePayloadPath, GPUScene*& newGPUScene)
{
    GContext->device->WaitIdle();
    ClearAnimationRuntime(); // drop runtime referencing the scene/GPUScene we're about to replace
    DestroyEditorRenderer(GContext);
    DestroyGPUScene(GContext->gpuScene);
    GContext->gpuScene = newGPUScene;
    newGPUScene = nullptr;
    GEditor.OpenSceneFile(scenePayloadPath);
    GEditor.selectedInstance = -1;
    GEditor.selectedMaterial = -1;
    GEditor.selectedLight = -1;
    GEditor.cameraUpdated = true;
    GEditor.state = FERunningEnter;
}

struct PendingSceneLoad
{
    String scenePayloadPath;
    Optional<ScopedArena> arena;
    Optional<AllocatorStack> alloc;
    Optional<MemoryMappedFile> file;
    Optional<FImportedScene> scene;
    SceneLoadStats stats;
    std::chrono::steady_clock::time_point loadStart;
    String envMapPath; // optional HDRI to load once the scene finishes streaming
    // Scene->geometry/texture bindings (also published into GEditor at install).
    Vector<GeometryHandle> meshGeometry{GLOBAL_ALLOC};
    Vector<GeometryHandle> curveGeometry{GLOBAL_ALLOC};
    Vector<TextureHandle> textureIDMap{GLOBAL_ALLOC};
};
static PendingSceneLoad* sPendingSceneLoad = nullptr;

static void DestroyPendingSceneLoad()
{
    if (!sPendingSceneLoad)
        return;
    Destruct(GLOBAL_ALLOC, sPendingSceneLoad);
    sPendingSceneLoad = nullptr;
}

// Routes a matching HDRI to load after the in-flight scene finishes streaming (its
// UploadEnvMap would otherwise Join the background drain). Falls back to loading
// immediately if no scene load is pending (e.g. it failed to start).
static void DeferEnvMapForPendingLoad(String const& envMapPath)
{
    if (sPendingSceneLoad)
        sPendingSceneLoad->envMapPath = envMapPath;
    else
        LoadEnvMap(envMapPath);
}

// Joins the background drain (which holds pointers into the load's mapped file) and frees
// the load's backing memory. Safe to call once Poll() has reported Ready/failed, or to
// force-complete a still-streaming load before starting another.
static void FinishPendingSceneLoad()
{
    if (!sPendingSceneLoad)
        return;
    if (GContext->gpuScene)
        GContext->gpuScene->Join();
    DestroyPendingSceneLoad();
}

// --- Skinned animation playback ---------------------------------------------------------------
// CPU skinning runtime for the installed scene. Skinned meshes were uploaded as dynamic geometry;
// each frame we evaluate their skeleton pose from the scene's clips and linear-blend-skin the
// rest-pose vertices into the dynamic ring (see Renderer/Animation.hpp). Rebuilt on every load.
namespace
{
struct DynamicMeshRuntime
{
    GeometryHandle handle{};
    Vector<FVertex> bind;         // dequantized rest-pose vertices
    // Skin (optional)
    int32_t skeleton{-1};
    Vector<FSkinBinding> binding; // per-vertex joints/weights (parallel to bind)
    // Morph (optional)
    int32_t morphTrack{-1};
    uint32_t morphTargetCount{0};
    Vector<float3> morphDeltas;   // POSITION deltas, target-major (t*vtxCount + v)
    Vector<uint32_t> indices;     // LOD0 topology, for recomputing normals after morphing
    explicit DynamicMeshRuntime(Allocator* alloc) : bind(alloc), binding(alloc), morphDeltas(alloc), indices(alloc) {}
};
struct AnimationRuntime
{
    Vector<DynamicMeshRuntime> meshes{GLOBAL_ALLOC};
    Vector<FPose> poses{GLOBAL_ALLOC}; // one per scene skeleton (reused each frame)
    // Clips that drive each skeleton, indexed by skeleton. Pre-bucketed at setup so each skeleton's
    // pose can be sampled independently (no shared write target), enabling parallel pose evaluation.
    Vector<Vector<uint32_t>> skeletonClips{GLOBAL_ALLOC};
    // Per-worker scratch (outer index == ParallelFor workerId) so meshes skin in parallel without
    // sharing buffers. Inner vectors are pre-sized to the largest mesh/skeleton to avoid per-frame
    // reallocation; each invocation uses only the prefix it needs.
    Vector<Vector<mat4>> skins{GLOBAL_ALLOC};      // skinning matrices
    Vector<Vector<FVertex>> morphs{GLOBAL_ALLOC}; // morphed base verts before skinning
    Vector<Vector<float>> morphWeights{GLOBAL_ALLOC};  // sampled morph weights
    // Rigid node animation: the scene-node skeleton index (-1 if none) and, per scene node, whether
    // it is animated or descends from an animated node (only those instances/lights get overridden).
    int32_t sceneNodeSkeleton{-1};
    Vector<uint8_t> nodeAffected{GLOBAL_ALLOC};
    bool rigidDrivesTransforms{false}; // some instance/light references an animated node (static fact)
    // Node of the first scene camera when it is animated (the editor view follows it), else -1. Only
    // the first camera matters: that's the one ApplySceneCamera feeds to the view.
    int32_t cameraNode{-1};
    float time{0.0f};
    float duration{0.0f}; // longest clip/morph track (seconds), for the UI timeline
    bool playing{true};
    bool dirty{false}; // a scrub requested a one-shot pose apply even while paused
    [[nodiscard]] bool HasRigid() const { return sceneNodeSkeleton >= 0; }
    [[nodiscard]] bool HasData() const { return !meshes.empty() || HasRigid(); }
};
AnimationRuntime sAnimation;
// Debug toggle: force serial CPU deformation. Persists across scene loads (not part of sAnimation).
bool sAnimateParallel = true;

void ClearAnimationRuntime() { sAnimation = AnimationRuntime{}; }

// Builds the skinning runtime from the freshly installed scene: dequantizes each skinned mesh's
// rest pose, reads its per-vertex binding, and allocates per-skeleton pose scratch.
void SetupAnimationRuntime()
{
    ClearAnimationRuntime();
    if (!GEditor.HasScene())
        return;
    FImportedScene& scene = GEditor.Scene();
    auto const& skeletons = scene.mTables.skeletons;

    FBlobDeserializer blobs = scene.GetBlobDeserializer();
    auto meshes = scene.GetMeshes();
    uint32_t maxJoints = 0;
    for (auto const& skel : skeletons)
        maxJoints = std::max(maxJoints, skel.Count());
    sAnimation.poses.resize(skeletons.size());

    // Rigid node animation: mark nodes that are animated or descend from an animated node, so the
    // per-frame override only touches moving instances/lights (and a held pose lets PT converge).
    sAnimation.sceneNodeSkeleton = scene.mTables.sceneNodeSkeleton;
    if (sAnimation.sceneNodeSkeleton >= 0)
    {
        FSkeleton const& nodes = skeletons[sAnimation.sceneNodeSkeleton];
        sAnimation.nodeAffected.assign(nodes.Count(), 0u);
        for (FAnimationClip const& clip : scene.mTables.clips)
            if (clip.skeleton == sAnimation.sceneNodeSkeleton)
                for (FAnimChannel const& channel : clip.channels)
                    if (channel.joint < nodes.Count())
                        sAnimation.nodeAffected[channel.joint] = 1u;
        // Topological storage (parent < child) lets one forward pass propagate to descendants.
        for (uint32_t i = 0; i < nodes.Count(); ++i)
        {
            int32_t parent = nodes.joints[i].parent;
            if (parent >= 0 && sAnimation.nodeAffected[static_cast<uint32_t>(parent)])
                sAnimation.nodeAffected[i] = 1u;
        }
        // Whether rigid animation actually moves any committed transform is static, so resolve it once
        // here (rather than racing on a shared flag while applying transforms in parallel each frame).
        auto isAffected = [&](int32_t node)
        { return node >= 0 && static_cast<size_t>(node) < sAnimation.nodeAffected.size() && sAnimation.nodeAffected[node]; };
        for (FInstance const& instance : scene.mTables.instances)
            if ((sAnimation.rigidDrivesTransforms = isAffected(instance.node)))
                break;
        if (!sAnimation.rigidDrivesTransforms)
            for (FLight const& light : scene.mTables.lights)
                if ((sAnimation.rigidDrivesTransforms = isAffected(light.node)))
                    break;
        // The editor view follows the first scene camera, so only that one needs tracking. Camera
        // motion drives the view (path-tracer reset), not a GPUScene commit, so it stays out of
        // rigidDrivesTransforms.
        if (!scene.mTables.cameras.empty() && isAffected(scene.mTables.cameras.front().node))
            sAnimation.cameraNode = scene.mTables.cameras.front().node;
    }
    for (size_t m = 0; m < meshes.size(); ++m)
    {
        FSerializedMesh const& mesh = meshes[m];
        bool const skinned = mesh.skinBinding.count != 0 && mesh.skeleton >= 0;
        bool const morphed = mesh.morphTrack >= 0 && mesh.morphTargetCount > 0;
        if ((!skinned && !morphed) || m >= GEditor.meshGeometry.size())
            continue;
        DynamicMeshRuntime rt(GLOBAL_ALLOC);
        rt.handle = GEditor.meshGeometry[m];
        Vector<FQVertex> quantized = blobs.ReadArray<FQVertex>(mesh.vertices, GLOBAL_ALLOC);
        rt.bind.resize(quantized.size());
        for (size_t i = 0; i < quantized.size(); ++i)
            rt.bind[i] = FQVertex::Unpack(quantized[i]);
        if (skinned)
        {
            rt.skeleton = mesh.skeleton;
            rt.binding = blobs.ReadArray<FSkinBinding>(mesh.skinBinding, GLOBAL_ALLOC);
        }
        if (morphed)
        {
            rt.morphTrack = mesh.morphTrack;
            rt.morphTargetCount = mesh.morphTargetCount;
            rt.morphDeltas = blobs.ReadArray<float3>(mesh.morphPositions, GLOBAL_ALLOC);
            // Topology is needed to recompute normals from the morphed positions each frame.
            if (!mesh.lods.empty())
                rt.indices = blobs.ReadArray<uint32_t>(mesh.lods[0].indices, GLOBAL_ALLOC);
        }
        sAnimation.meshes.push_back(std::move(rt));
    }

    // Per-worker scratch, pre-sized to the largest mesh/skeleton so skinning jobs never reallocate.
    uint32_t maxVerts = 0, maxTargets = 0;
    for (DynamicMeshRuntime const& rt : sAnimation.meshes)
    {
        maxVerts = std::max(maxVerts, static_cast<uint32_t>(rt.bind.size()));
        maxTargets = std::max(maxTargets, rt.morphTargetCount);
    }
    size_t const lanes = GContext->jobs->GetParallelForConcurrency();
    sAnimation.skins.resize(lanes, Vector<mat4>{GLOBAL_ALLOC});
    sAnimation.morphs.resize(lanes, Vector<FVertex>{GLOBAL_ALLOC});
    sAnimation.morphWeights.resize(lanes, Vector<float>{GLOBAL_ALLOC});
    for (size_t l = 0; l < lanes; ++l)
    {
        sAnimation.skins[l].assign(maxJoints, mat4(1.0f));
        sAnimation.morphs[l].resize(maxVerts);
        sAnimation.morphWeights[l].resize(maxTargets);
    }

    // Bucket clips by the skeleton they drive so the per-skeleton pose pass is independent.
    sAnimation.skeletonClips.assign(skeletons.size(), Vector<uint32_t>{GLOBAL_ALLOC});
    for (uint32_t ci = 0; ci < scene.mTables.clips.size(); ++ci)
    {
        int32_t const sk = scene.mTables.clips[ci].skeleton;
        if (sk >= 0 && static_cast<size_t>(sk) < skeletons.size())
            sAnimation.skeletonClips[static_cast<size_t>(sk)].push_back(ci);
    }

    for (FAnimationClip const& clip : scene.mTables.clips)
        sAnimation.duration = std::max(sAnimation.duration, clip.duration);
    for (FMorphTrack const& track : scene.mTables.morphTracks)
        sAnimation.duration = std::max(sAnimation.duration, track.duration);
    // Skinned-only imports (no clip/morph tracks) should start paused: there's deformable data,
    // but no time-varying source to advance.
    sAnimation.playing = !scene.mTables.clips.empty() || !scene.mTables.morphTracks.empty();

    LOG(Editor, LogInfo, "Animation runtime: {} deforming mesh(es), {} skeleton(s), {} clip(s), {} morph track(s)",
        sAnimation.meshes.size(), skeletons.size(), scene.mTables.clips.size(), scene.mTables.morphTracks.size());
}
} // namespace

// --- Animation update, expressed as a per-frame JobGraph -----------------------------------------
// BeginAnimationUpdate builds a small dependency graph (pose -> rigid/lights, pose -> begin-dynamic
// -> deform -> end-dynamic, all joined by a "done" barrier) and submits it, so the main thread can
// do its own per-frame bookkeeping (camera/UBO globals) while the pose pass runs on the pool.
// EndAnimationUpdate waits on the done barrier - pumping the graph's main-thread nodes (the dynamic
// ring window open/close, the lights apply) on the calling thread - before the caller commits the
// scene. The dynamic-geometry window and scene commit still happen on this same frame, so the
// GPUScene slot/commit/TLAS ordering is intact.

// This frame's animation graph, alive between Begin/EndAnimationUpdate (null when inactive).
static UniquePtr<JobGraph> sAnimGraph;
// The graph's terminal join node; EndAnimationUpdate waits on it.
static JobHandle sAnimDone;
// True when this frame actually evaluates animation (drives whether EndAnimationUpdate does work).
static bool sAnimFrameActive = false;
// Whether this frame's animation changes anything the scene commit must pick up (rigid transforms
// and/or dynamic geometry). Resolved synchronously at build time.
static bool sAnimChanged = false;

// Writes one animated node's world transform onto a target, if that node is animated. Reads the
// (already-evaluated) scene-node pose read-only; each call writes a disjoint target, so this is safe
// to invoke concurrently across instances.
static void ApplyAnimatedNode(int32_t node, FTransform& dst)
{
    auto const& affected = sAnimation.nodeAffected;
    if (node < 0 || static_cast<size_t>(node) >= affected.size() || !affected[node])
        return;
    FPose const& nodePose = sAnimation.poses[static_cast<size_t>(sAnimation.sceneNodeSkeleton)];
    decompose(nodePose.globals[node], dst.scale, dst.rotation, dst.transform);
}

// CPU deformation for one dynamic mesh: morph (POSITION deltas) then skin, written into the mesh's
// dynamic ring slot. Uses the calling worker's scratch lane; each mesh's ring region + output is
// disjoint, so this is race-free across workers.
static void DeformMesh(FImportedScene* scene, GPUScene* gpu, DynamicMeshRuntime const& rt, size_t worker)
{
    ZoneScopedN("Skin Mesh");
    if (gpu->Query(rt.handle) != GPUScene::Result::Ready)
        return;
    auto const& skeletons = scene->mTables.skeletons;
    size_t n = rt.bind.size();
    const FVertex* base = rt.bind.data();

    // Morph: base' = base + Σ weightₜ · deltaₜ (POSITION only), then rebuild normals from the
    // morphed surface (skinning, applied next, rotates those fresh normals correctly).
    if (rt.morphTrack >= 0 && rt.morphTargetCount > 0 &&
        static_cast<size_t>(rt.morphTrack) < scene->mTables.morphTracks.size())
    {
        FMorphTrack const& track = scene->mTables.morphTracks[rt.morphTrack];
        Vector<float>& weights = sAnimation.morphWeights[worker];
        Vector<FVertex>& morphed = sAnimation.morphs[worker];
        float t = track.duration > 0.0f ? std::fmod(sAnimation.time, track.duration) : 0.0f;
        SampleTrack(Span<const float>(track.times.data(), track.times.size()),
                    Span<const float>(track.values.data(), track.values.size()), rt.morphTargetCount,
                    track.interp, t, Span<float>(weights.data(), rt.morphTargetCount));
        for (size_t v = 0; v < n; ++v)
        {
            FVertex mv = rt.bind[v];
            for (uint32_t tt = 0; tt < rt.morphTargetCount; ++tt)
                mv.position += weights[tt] * rt.morphDeltas[static_cast<size_t>(tt) * n + v];
            morphed[v] = mv;
        }
        RecomputeNormals(Span<FVertex>(morphed.data(), n),
                         Span<const uint32_t>(rt.indices.data(), rt.indices.size()));
        base = morphed.data();
    }

    Span<std::byte> dst = gpu->UpdateDynamicGeometry(rt.handle);
    Span<FQVertex> out(reinterpret_cast<FQVertex*>(dst.data()), dst.size() / sizeof(FQVertex));
    if (rt.skeleton >= 0)
    {
        FSkeleton const& skel = skeletons[rt.skeleton];
        Span<mat4> palette(sAnimation.skins[worker].data(), skel.Count());
        ComputeSkinningMatrices(skel, sAnimation.poses[rt.skeleton], palette);
        SkinVertices(Span<const FVertex>(base, n),
                     Span<const FSkinBinding>(rt.binding.data(), rt.binding.size()),
                     Span<const mat4>(palette.data(), palette.size()), out);
    }
    else
    {
        for (size_t v = 0; v < n && v < out.size(); ++v)
            out[v] = FQVertex::Pack(base[v]);
    }
}

// Builds and submits this frame's animation JobGraph (non-blocking) and advances the clock. The
// pose pass runs on the pool while the caller does independent CPU work; EndAnimationUpdate then
// waits on the graph. No-op (leaves the frame inactive) while paused/held or still streaming, so a
// held pose lets the path tracer keep accumulating.
void BeginAnimationUpdate(float dt)
{
    sAnimFrameActive = false;
    sAnimChanged = false;
    sAnimDone = JobHandle{};
    sAnimGraph.reset(); // drop the previous frame's graph (drains it if a caller skipped End)
    if (sPendingSceneLoad || !sAnimation.HasData())
        return;
    GPUScene* gpu = GContext->gpuScene;
    if (!gpu)
        return;
    bool const doDynamic = !sAnimation.meshes.empty() && gpu->HasDynamicGeometry();
    bool const doRigid = sAnimation.HasRigid();
    if (!doDynamic && !doRigid)
        return;
    // Paused and not scrubbed: skip so a held pose lets the path tracer keep converging.
    if (!sAnimation.playing && !sAnimation.dirty)
        return;
    ZoneScoped;

    if (sAnimation.playing)
        sAnimation.time += dt;
    sAnimation.dirty = false;
    sAnimFrameActive = true;

    FImportedScene* scene = &GEditor.Scene();
    auto const& skeletons = scene->mTables.skeletons;
    ExecutionPolicy const policy = sAnimateParallel ? ExecutionPolicy::Par : ExecutionPolicy::Seq;

    Allocator* allocator = GContext->editorFrameScratch.get();
    sAnimGraph = ConstructUnique<JobGraph>(allocator, *GContext->jobs, allocator);
    JobGraph& graph = *sAnimGraph;
    JobHandle const done = graph.AddBarrier("Animation Done");
    sAnimDone = done;

    // Pose: evaluate every skeleton's pose into world matrices. Each skeleton writes its own FPose
    // and samples only its pre-bucketed clips, so the skeletons are independent and run in parallel.
    // With no skeletons it degenerates to a no-op source the rest of the graph can hang off of.
    JobHandle const pose = skeletons.empty()
        ? graph.AddBarrier("Anim Pose")
        : graph.AddParallelFor("Anim Pose", policy, skeletons.size(),
            [scene](size_t s)
            {
                ZoneScopedN("Anim Pose");
                auto const& skels = scene->mTables.skeletons;
                ResetToRest(skels[s], sAnimation.poses[s]);
                for (uint32_t clipIdx : sAnimation.skeletonClips[s])
                {
                    FAnimationClip const& clip = scene->mTables.clips[clipIdx];
                    float t = clip.duration > 0.0f ? std::fmod(sAnimation.time, clip.duration) : 0.0f;
                    SampleClip(clip, t, sAnimation.poses[s]);
                }
                ComputeGlobals(skels[s], sAnimation.poses[s]);
            });

    // Rigid node animation: write animated nodes' world transforms onto their instances/lights. The
    // instance pass (potentially large) fans out across the pool; lights are few and run as a
    // main-thread node, concurrently (disjoint writes) with the instance jobs.
    if (doRigid)
    {
        auto& instances = scene->mTables.instances;
        JobHandle const rigid = graph.AddParallelFor("Anim Rigid", policy, instances.begin(), instances.end(),
            [](FInstance& instance) { ApplyAnimatedNode(instance.node, instance.transform); });
        JobHandle const lights = graph.AddMain("Anim Lights",
            [scene]
            {
                for (FLight& light : scene->mTables.lights)
                    ApplyAnimatedNode(light.node, light.transform);
            });
        // Cameras are few and updated like lights; the editor view consumes the result (see
        // ApplyAnimatedCameraToView). No GPUScene commit is needed, so this doesn't touch sAnimChanged.
        JobHandle const cameras = graph.AddMain("Anim Cameras",
            [scene]
            {
                for (FCamera& camera : scene->mTables.cameras)
                    ApplyAnimatedNode(camera.node, camera.transform);
            });
        graph.DependsOn(rigid, pose);
        graph.DependsOn(lights, pose);
        graph.DependsOn(cameras, pose);
        graph.DependsOn(done, rigid, lights, cameras);
        sAnimChanged |= sAnimation.rigidDrivesTransforms;
    }

    // CPU deformation into the dynamic ring. The window opens on a main-thread node (advances the
    // slot), the per-mesh skinning fans out across the pool, then the window closes on a main-thread
    // node once the skinning completes - strictly ordered begin -> deform -> end by dependencies.
    if (doDynamic)
    {
        JobHandle const beginDynamic =
            graph.AddMain("Begin Dynamic Geometry", [gpu] { gpu->BeginDynamicGeometryUpdate(); });
        JobHandle const deform =
            graph.AddParallelFor("Anim Deform", policy, sAnimation.meshes.begin(), sAnimation.meshes.end(),
                [scene, gpu](DynamicMeshRuntime const& rt, size_t worker) { DeformMesh(scene, gpu, rt, worker); });
        JobHandle const endDynamic =
            graph.AddMain("End Dynamic Geometry", [gpu] { gpu->EndDynamicGeometryUpdate(); });
        graph.DependsOn(beginDynamic, pose);
        graph.DependsOn(deform, beginDynamic);
        graph.DependsOn(endDynamic, deform);
        graph.DependsOn(done, endDynamic);
        sAnimChanged = true;
    }

    graph.Submit();
}

// Waits on this frame's animation graph (running its main-thread nodes - the dynamic ring window
// open/close and the lights apply - on the calling thread). Returns true if any dynamic geometry /
// instance transform changed (the caller then re-commits the scene so dynamic instances encode the
// current ring slot).
bool EndAnimationUpdate()
{
    if (!sAnimFrameActive)
        return false;
    ZoneScoped;
    CHECK(sAnimGraph);
    sAnimGraph->Wait(sAnimDone);
    sAnimGraph.reset();
    return sAnimChanged;
}

// True when the editor view should be driven by an animated scene camera this frame: the first scene
// camera is animated and the clip is advancing (playing) or being scrubbed. Query this before
// BeginAnimationUpdate consumes the scrub `dirty` flag.
bool AnimatedCameraDrivesView()
{
    return sAnimation.cameraNode >= 0 && (sAnimation.playing || sAnimation.dirty);
}

// Drives the editor arcball from the animated scene camera's current transform (maintained each
// active frame by the "Anim Cameras" pass, mirroring lights). This consumes the transform produced by
// the previous frame's EndAnimationUpdate - one frame of latency that keeps the pose pass overlapped
// with the camera/UBO setup and is imperceptible on a smoothly playing camera. Mirrors the
// transform->arcball mapping in ApplySceneCamera. Returns true if it moved the view (so the caller
// resets path-tracer accumulation).
bool ApplyAnimatedCameraToView()
{
    if (sAnimation.cameraNode < 0 || !GEditor.HasScene())
        return false;
    auto cameras = GEditor.Scene().GetCameras();
    if (cameras.empty())
        return false;
    FTransform const& xf = cameras.front().transform;
    vec3 dir = xf.rotation * vec3(0, 0, 1);
    GEditor.camera.center = xf.transform - dir * GEditor.camera.radius;
    GEditor.camera.rot = xf.rotation;
    return true;
}

// Minimal playback panel. Only shown when the installed scene has animation. Scrubbing while paused
// requests a one-shot pose apply (UpdateAnimation honors `dirty`), so a held frame still updates.
void FAnimationPanel()
{
    if (!sAnimation.HasData())
        return;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 0.70f));
    if (ImGui::Begin("Animation"))
    {
        if (ImModalButton(sAnimation.playing ? PSI_PAUSE " Pause" : PSI_PLAY " Play", 0, 2))
            sAnimation.playing = !sAnimation.playing;
        ImGui::SameLine();
        if (ImModalButton(PSI_REPEAT " Restart", 1, 2))
        {
            sAnimation.time = 0.0f;
            sAnimation.dirty = true;
        }
        if (sAnimation.duration > 0.0f)
        {
            float t = std::fmod(sAnimation.time, sAnimation.duration);
            if (ImGui::SliderFloat("Time", &t, 0.0f, sAnimation.duration, "%.2f s"))
            {
                sAnimation.time = t;
                sAnimation.dirty = true; // apply this pose now, even while paused
            }
        }
        ImGui::TextDisabled("%zu deforming mesh(es)%s", sAnimation.meshes.size(),
                            sAnimation.HasRigid() ? ", rigid nodes" : "");
        // Debug: run CPU skinning / rigid apply serially (ExecutionPolicy::Seq) to isolate threading.
        ImGui::Checkbox("Parallel deformation", &sAnimateParallel);
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

// Pumped once per editor frame. The scene is already installed and rendering; this advances
// the background drain, re-committing each frame so geometry/textures pop into the live
// scene as they become resident. Returns true while the scene is still streaming.
bool PumpSceneLoad()
{
    if (!sPendingSceneLoad)
        return false;
    CHECK(GContext->gpuScene);
    GPUScene::Result r = GContext->gpuScene->Poll();
    if (r == GPUScene::Result::InProgress)
    {
        // Re-commit so newly resident textures replace their defaults; geometry streams into
        // the TLAS automatically (the per-frame TLAS pass only writes Ready instances).
        CommitSceneToGPU(GContext->gpuScene, GEditor.Scene(), GEditor.shaderGlobals, true);
        return true;
    }

    String envMapPath = sPendingSceneLoad->envMapPath;
    if (r == GPUScene::Result::Ready)
    {
        CommitSceneToGPU(GContext->gpuScene, GEditor.Scene(), GEditor.shaderGlobals, true);
        double const loadMs = MillisecondsSince(sPendingSceneLoad->loadStart);
        LOG(Editor, LogInfo, "Scene streamed in {:.2f} ms, {} meshes, {} instances, {} curves, {} materials",
            loadMs, sPendingSceneLoad->stats.sceneMeshCount, sPendingSceneLoad->stats.sceneInstanceCount,
            sPendingSceneLoad->stats.sceneCurveCount, sPendingSceneLoad->stats.sceneMaterialCount);
    }
    else
    {
        LOG(Editor, LogError, "Scene stream failed ({}); the partially loaded scene stays installed",
            static_cast<int>(r));
    }
    DestroyPendingSceneLoad(); // Poll() already joined the worker, so the backing memory is free to release
    if (r == GPUScene::Result::Ready && !envMapPath.empty())
        LoadEnvMap(envMapPath);
    return false;
}

void RequestLoadScene(StringView path, StringView envMapPath)
{
    sDeferredScenePath = path;
    sDeferredEnvMapPath = envMapPath;
}

void PumpDeferredSceneLoad()
{
    if (sDeferredScenePath.empty())
        return;
    String path = std::move(sDeferredScenePath);
    String envMapPath = std::move(sDeferredEnvMapPath);
    sDeferredScenePath.clear();
    sDeferredEnvMapPath.clear();
    LoadScene(path);
    if (!envMapPath.empty())
        DeferEnvMapForPendingLoad(envMapPath);
}

void LoadScene(StringView path)
{
    LOG(Editor, LogInfo, "Loading scene: {}", path);

    // A previous streaming load must complete before starting another (it owns GEditor maps
    // + the GPUScene about to be replaced).
    if (sPendingSceneLoad)
        FinishPendingSceneLoad();

    auto const loadStart = std::chrono::steady_clock::now();
    GPUScene* gpu = nullptr;
    PendingSceneLoad* load = nullptr;
    try
    {
        String scenePayloadPath = PrepareScenePayloadFile(path);
        load = Construct<PendingSceneLoad>(GLOBAL_ALLOC);
        load->scenePayloadPath = scenePayloadPath;
        load->loadStart = loadStart;
        load->arena.emplace(GLOBAL_ALLOC, kDefaultSceneLoadScratchBudget);
        load->alloc.emplace(*load->arena);
        load->file.emplace(scenePayloadPath, MemoryMappedAccess::ReadOnly);
        load->scene.emplace(*load->file, load->alloc->Ptr());
        LoadFSCN(*load->scene);

        InitializeSceneLoad(*load->scene, load->stats, gpu);
        // Globals/camera + env map + view LUTs first (small, synchronous); then queue the
        // heavy geometry/textures for the background drain.
        PrepareSceneGlobals(*load->scene, gpu, *load->alloc);
        BeginSceneUpload(*load->scene, gpu, load->meshGeometry, load->curveGeometry, load->textureIDMap);

        // Publish the handle maps and install the scene immediately so it renders while the
        // queued uploads stream in (PumpSceneLoad drives the drain + re-commits).
        GEditor.meshGeometry = std::move(load->meshGeometry);
        GEditor.curveGeometry = std::move(load->curveGeometry);
        GEditor.textureIDMap = std::move(load->textureIDMap);
        InstallLoadedScene(load->scenePayloadPath, gpu); // nulls `gpu`; ownership moves to GContext
        CommitSceneToGPU(GContext->gpuScene, GEditor.Scene(), GEditor.shaderGlobals, true);
        SetupAnimationRuntime(); // builds CPU skinning state from the installed scene
        sPendingSceneLoad = load;
    }
    catch (std::exception const& e)
    {
        sPendingSceneLoad = nullptr;
        DestroyGPUScene(gpu); // null after a successful install; otherwise joins + frees the new scene
        ClearAnimationRuntime();
        if (load) Destruct(GLOBAL_ALLOC, load);
        LOG(Editor, LogError, "Failed to load scene: {} ({})", path, e.what());
    }
    catch (...)
    {
        sPendingSceneLoad = nullptr;
        DestroyGPUScene(gpu);
        ClearAnimationRuntime();
        if (load) Destruct(GLOBAL_ALLOC, load);
        LOG(Editor, LogError, "Failed to load scene: {}", path);
    }
}

void LoadEnvMap(StringView path)
{
    LOG(Editor, LogInfo, "Loading HDRI env map: {}", path);
    auto* gpu = GContext->gpuScene;
    if (!gpu)
    {
        LOG(Editor, LogWarn, "Ignoring HDRI env map load before a scene is loaded");
        return;
    }
    try
    {
        FTexture tex(GLOBAL_ALLOC);
        LoadHDR(tex, path);
        gpu->UploadEnvMap(tex);
        FLight& environment = GEditor.Scene().EnsureEnvironmentLight();
        environment.environmentMap = true;
        gpu->BuildUBO(GEditor.shaderGlobals);
        UpdateSceneLights();
        GEditor.shaderGlobals.ptAccumulatedFrames = 0;
        LOG(Editor, LogInfo, "HDRI env map loaded successfully");
    }
    catch (...)
    {
        LOG(Editor, LogError, "Failed to load HDRI env map: {}", path);
    }
}

void HandleFile(const char* filePath)
{
    auto path = std::filesystem::path(filePath);
    auto ext = path.extension().string();
    // Normalize to lowercase
    for (auto& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".gltf" || ext == ".glb" || ext == ".fscn")
    {
        // HDRIs sharing the scene's filename load too. The scene load is async, so defer
        // the env map until the scene is installed (PumpSceneLoad applies it).
        String hdriPath = path.string().substr(0, path.string().length() - ext.length());
        String envPath;
        if (std::filesystem::exists(hdriPath + ".hdr"))
            envPath = hdriPath + ".hdr";
        else if (std::filesystem::exists(hdriPath + ".hdri"))
            envPath = hdriPath + ".hdri";
        RequestLoadScene(filePath, envPath);
    }
    else if (ext == ".hdr" || ext == ".hdri")
    {
        LoadEnvMap(filePath);
    }
    else
    {
        LOG(Editor, LogWarn, "Unknown file type dropped: '{}'", filePath);
    }
}
