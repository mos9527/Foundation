#pragma once
#include "../../Common/IO.hpp"
#include "../../Runtime/RHI/RHI.hpp"
#include "../Shaders/Shaders.hpp"
struct IRenderPass {
private:
	std::vector<std::filesystem::file_time_type> loadedShaderTimestamps;
public:
	const char* const name;
	const char* const desc;	
	const std::vector<std::wstring> shaderSources;

	RHI::Device* const device;

	IRenderPass(
		RHI::Device* device, 
		std::vector<std::wstring>&& shaderSources,
		const char* name = "<to be filled>", 
		const char* desc = "<to be filled>"
	) : device(device), shaderSources(shaderSources), loadedShaderTimestamps(shaderSources.size()), name(name), desc(desc) {};
	virtual void reset() = 0;
	inline bool is_shader_uptodate(uint sourceIndex) {
		auto fullname = ::ExpandShaderPath(shaderSources[sourceIndex].c_str());
		return loadedShaderTimestamps[sourceIndex] == std::filesystem::last_write_time(fullname);
	}
	inline bool is_all_shader_uptodate() {
		for (uint i = 0; i < shaderSources.size(); i++)
			if (!is_shader_uptodate(i)) return false;
		return true;
	}
protected:	
	inline std::unique_ptr<RHI::Shader> build_shader(uint sourceIndex, const wchar_t* entrypoint, const wchar_t* target, std::vector<const wchar_t*>&& defines = {}) {
		CHECK(sourceIndex < shaderSources.size() && "Source index OOB");
		auto fullname = ::ExpandShaderPath(shaderSources[sourceIndex].c_str());
		auto ptr = ::BuildShader(shaderSources[sourceIndex].c_str(), entrypoint, target, std::move(defines));
		loadedShaderTimestamps[sourceIndex] = std::filesystem::last_write_time(fullname);
		LOG(INFO) << "Built shader " << wstring_to_utf8(shaderSources[sourceIndex].c_str()) << " @ " << loadedShaderTimestamps[sourceIndex].time_since_epoch();
		return ptr;
	}
};
