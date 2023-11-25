#pragma once
#include "../../../Runtime/RHI/RHI.hpp"
#include "../../../Runtime/Asset/AssetRegistry.hpp"
#include "../../../Runtime/RenderGraph/RenderGraph.hpp"
#include "../../../Runtime/RenderGraph/RenderGraphResource.hpp"
#include "../../../Runtime/RenderGraph/RenderGraphResourceCache.hpp"
#include "../../Scene/SceneView.hpp"

inline std::unique_ptr<RHI::Shader> BuildShader(const wchar_t* sourceName, const wchar_t* entrypoint, const wchar_t* target, std::vector<const wchar_t*>&& defines = {}) {	
#ifdef _DEBUG
	path_t shaderAbsPath = std::format(L"Source/Editor/Renderer/Shaders/{}.hlsl", sourceName);
	shaderAbsPath = get_absolute_path(shaderAbsPath);
	path_t commonAbsPath = std::format(L"Source/Common/");
	commonAbsPath = get_absolute_path(commonAbsPath);
	path_t shaderParentPath = shaderAbsPath.parent_path();
	std::vector<const wchar_t*> extraIncludes = { commonAbsPath.c_str(), shaderParentPath.c_str() };
	return std::make_unique<RHI::Shader>(shaderAbsPath.c_str(), entrypoint, target, std::move(defines), std::move(extraIncludes));
#else
	// xxx Load from prebuilt shader blobs
#endif
}
