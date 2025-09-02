#include "GPUCulling.hpp"
#include "Renderer.hpp"
using namespace Foundation;
GPUCulling::GPUCulling(Renderer& renderer, ResourceHandle sceneGlobal, ResourceHandle sceneInstance, ResourceHandle scenePrimitive):
    sceneGlobal(sceneGlobal), sceneInstance(sceneInstance), scenePrimitive(scenePrimitive)
{
    m_cmds = renderer.CreateResource("GPUCulling.Commands",
        RHIBufferDesc{
            .usage = RHIBufferUsageBits::IndirectBuffer | RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::TransferDestination,
            .size = kMaxCommandBytes
        });
    m_ctr = renderer.CreateResource("GPUCulling.CommandCounter",
        RHIBufferDesc{
            .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::TransferDestination,
            .size = sizeof(uint64_t)
        });
}
void GPUCulling::Setup(PassHandle self, Renderer& renderer) {

}
void GPUCulling::Record(PassHandle self, Renderer&, RHICommandList* cmd) {

}
