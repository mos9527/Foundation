#pragma once
#include "../Common.hpp"
namespace RHI {
	class ShaderBlob : public RHIObject {
		using RHIObject::m_Name;
		std::vector<unsigned char> m_Data;
	public:
		ShaderBlob(const wchar_t* sourcePath, const char* entrypoint, const char* target, D3D_SHADER_MACRO* macro = NULL);
		ShaderBlob(void* csoData, size_t size);

		inline void* GetData() { return m_Data.data(); }
		inline size_t GetSize() { return m_Data.size(); }
		
		using RHIObject::GetName;
		inline void SetName(name_t name) {
			m_Name = name;			
		}
	};
}