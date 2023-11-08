#pragma once
#include "../RHI/RHI.hpp"
#include "SceneGraph.hpp"
#include "Structs.hpp"
using namespace RHI;
class Renderer {	
	entt::registry registry;

	GeometryManager geometry_manager{ registry };
	MaterialManager material_manager{ registry };
	SceneGraph scene{ registry, geometry_manager, material_manager };
	uint numGeometry, numInstances;

	std::vector<std::unique_ptr<DescriptorHeap>> boundDescHeaps;
	// storage descriptor heap for storing maybe-used CBV-SRV-UAV descriptors
	// which should be copied at runtime when necessary
	std::unique_ptr<DescriptorHeap> storageDescHeap;
	template<typename T> struct StructuredBuffer {		
		std::unique_ptr<Buffer> uploadBuffer;
		std::unique_ptr<Buffer> destBuffer;

		std::unique_ptr<Descriptor> dest_bound_desc;		
		void create(Device* device, DescriptorHeap* srvHeap, size_t num_elements , name_t name=L"StructuredBuffer") {
			uploadBuffer = std::make_unique<Buffer>(device, Buffer::BufferDesc::GetGenericBufferDesc(
				num_elements * sizeof(T), sizeof(T), ResourceState::CopySource, ResourceUsage::Upload
			));
			destBuffer = std::make_unique<Buffer>(device, Buffer::BufferDesc::GetGenericBufferDesc(
				num_elements * sizeof(T), sizeof(T), ResourceState::CopyDest, ResourceUsage::Default
			));
			dest_bound_desc = srvHeap->AllocateDescriptor();
			device->CreateStructedBufferShaderResourceView(destBuffer.get(), dest_bound_desc.get());
			uploadBuffer->SetName(name + L" (Upload)");
			destBuffer->SetName(name);			
		}
		void update_upload_buffer(T* data, size_t index) {
			uploadBuffer->Update((void*)data, sizeof(data), sizeof(data) * index);
		}
		void update_dest_buffer(CommandList* cmdList) {
			destBuffer->SetState(cmdList, ResourceState::CopyDest);
			destBuffer->QueueCopy(cmdList, uploadBuffer.get(), 0, 0, uploadBuffer->GetDesc().width);
			destBuffer->SetState(cmdList, ResourceState::GenericRead);
		}
	};
	StructuredBuffer<InstanceData> instanceBuffer;
	StructuredBuffer<GeometryData> geometryBuffer;
	template<typename T> struct ConstantBuffer {	
		std::unique_ptr<Buffer> buffer;
		std::unique_ptr<Descriptor> bound_desc;
		T data;
		void create(Device* device, DescriptorHeap* cbvHeap, name_t name = L"ConstantBuffer") {
			buffer = std::make_unique<Buffer>(device, Buffer::BufferDesc::GetGenericBufferDesc(
				GetAlignedSize(sizeof(T), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT), sizeof(T), ResourceState::GenericRead
			));			
			bound_desc = cbvHeap->AllocateDescriptor();
			device->CreateConstantBufferView(buffer.get(), bound_desc.get());
			buffer->SetName(name);
		}
		void update() {
			update(&data);
		}
		void update(T* data) {
			buffer->Update(data, sizeof(T), 0);
		}
	};
	struct Camera : public CameraData
	{
		Camera() {
			position = { 0,0,0,1 };
			direction = { 0,0,1,1 };
			FovAspectNearZFarZ = { XM_PIDIV4, 1.0f, 0.01f, 100.0f };
			update();
		}
		void update() {
			view = XMMatrixLookToLH(
				position, direction, { 0,1,0 }
			);
			projection = XMMatrixPerspectiveFovLH(
				FovAspectNearZFarZ.x, FovAspectNearZFarZ.y, FovAspectNearZFarZ.z, FovAspectNearZFarZ.w
			);
			viewProjection = view * projection;
			// store transposed values
			view = view.Transpose();
			projection = projection.Transpose();
			viewProjection = viewProjection.Transpose();
			// 6 clipping planes
			// https://github.com/microsoft/DirectX-Graphics-Samples/blob/master/Samples/Desktop/D3D12MeshShaders/src/DynamicLOD/D3D12DynamicLOD.cpp#L475
			XMMATRIX vp = viewProjection;
			clipPlanes[0] = XMPlaneNormalize(XMVectorAdd(vp.r[3], vp.r[0])),      // Left
				clipPlanes[1] = XMPlaneNormalize(XMVectorSubtract(vp.r[3], vp.r[0])), // Right
				clipPlanes[2] = XMPlaneNormalize(XMVectorAdd(vp.r[3], vp.r[1])),      // Bottom
				clipPlanes[3] = XMPlaneNormalize(XMVectorSubtract(vp.r[3], vp.r[1])), // Top
				clipPlanes[4] = XMPlaneNormalize(vp.r[2]),                            // Near
				clipPlanes[5] = XMPlaneNormalize(XMVectorSubtract(vp.r[3], vp.r[2])); // Far	
		}
	};
	ConstantBuffer<Camera> cameraCBV;
	std::unique_ptr<Device> device;
	std::unique_ptr<Swapchain> swapChain;	

	std::vector<std::unique_ptr<CommandList>> commandLists;
	std::vector<size_t> fenceValues;
	std::vector<std::unique_ptr<Fence>> fences;

	struct DepthStencil {
		std::unique_ptr<RHI::Texture> depthStencilBuffer;
		std::unique_ptr<RHI::Descriptor> depthStencilView;
		void create(Device* device, DescriptorHeap* dsvHeap, UINT width, UINT height) {
			resize(device, dsvHeap, width, height);
		}
		void resize(Device* device, DescriptorHeap* dsvHeap, UINT width, UINT height) {
			depthStencilBuffer = std::make_unique<RHI::Texture>(device, RHI::Texture::TextureDesc::GetTextureBufferDesc(
				ResourceFormat::R32_FLOAT,
				ResourceDimension::Texture2D,
				width,
				height,
				1, 1, 1, 0,
				ResourceFlags::DepthStencil,
				ResourceUsage::Default,
				ResourceState::DepthWrite,
				ResourcePoolType::Default,
				ClearValue{
					.clear = true,
					.depthStencil = DepthStencilValue{
						.depth = 0.0f,
						.stencil = 0u
					}
				}
			));
			depthStencilView = dsvHeap->AllocateDescriptor();
			device->CreateDepthStencilView(depthStencilBuffer.get(), depthStencilView.get());
		}
	};
	/* testing */
	DepthStencil testDepthStencil;
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