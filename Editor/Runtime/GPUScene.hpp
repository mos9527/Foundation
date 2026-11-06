#pragma once
#include <Editor/Scene/Scene.hpp>
#include <Renderer/GPUScene.hpp>

namespace Foundation::RHI { struct RHIDeviceCapabilities; }

struct FSceneGPUResources
{
    Vector<GeometryHandle> meshGeometry{GLOBAL_ALLOC};
    Vector<GeometryHandle> curveGeometry{GLOBAL_ALLOC};
    Vector<GeometryHandle> instanceGeometry{GLOBAL_ALLOC};
    Vector<TextureHandle> textureIDMap{GLOBAL_ALLOC};
    HashMap<FUUID, uint32_t> meshById{GLOBAL_ALLOC};
    HashMap<FUUID, uint32_t> curveById{GLOBAL_ALLOC};
    HashMap<FUUID, uint32_t> textureById{GLOBAL_ALLOC};
    HashMap<FUUID, uint32_t> materialById{GLOBAL_ALLOC};
};

bool IsSceneEnvironmentTexture(FImportedScene const& scene, size_t textureIndex);

GPUSceneDesc CalculateSceneGPUDesc(FImportedScene const& scene, Foundation::RHI::RHIDeviceCapabilities const& caps);
void UploadSceneResources(FImportedScene& scene, GPUScene& gpu, FSceneGPUResources& outResources);
// Upload only newly appended resource-table entries; retain existing GPU handles and instance overrides.
void AppendSceneResources(FImportedScene& scene, GPUScene& gpu, FSceneGPUResources& resources);
void UploadSceneEnvironment(FImportedScene const& scene, FLight const& environment, GPUScene& gpu);

// NOTE: For scene it's Instances, Materials and Lights only.
GPUScene::UpdateResult CommitSceneToGPU(FImportedScene& scene, GPUScene& gpu, FSceneGPUResources const& resources,
                                        RendererUBO& globals, GPUSceneUpdateFlags flag = kDefaultGPUSceneUpdateFlags);
