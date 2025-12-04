#pragma once
#include "Context.hpp"
#include "Mesh.hpp"
#include "Serialization.hpp"
// Components
struct FTransform
{
    float3 transform;
    quat rotation;
    float3 scale;
};
struct FInstance
{
    FTransform transform;
    uint32_t meshIndex;
};
struct FCamera
{
    FTransform transform;
    float fovY;
};

void SceneLoadGLTF(StringView path, Vector<FMesh>& outMeshes, Vector<FInstance>& outInstances, Vector<FCamera>& outCameras);
void SceneLoadFromFile(StringView scenePath, Vector<FMesh>& outMeshes, Vector<FInstance>& outInstances, Vector<FCamera>& outCameras);
void SceneSaveBinFile(StringView path, Vector<FMesh> const& meshes, Vector<FInstance> const& instances,
                 Vector<FCamera> const& cameras);
