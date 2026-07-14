#pragma once
#include <Editor/Scene/Scene.hpp>
#include <Renderer/GPUScene.hpp>

namespace Foundation::RHI { struct RHIDeviceCapabilities; }

// Resident GPU handles and id->table-index maps; rebuilt per UploadSceneResources.
struct FSceneGPUResources
{
    Vector<GeometryHandle> meshGeometry{GLOBAL_ALLOC};
    Vector<GeometryHandle> curveGeometry{GLOBAL_ALLOC};
    Vector<TextureHandle> textureIDMap{GLOBAL_ALLOC};
    HashMap<FUUID, uint32_t> meshById{GLOBAL_ALLOC};
    HashMap<FUUID, uint32_t> curveById{GLOBAL_ALLOC};
    HashMap<FUUID, uint32_t> textureById{GLOBAL_ALLOC};
    HashMap<FUUID, uint32_t> materialById{GLOBAL_ALLOC};
};

bool IsSceneEnvironmentTexture(FImportedScene const& scene, size_t textureIndex);

GPUSceneDesc CalculateSceneGPUDesc(FImportedScene const& scene, Foundation::RHI::RHIDeviceCapabilities const& caps,
                                   uint32_t minLightBudget = 1024u);

// Queues every mesh/curve/texture in `scene` (other than its environment map, see
// UploadSceneEnvironment) onto `gpu`'s upload queue and fills `outResources` by resource index.
// Drain with gpu.Join() or gpu.Poll().
void UploadSceneResources(FImportedScene& scene, GPUScene& gpu, FSceneGPUResources& outResources);

// Synchronously uploads `environment`'s HDRI map and importance-sampling CDFs. Requires
// environment.HasEnvironmentTexture().
void UploadSceneEnvironment(FImportedScene const& scene, FLight const& environment, GPUScene& gpu);

// Fills and commits the GPUScene instance/material/light tables from `scene`, resolving instance
// geometry and material textures through `resources`, then rebuilds the UBO.
// `frameNumber` stamps motion-vector contributors (defaults to `globals.frameNumber`).
GPUScene::UpdateResult CommitSceneToGPU(FImportedScene& scene, GPUScene& gpu, FSceneGPUResources const& resources,
                                        RendererUBO& globals, bool resetAccumulation = true,
                                        uint32_t frameNumber = UINT32_MAX);
