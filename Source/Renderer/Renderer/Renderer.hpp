#pragma once
#include "../RHI/RHI.hpp"
#include "SceneGraph.hpp"
using namespace RHI;
class Renderer {	
	entt::registry registry;

	GeometryManager geometry_manager{ registry };
	MaterialManager material_manager{ registry };
	SceneGraph scene{ registry, geometry_manager, material_manager };
	std::vector<std::unique_ptr<DescriptorHeap>> boundDescHeaps;
	// storage descriptor heap for storing maybe-used CBV-SRV-UAV descriptors
	// which should be copied at runtime when necessary
	std::unique_ptr<DescriptorHeap> storageDescHeap;
	template<typename T> struct ShaderVisibleBuffer {
		std::unique_ptr<Buffer> buffer;
		std::unique_ptr<Descriptor> storage_desc;
		std::unique_ptr<Descriptor> bound_desc;
		void create(Device* device, size_t num_elements) {
			buffer = std::make_unique<Buffer>(device, Buffer::BufferDesc::GetGenericBufferDesc(
				num_elements * sizeof(T), sizeof(T)
			));
		}
		void update(T data, size_t index) {
			buffer->Update((void*)data, sizeof(data), sizeof(data) * index);
		}
	};
	ShaderVisibleBuffer<InstanceData> instanceBuffer;

	std::unique_ptr<Device> device;
	std::unique_ptr<Swapchain> swapChain;	

	std::vector<std::unique_ptr<CommandList>> commandLists;
	std::vector<size_t> fenceValues;
	std::vector<std::unique_ptr<Fence>> fences;

	/* testing */
	std::unique_ptr<RHI::Texture> testDepthStencil;
	std::unique_ptr<PipelineState> testPso;
	std::unique_ptr<ShaderBlob> testAS, testMS, testPS;
	std::unique_ptr<RootSignature> testRootSig;

	void begin_frame();
	void end_frame();
	void wait(CommandQueue* queue, Fence* fence);
	void wait(CommandListType type);
	void resize_buffers(UINT width, UINT height);
	void update_scene_buffer();
	bool bLoading = false;
public:
	bool bVsync = false;
	Renderer(Device::DeviceDesc const&, Swapchain::SwapchainDesc const&);
	void load_resources();
	void resize_viewport(UINT width, UINT height);
	void render();
	auto getDevice() { return device.get(); }
};