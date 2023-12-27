#pragma once
#include "../../Runtime/RHI/RHI.hpp"
#include "../../Common/IO.hpp"
inline std::wstring ExpandShaderPath(const wchar_t* sourceName) {	
	return std::format(L"{}/{}.hlsl", WSTR_PREFIX(SHADER_DIR), sourceName);
}
inline std::unique_ptr<RHI::Shader> BuildShader(const wchar_t* sourceName, const wchar_t* entrypoint, const wchar_t* target, std::vector<const wchar_t*>&& defines = {}) {
	path_t shaderAbsPath = ExpandShaderPath(sourceName);
	path_t commonAbsPath = std::format(L"{}/Common/", WSTR_PREFIX(SOURCE_DIR));
	path_t shaderParentPath = WSTR_PREFIX(SHADER_DIR);
	std::vector<const wchar_t*> extraIncludes = { commonAbsPath.c_str(), shaderParentPath.c_str() };
	return std::make_unique<RHI::Shader>(shaderAbsPath.c_str(), entrypoint, target, std::move(defines), std::move(extraIncludes));
}
