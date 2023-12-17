#include "LTCFittedLUT.hpp"
#include "Data/LTC.hpp"

using namespace RHI;
LTCFittedLUT::LTCFittedLUT(Device* device) : device(device) {
	ltcLUT = std::make_unique<Texture>(
		device, Resource::ResourceDesc::GetTextureBufferDesc(
			ResourceFormat::R32G32B32A32_FLOAT, ResourceDimension::Texture2D,
			64, 64, 1 ,2
		)
	);
	ltcLUT->SetName(L"LTC Fit LUT");
	ltcSRV = std::make_unique<ShaderResourceView>(ltcLUT.get(), ShaderResourceViewDesc::GetTexture2DArrayDesc(
			ResourceFormat::R32G32B32A32_FLOAT, 0, 1, 0, 2, 0
		)
	);
	UploadPrecomputed();
}
// xxx replace sampling with analytic approximation
void LTCFittedLUT::UploadPrecomputed() {
	UploadContext ctx(device);
	ctx.Begin();
	auto intermediate = ctx.CreateIntermediateTexture2DContainer(ltcLUT.get());
	intermediate->WriteSubresource((void*)&g_ltc_1, 64 * sizeof(float) * 4, 64, 0, 0);
	intermediate->WriteSubresource((void*)&g_ltc_2, 64 * sizeof(float) * 4, 64, 1, 0);
	ctx.QueueUploadTexture(ltcLUT.get(), intermediate, 0, 2);
	ctx.End().Wait();
}

// void LTCFittedLUT::Compute();