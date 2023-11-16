#pragma once
#include "D3D12Types.hpp"
namespace RHI {
	class ShaderBlob {
		name_t m_Name;
		std::vector<unsigned char> m_Data;
	public:
		ShaderBlob(const wchar_t* sourcePath, const char* entrypoint, const char* target, D3D_SHADER_MACRO* macro = NULL);
		ShaderBlob(void* csoData, size_t size);

		inline void* GetData() { return m_Data.data(); }
		inline size_t GetSize() { return m_Data.size(); }
		
		inline auto const& GetName() { return m_Name; }
		inline void SetName(name_t name) {
			m_Name = name;			
		}
	};
}