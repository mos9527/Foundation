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

GPUSceneDesc CalculateSceneGPUDesc(FImportedScene const& scene, Foundation::RHI::RHIDeviceCapabilities const& caps,
                                   uint32_t minLightBudget = 1024u);
void UploadSceneResources(FImportedScene& scene, GPUScene& gpu, FSceneGPUResources& outResources);
void UploadSceneEnvironment(FImportedScene const& scene, FLight const& environment, GPUScene& gpu);

// NOTE: For scene it's Instances, Materials and Lights only.
GPUScene::UpdateResult CommitSceneToGPU(FImportedScene& scene, GPUScene& gpu, FSceneGPUResources const& resources,
                                        RendererUBO& globals, GPUSceneUpdateFlags flag = kDefaultGPUSceneUpdateFlags);
