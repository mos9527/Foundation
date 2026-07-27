#include "Animation.hpp"
#include <algorithm>
#include <cmath>

namespace
{
uint32_t AppendRange(uint64_t& cursor, uint64_t size, uint32_t alignment)
{
    cursor = AlignUp(cursor, static_cast<uint64_t>(alignment));
    CHECK_MSG(cursor + size <= UINT32_MAX, "Skinning input exceeds uint32_t range");
    uint32_t offset = static_cast<uint32_t>(cursor);
    cursor += size;
    return offset;
}
}

FAnimationRuntimeBudget CalculateAnimationRuntimeBudget(FImportedScene const& scene)
{
    uint64_t inputCursor = 0;
    for (FSerializedMesh const& mesh : scene.GetMeshes())
    {
        if (mesh.skeleton.IsNil())
            continue;
        AppendRange(inputCursor, mesh.vertices.decodedSize, alignof(FQVertex));
        AppendRange(inputCursor, mesh.skinBinding.decodedSize, alignof(FSkinBinding));
        CHECK_MSG(!mesh.lods.empty(), "Skinned mesh has no LOD0 indices");
        AppendRange(inputCursor, mesh.lods[0].indices.decodedSize, alignof(uint32_t));
    }
    uint64_t paletteMatrices = 0;
    for (FSkeleton const& skeleton : scene.GetSkeletons())
        paletteMatrices += skeleton.Count();
    CHECK_MSG(paletteMatrices <= SIZE_MAX / (4u * sizeof(mat4)),
              "Animation palette budget exceeds addressable range");
    return {.inputBytes = static_cast<size_t>(inputCursor),
            .paletteBytes = static_cast<size_t>(paletteMatrices) * 4u * sizeof(mat4)};
}

FAnimationRuntime::FAnimationRuntime(Allocator* alloc)
    : mAllocator(alloc), mMeshes(alloc), mOutputs(alloc), mPlaybacks(alloc), mSkeletonClips(alloc), mPoses(alloc),
      mPaletteSkeletonOffsets(alloc), mDispatches(alloc)
{
    CHECK(mAllocator != nullptr);
}

void FAnimationRuntime::Initialize(FImportedScene& scene, GPUScene& gpu, RHIDevice* device,
                                   FSceneGPUResources& resources)
{
    Reset(&resources);
    mScene = &scene;
    mGPU = &gpu;
    mDevice = device;
    CHECK(mDevice != nullptr);

    auto meshes = scene.GetMeshes();
    auto skeletons = scene.GetSkeletons();
    auto clips = scene.GetClips();
    mMeshes.resize(meshes.size());
    mPaletteSkeletonOffsets.resize(skeletons.size());
    mPlaybacks.reserve(skeletons.size());
    mSkeletonClips.reserve(skeletons.size());
    mPoses.reserve(skeletons.size());

    uint64_t inputCursor = 0;
    for (uint32_t meshIndex = 0; meshIndex < meshes.size(); ++meshIndex)
    {
        FSerializedMesh const& mesh = meshes[meshIndex];
        if (mesh.skeleton.IsNil())
            continue;
        int skeletonIndex = scene.SkeletonIndex(mesh.skeleton);
        CHECK_MSG(skeletonIndex >= 0, "Skinned mesh references an unknown skeleton");
        CHECK_MSG(!mesh.lods.empty(), "Skinned mesh has no LOD0 indices");
        CHECK_MSG(mesh.skinBinding.count == mesh.vertexCount, "Skinned mesh binding count mismatch");
        MeshInput& input = mMeshes[meshIndex];
        input.bindVertexOffset = AppendRange(inputCursor, mesh.vertices.decodedSize, alignof(FQVertex));
        input.skinBindingOffset = AppendRange(inputCursor, mesh.skinBinding.decodedSize, alignof(FSkinBinding));
        input.indexOffset = AppendRange(inputCursor, mesh.lods[0].indices.decodedSize, alignof(uint32_t));
        input.vertexCount = mesh.vertexCount;
        input.indexCount = mesh.lods[0].indexCount;
        input.skeletonIndex = static_cast<uint32_t>(skeletonIndex);
        input.valid = true;
    }
    mInputSize = static_cast<uint32_t>(inputCursor);
    if (mInputSize == 0)
        return;

    mInputStagingBuffer = mDevice->CreateBuffer(
        {.resource = {.heap = RHIDeviceHeapType::Upload,
                      .hostAccess = RHIResourceHostAccess::WriteOnly,
                      .coherent = true,
                      .staging = true},
         .usage = RHIBufferUsageBits::TransferSource,
         .size = mInputSize});
    mInputBuffer = mDevice->CreateBuffer(
        {.resource = {.heap = RHIDeviceHeapType::Local},
         .usage = RHIBufferUsageBits::TransferDestination | RHIBufferUsageBits::TransferSource |
             RHIBufferUsageBits::StorageBuffer,
         .size = mInputSize});
    mInputStagingBuffer->DebugSetObjectName("Animation Input Staging");
    mInputBuffer->DebugSetObjectName("Animation Input");
    char* inputMapped = mInputStagingBuffer->Map<char>();
    for (uint32_t meshIndex = 0; meshIndex < meshes.size(); ++meshIndex)
    {
        MeshInput const& input = mMeshes[meshIndex];
        if (!input.valid)
            continue;
        FSerializedMesh const& mesh = meshes[meshIndex];
        CHECK(scene.ReadBlob(mesh.vertices, inputMapped + input.bindVertexOffset, mesh.vertices.decodedSize,
                             mAllocator));
        CHECK(scene.ReadBlob(mesh.skinBinding, inputMapped + input.skinBindingOffset, mesh.skinBinding.decodedSize,
                             mAllocator));
        CHECK(scene.ReadBlob(mesh.lods[0].indices, inputMapped + input.indexOffset,
                             mesh.lods[0].indices.decodedSize, mAllocator));
    }
    mInputUploadPending = true;

    uint32_t paletteOffset = 0;
    for (uint32_t skeletonIndex = 0; skeletonIndex < skeletons.size(); ++skeletonIndex)
    {
        mPaletteSkeletonOffsets[skeletonIndex] = paletteOffset;
        paletteOffset += skeletons[skeletonIndex].Count();
        mPlaybacks.push_back({.skeletonIndex = skeletonIndex});
        mSkeletonClips.emplace_back(mAllocator);
        mPoses.emplace_back(mAllocator);
    }
    mPaletteFrameMatrices = paletteOffset;
    CHECK_MSG(mPaletteFrameMatrices != 0, "Skinned scene has no joints");
    mPaletteBuffer = mDevice->CreateBuffer(
        {.resource = {.heap = RHIDeviceHeapType::Upload,
                      .hostAccess = RHIResourceHostAccess::WriteOnly,
                      .coherent = true},
         .usage = RHIBufferUsageBits::StorageBuffer,
         .size = static_cast<size_t>(mPaletteFrameMatrices) * kPaletteFrames * sizeof(mat4)});
    mPaletteBuffer->DebugSetObjectName("Animation Palettes");
    mPaletteMapped = mPaletteBuffer->Map<mat4>();

    for (uint32_t clipIndex = 0; clipIndex < clips.size(); ++clipIndex)
    {
        int skeletonIndex = scene.SkeletonIndex(clips[clipIndex].skeleton);
        if (skeletonIndex >= 0)
            mSkeletonClips[static_cast<uint32_t>(skeletonIndex)].push_back(clipIndex);
    }
    for (uint32_t skeletonIndex = 0; skeletonIndex < mPlaybacks.size(); ++skeletonIndex)
        if (!mSkeletonClips[skeletonIndex].empty())
            mPlaybacks[skeletonIndex].clipIndex = mSkeletonClips[skeletonIndex].front();

    auto instances = scene.GetInstances();
    resources.instanceGeometry.assign(instances.size(), GeometryHandle{});
    for (uint32_t instanceIndex = 0; instanceIndex < instances.size(); ++instanceIndex)
    {
        FInstance const& instance = instances[instanceIndex];
        if (instance.type != FInstanceType::Mesh)
            continue;
        int meshIndex = scene.MeshIndex(instance.resource);
        if (meshIndex < 0 || !mMeshes[static_cast<uint32_t>(meshIndex)].valid)
            continue;
        MeshInput const& input = mMeshes[static_cast<uint32_t>(meshIndex)];
        GeometryHandle output;
        GPUScene::Result result = gpu.Allocate(input.vertexCount, input.indexCount, output, true);
        CHECK_MSG(result == GPUScene::Result::Ready, "Failed to allocate skinned output ({})",
                  static_cast<uint32_t>(result));
        resources.instanceGeometry[instanceIndex] = output;
        mOutputs.push_back(
            {.instanceIndex = instanceIndex, .meshIndex = static_cast<uint32_t>(meshIndex), .geometry = output});
    }
    mForceUpdate = !mOutputs.empty();
}

void FAnimationRuntime::Reset(FSceneGPUResources* resources)
{
    if (resources)
        resources->instanceGeometry.clear();
    mDispatches.clear();
    mOutputs.clear();
    mMeshes.clear();
    mPlaybacks.clear();
    mSkeletonClips.clear();
    mPoses.clear();
    mPaletteSkeletonOffsets.clear();
    mInputBuffer.Reset();
    mInputStagingBuffer.Reset();
    mPaletteBuffer.Reset();
    mPaletteMapped = nullptr;
    mInputSize = 0;
    mPaletteFrameMatrices = 0;
    mInputUploadPending = false;
    mForceUpdate = false;
    mScene = nullptr;
    mGPU = nullptr;
    mDevice = nullptr;
}

void FAnimationRuntime::RemoveInstance(uint32_t instanceIndex, FSceneGPUResources& resources)
{
    for (size_t i = 0; i < mOutputs.size();)
    {
        if (mOutputs[i].instanceIndex == instanceIndex)
        {
            mOutputs.erase(mOutputs.begin() + i);
            continue;
        }
        if (mOutputs[i].instanceIndex > instanceIndex)
            --mOutputs[i].instanceIndex;
        ++i;
    }
    if (instanceIndex < resources.instanceGeometry.size())
        resources.instanceGeometry.erase(resources.instanceGeometry.begin() + instanceIndex);
}

Span<const uint32_t> FAnimationRuntime::GetSkeletonClips(uint32_t skeletonIndex) const
{
    if (skeletonIndex >= mSkeletonClips.size())
        return {};
    auto const& clips = mSkeletonClips[skeletonIndex];
    return {clips.data(), clips.size()};
}

bool FAnimationRuntime::Tick(float dt, uint64_t frame)
{
    if (!HasSkinning() || !mScene || !mGPU)
        return false;

    auto skeletons = mScene->GetSkeletons();
    auto clips = mScene->GetClips();
    Vector<uint8_t> evaluated(skeletons.size(), 0u, mAllocator);
    uint32_t frameBase = static_cast<uint32_t>(frame % kPaletteFrames) * mPaletteFrameMatrices;
    for (uint32_t skeletonIndex = 0; skeletonIndex < skeletons.size(); ++skeletonIndex)
    {
        FAnimationPlayback& playback = mPlaybacks[skeletonIndex];
        bool evaluate = mForceUpdate || playback.dirty || playback.playing;
        if (!evaluate)
            continue;
        FAnimationClip const* clip =
            playback.clipIndex < clips.size() && clips[playback.clipIndex].skeleton == skeletons[skeletonIndex].id
            ? &clips[playback.clipIndex]
            : nullptr;
        if (playback.playing && clip)
        {
            playback.time += dt * playback.speed;
            if (clip->duration > 0.0f)
            {
                if (playback.loop)
                {
                    playback.time = std::fmod(playback.time, clip->duration);
                    if (playback.time < 0.0f)
                        playback.time += clip->duration;
                }
                else
                {
                    float clamped = std::clamp(playback.time, 0.0f, clip->duration);
                    playback.playing = clamped != playback.time ? false : playback.playing;
                    playback.time = clamped;
                }
            }
        }
        FPose& pose = mPoses[skeletonIndex];
        ResetToRest(skeletons[skeletonIndex], pose);
        if (clip)
            SampleClip(*clip, playback.time, pose);
        ComputeGlobals(skeletons[skeletonIndex], pose);
        uint32_t paletteOffset = frameBase + mPaletteSkeletonOffsets[skeletonIndex];
        ComputeSkinningMatrices(skeletons[skeletonIndex], pose,
                                Span<mat4>{mPaletteMapped + paletteOffset, skeletons[skeletonIndex].Count()});
        playback.dirty = false;
        evaluated[skeletonIndex] = 1u;
    }

    mDispatches.clear();
    for (Output& output : mOutputs)
    {
        MeshInput const& input = mMeshes[output.meshIndex];
        if (!evaluated[input.skeletonIndex])
            continue;
        uint32_t outputOffset = 0;
        uint32_t outputType = 0;
        GSInstanceFlags outputFlags;
        mGPU->ResolveGeometry(output.geometry, outputOffset, outputType, outputFlags);
        CHECK(outputFlags & GSInstanceFlagsBits::Dynamic);
        uint32_t paletteOffset = frameBase + mPaletteSkeletonOffsets[input.skeletonIndex];
        mDispatches.push_back({
            .geometry = output.geometry,
            .bindVertexOffset = input.bindVertexOffset,
            .skinBindingOffset = input.skinBindingOffset,
            .indexSourceOffset = input.indexOffset,
            .vertexDestinationOffset = outputOffset + static_cast<uint32_t>(sizeof(GSMesh)),
            .indexDestinationOffset = outputOffset + static_cast<uint32_t>(sizeof(GSMesh)) +
                input.vertexCount * static_cast<uint32_t>(sizeof(FQVertex)),
            .paletteOffset = paletteOffset * static_cast<uint32_t>(sizeof(mat4)),
            .vertexCount = input.vertexCount,
            .indexCount = input.indexCount,
            .jointCount = skeletons[input.skeletonIndex].Count(),
            .updateIndices = output.updateIndices,
        });
        output.updateIndices = false;
    }
    if (!mDispatches.empty())
    {
        mGPU->BeginDynamicGeometryUpdate();
        for (Dispatch const& dispatch : mDispatches)
            mGPU->UpdateDynamicGeometryGPU(dispatch.geometry, true, dispatch.updateIndices);
        mGPU->EndDynamicGeometryUpdate();
    }
    mForceUpdate = false;
    return !mDispatches.empty();
}

void FAnimationRuntime::BuildGraph(Renderer* renderer, RendererResources& resources)
{
    if (!HasSkinning())
        return;
    CHECK(renderer != nullptr);
    ResourceHandle dynamicPrimitiveBuffer = resources.dynamicPrimitiveBuffer;
    CHECK(dynamicPrimitiveBuffer != kInvalidHandle);
    CHECK_MSG(renderer->GetFrameSwaps() <= kPaletteFrames,
              "Animation palette ring has {} slots but renderer requires {}", kPaletteFrames,
              renderer->GetFrameSwaps());
    ResourceHandle input = renderer->CreateResource("Animation Input", mInputBuffer.Get());
    ResourceHandle staging = renderer->CreateResource("Animation Input Staging", mInputStagingBuffer.Get());
    ResourceHandle palettes = renderer->CreateResource("Animation Palettes", mPaletteBuffer.Get());
    renderer->CreatePass(
        "Animation Input Upload", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindBufferCopySrc(self, staging);
            r->BindBufferCopyDst(self, input);
        },
        [=](PassHandle, Renderer* r, RHICommandList* cmd)
        {
            if (!mInputUploadPending)
                return;
            RHICommandList::CopyBufferRegion region{.size = mInputSize};
            cmd->CopyBuffer(r->DerefResource(staging).Get<RHIBuffer*>(),
                            r->DerefResource(input).Get<RHIBuffer*>(),
                            Span<const RHICommandList::CopyBufferRegion>{&region, 1});
            mInputUploadPending = false;
        });
    renderer->CreatePass(
        "Animation Index Seed", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindBufferCopySrc(self, input);
            r->BindBufferCopyDst(self, dynamicPrimitiveBuffer);
        },
        [=](PassHandle, Renderer* r, RHICommandList* cmd)
        {
            RHIBuffer* src = r->DerefResource(input).Get<RHIBuffer*>();
            RHIBuffer* dst = r->DerefResource(dynamicPrimitiveBuffer).Get<RHIBuffer*>();
            for (Dispatch const& dispatch : mDispatches)
            {
                if (!dispatch.updateIndices)
                    continue;
                RHICommandList::CopyBufferRegion region{
                    .srcOffset = dispatch.indexSourceOffset,
                    .dstOffset = dispatch.indexDestinationOffset,
                    .size = dispatch.indexCount * static_cast<uint32_t>(sizeof(uint32_t))};
                cmd->CopyBuffer(src, dst, Span<const RHICommandList::CopyBufferRegion>{&region, 1});
            }
        });
    renderer->CreatePass(
        "GPU Skinning", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Compute, "main",
                          r->GetApplication()->ResolveRelativePathBase("Data/Shaders/Editor/ECSSkinning.spv"));
            r->BindBufferStorageRead(self, input, RHIPipelineStageBits::ComputeShader, "skinningInput");
            r->BindBufferStorageRead(self, palettes, RHIPipelineStageBits::ComputeShader, "skinningPalette");
            r->BindBufferUnordered(self, dynamicPrimitiveBuffer, RHIPipelineStageBits::ComputeShader,
                                   "dynamicPrimitives");
            r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, 6u * sizeof(uint32_t));
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            struct Push
            {
                uint32_t bindVertexOffset;
                uint32_t skinBindingOffset;
                uint32_t vertexDestinationOffset;
                uint32_t paletteOffset;
                uint32_t vertexCount;
                uint32_t jointCount;
            };
            r->CmdSetPipeline(self, cmd);
            for (Dispatch const& dispatch : mDispatches)
            {
                Push push{.bindVertexOffset = dispatch.bindVertexOffset,
                          .skinBindingOffset = dispatch.skinBindingOffset,
                          .vertexDestinationOffset = dispatch.vertexDestinationOffset,
                          .paletteOffset = dispatch.paletteOffset,
                          .vertexCount = dispatch.vertexCount,
                          .jointCount = dispatch.jointCount};
                r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, push);
                r->CmdDispatch(self, cmd, {dispatch.vertexCount, 1, 1});
            }
            mDispatches.clear();
        });
}
