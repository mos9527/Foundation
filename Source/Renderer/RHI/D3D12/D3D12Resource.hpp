#pragma once
namespace RHI {
	enum class ResourceBindType {
		ShaderResource,
		UnorderedAccess,
		RenderTarget,
		DepthStencil,
		Unbound
	};
	// https://asawicki.info/news_1755_untangling_direct3d_12_memory_heap_types_and_pools
	enum class ResourceUsage {
		Default,
		Upload,
		Readback
	};
}