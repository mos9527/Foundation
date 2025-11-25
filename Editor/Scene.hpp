#pragma once
#include "Context.hpp"
#include "Mesh.hpp"
// Components
struct FInstance
{
    float3 transform;
    quat rotation;
    float3 scale;

    uint32_t meshIndex;
};
void LoadGLTF(StringView path, Vector<FMesh>& outMeshes, Vector<FInstance>& outInstances);
