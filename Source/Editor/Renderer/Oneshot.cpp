#include "Oneshot.hpp"
using namespace RHI;
void OneshotPass<IBLPrefilterPass>::Process(TextureAsset* srcImage) {	
	// Reset the cubemap buffer	
	uint dimension = std::min((uint)srcImage->texture.texture.GetDesc().width, srcImage->texture.texture.GetDesc().height);
	// Ensure the image dimension is in the power of 2
	dimension = 1 << (uint)floor(std::log2(dimension));	
	uint numMips = Resource::ResourceDesc::numMipsOfDimension(dimension, dimension);
	cubeMap = std::make_unique<Texture>(device, Resource::ResourceDesc::GetTextureBufferDesc(
		ResourceFormat::R32G32B32A32_FLOAT /* UAVs can't take RGB32 so... */, ResourceDimension::Texture2D, dimension, dimension,
		numMips,
		6, /* a cubemap! */
		1, 0,
		ResourceFlags::UnorderedAccess, ResourceHeapType::Default,
		ResourceState::UnorderedAccess
	));	
	for (uint i = 0; i < numMips; i++)
		cubeMapUAV.push_back(std::make_unique<UnorderedAccessView>(
			cubeMap.get(), UnorderedAccessViewDesc::GetTexture2DArrayDesc(ResourceFormat::R32G32B32A32_FLOAT, i, 0, 6, 0)
		));	
	RenderGraph rg(cache);
	std::vector<RgHandle> rgCubemapUAV;
	for (uint i = 0; i < numMips; i++)
		rgCubemapUAV.push_back(rg.import<UnorderedAccessView>(cubeMapUAV[i].get()));	
	// Finalize handles	
	proc_Prefilter.insert(rg, {
		.panoSrv = rg.import<ShaderResourceView>(srcImage->textureSRV.get()),
		.cubemapOut = rg.import<Texture>(cubeMap.get()),
		.cubemapOutUAV = rgCubemapUAV[0],
		.cubemapOutUAVs = rgCubemapUAV,
		.radianceOut = rgCubemapUAV[0], /* stub */
		.irradianceOut = rgCubemapUAV[0] /* stub */
	});
	// Acquire a free Direct context
	auto* cmd = device->GetDefaultCommandList<CommandListType::Compute>();
	CHECK(!cmd->IsOpen() && "The Default Compute Command list is in use!");
	PIXCaptureParameters captureParams = {};
	captureParams.GpuCaptureParameters.FileName = (PWSTR)L"IBLPrefilter-Capture.wpix";

	PIXBeginCapture(PIX_CAPTURE_GPU, &captureParams);
	cmd->Begin();	
	static ID3D12DescriptorHeap* const heaps[] = { device->GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->GetNativeHeap() };
	cmd->GetNativeCommandList()->SetDescriptorHeaps(1, heaps);
	rg.execute(cmd);
	cmd->Close();
	LOG(INFO) << "Executing...";
	cmd->Execute().Wait();
	LOG(INFO) << "Done.";
	PIXEndCapture(FALSE);
}