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

void LoadGLTF(StringView path, Vector<FMesh>& outMeshes, Vector<FInstance>& outInstances, Vector<FCamera>& outCameras);

void SceneSerialize(FWriter& w, Vector<FMesh> const& meshes, Vector<FInstance> const& instances, Vector<FCamera> const& cameras);
void SceneDeserialize(FReader& r, Vector<FMesh>& meshes, Vector<FInstance>& instances, Vector<FCamera>& cameras);
