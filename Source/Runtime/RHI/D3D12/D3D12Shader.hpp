#pragma once
#include "D3D12Types.hpp"
namespace RHI {
	class Blob {
	protected:
		name_t m_Name;
		std::vector<unsigned char> m_Data;
	public:		
		Blob() {};
		Blob(void* data, size_t size);

		inline void* GetData() { return m_Data.data(); }
		inline size_t GetSize() { return m_Data.size(); }
		
		inline auto const& GetName() { return m_Name; }
		inline void SetName(name_t name) {
			m_Name = name;			
		}
	};
	class Shader : public Blob {
	public:
		Shader(const wchar_t* sourcePath, const wchar_t* entrypoint, const wchar_t* target);
	};
}