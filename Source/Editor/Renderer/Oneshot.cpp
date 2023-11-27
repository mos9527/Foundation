#include "Oneshot.hpp"
using namespace RHI;
#define IRRIDANCE_MAP_DIMENSION 64	
#define NUM_RADIANCE_CUBEMAPS 2 // GGX & Charile
#define NUM_LUTS 2 // GGX & Charlie
OneshotPass<IBLPrefilterPass>::OneshotPass(Device* device, uint dimension_) :
	device(device), proc_Prefilter(device) {		
	// Ensure the image dimension is in the power of 2
	dimension = 1 << (uint)floor(std::log2(dimension_));	
	numMips = Resource::ResourceDesc::numMipsOfDimension(dimension, dimension);
	cubeMap = std::make_unique<Texture>(device, Resource::ResourceDesc::GetTextureBufferDesc(
		ResourceFormat::R16G16B16A16_FLOAT /* UAVs can't take RGB32 so... */, ResourceDimension::Texture2D, dimension, dimension,
		numMips,
		6, /* a cubemap! */
		1, 0,
		ResourceFlags::UnorderedAccess, ResourceHeapType::Default,
		ResourceState::UnorderedAccess
	));	
	for (uint i = 0; i < numMips; i++) {
		cubeMapUAVs.push_back(std::make_unique<UnorderedAccessView>(
			cubeMap.get(), 
			UnorderedAccessViewDesc::GetTexture2DArrayDesc(ResourceFormat::R16G16B16A16_FLOAT, i, 0, 6, 0)
		));
	}
	cubemapSRV = std::make_unique<ShaderResourceView>(
		cubeMap.get(), ShaderResourceViewDesc::GetTextureCubeDesc(ResourceFormat::R16G16B16A16_FLOAT, 0, numMips)
	);
	// IRRADIANCE
	irridanceMap = std::make_unique<Texture>(device, Resource::ResourceDesc::GetTextureBufferDesc(
		ResourceFormat::R16G16B16A16_FLOAT /* UAVs can't take RGB32 so... */, ResourceDimension::Texture2D, IRRIDANCE_MAP_DIMENSION, IRRIDANCE_MAP_DIMENSION,
		1,
		6,
		1, 0,
		ResourceFlags::UnorderedAccess, ResourceHeapType::Default,
		ResourceState::UnorderedAccess
	));
	irridanceMapUAV = std::make_unique<UnorderedAccessView>(
		irridanceMap.get(),
		UnorderedAccessViewDesc::GetTexture2DArrayDesc(ResourceFormat::R16G16B16A16_FLOAT, 0, 0, 6, 0)
	);
	// RADIANCE
	radianceMapArray = std::make_unique<Texture>(device, Resource::ResourceDesc::GetTextureBufferDesc(
		ResourceFormat::R16G16B16A16_FLOAT, ResourceDimension::Texture2D, dimension, dimension,
		numMips,
		6 * NUM_RADIANCE_CUBEMAPS,
		1, 0,
		ResourceFlags::UnorderedAccess, ResourceHeapType::Default,
		ResourceState::UnorderedAccess
	));
	for (uint j = 0; j < NUM_RADIANCE_CUBEMAPS; j++) {
		for (uint i = 0; i < numMips; i++) {
			// [mip 0-N map 1] [mip 0-M map 2] ...
			radianceCubeArrayUAVs.push_back(std::make_unique<UnorderedAccessView>(
				radianceMapArray.get(),
				UnorderedAccessViewDesc::GetTexture2DArrayDesc(ResourceFormat::R16G16B16A16_FLOAT, i, 0, 6, 0)
			));
		}
	}
	radianceCubeArraySRV = std::make_unique<ShaderResourceView>(
		radianceMapArray.get(),
		ShaderResourceViewDesc::GetTextureCubeArrayDesc(
			ResourceFormat::R16G16B16A16_FLOAT, 0, numMips, 0, NUM_RADIANCE_CUBEMAPS
		)
	);
	// LUT
	lutArray = std::make_unique<Texture>(device, Resource::ResourceDesc::GetTextureBufferDesc(
		ResourceFormat::R16G16_FLOAT, ResourceDimension::Texture2D, dimension, dimension,
		1,
		NUM_LUTS,
		1, 0,
		ResourceFlags::UnorderedAccess, ResourceHeapType::Default,
		ResourceState::UnorderedAccess
	));	
	lutArrayUAV = std::make_unique<UnorderedAccessView>(
		lutArray.get(),
		UnorderedAccessViewDesc::GetTexture2DArrayDesc(ResourceFormat::R16G16_FLOAT, 0, 0, NUM_LUTS,0)
	);
	lutArraySRV = std::make_unique<ShaderResourceView>(
		lutArray.get(),
		ShaderResourceViewDesc::GetTexture2DDesc(
			ResourceFormat::R16G16_FLOAT, 0, 1
		)
	);
}
void OneshotPass<IBLPrefilterPass>::Process(TextureAsset* srcImage) {	
	auto fill_handles = [&](RenderGraph& rg) {
		std::vector<RgHandle> rg_CubemapUAVs, rg_RadianceCubeArrayUAVs;
		for (auto& uav : cubeMapUAVs) rg_CubemapUAVs.push_back(rg.import<UnorderedAccessView>(uav.get()));
		for (auto& uav : radianceCubeArrayUAVs) rg_RadianceCubeArrayUAVs.push_back(rg.import<UnorderedAccessView>(uav.get()));
		return IBLPrefilterPass::IBLPrefilterPassHandles{
			.panoSrv = rg.import<ShaderResourceView>(srcImage->textureSRV.get()),

			.cubemap = rg.import<Texture>(cubeMap.get()),
			.cubemapUAVs = rg_CubemapUAVs,
			.cubemapSRV = rg.import<ShaderResourceView>(cubemapSRV.get()),

			.radianceCubeArray = rg.import<Texture>(radianceMapArray.get()),
			.radianceCubeArrayUAVs = rg_RadianceCubeArrayUAVs,
			.radianceCubeArraySRV = rg.import<ShaderResourceView>(radianceCubeArraySRV.get()),

			.irradianceCube = rg.import<Texture>(irridanceMap.get()),
			.irradianceCubeUAV = rg.import<UnorderedAccessView>(irridanceMapUAV.get()),
			.irradianceCubeSRV = rg.import<ShaderResourceView>(irridanceCubeSRV.get()),

			.lutArray = rg.import<Texture>(lutArray.get()),
			.lutArrayUAV = rg.import<UnorderedAccessView>(lutArrayUAV.get()),
			.lutArraySRV = rg.import<ShaderResourceView>(lutArraySRV.get())
		};		
	};
	auto execute_graph = [&](RenderGraph& rg) {
		// Acquire a free Compute context
		auto* cmd = device->GetDefaultCommandList<CommandListType::Compute>();
		CHECK(!cmd->IsOpen() && "The Default Compute Command list is in use!");	
		cmd->Begin();	
		static ID3D12DescriptorHeap* const heaps[] = { device->GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->GetNativeHeap() };
		cmd->GetNativeCommandList()->SetDescriptorHeaps(1, heaps);
		rg.execute(cmd);
		cmd->Close();		
		cmd->Execute().Wait();		
	};
	auto subproc_pano2cube = [&]() {
		RenderGraph rg(cache);
		auto handles = fill_handles(rg);
		proc_Prefilter.insert_pano2cube(rg, handles);
		execute_graph(rg);
	};
	auto subproc_diffuse_prefilter = [&]() {
		RenderGraph rg(cache);
		auto handles = fill_handles(rg);
		proc_Prefilter.insert_diffuse_prefilter(rg, handles);
		execute_graph(rg);
	};
	auto subproc_specular_prefilter = [&](uint mipIndex, uint cubeIndex) {
		RenderGraph rg(cache);
		auto handles = fill_handles(rg);
		proc_Prefilter.insert_specular_prefilter(rg, handles, mipIndex, numMips,cubeIndex);
		execute_graph(rg);
	};
	auto subproc_lut = [&]() {
		RenderGraph rg(cache);
		auto handles = fill_handles(rg);
		proc_Prefilter.insert_lut(rg, handles);
		execute_graph(rg);
	};
	subproc_pano2cube();
	subproc_diffuse_prefilter();
	return; // testing...
	for (uint j = 0; j < NUM_RADIANCE_CUBEMAPS; j++) {
		for (uint i = 0; i < numMips; i++) {
			subproc_specular_prefilter(i, j);
		}
	}
	subproc_lut();
};