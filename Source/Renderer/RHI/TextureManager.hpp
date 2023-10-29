#pragma once
#include "RHI.hpp"
#include "../../IO/Image.hpp"
#define MAX_NUM_TEXTURES 0xffff
namespace RHI {
	class TextureManager{
	public:
		TextureManager();
		
		handle_type LoadTexture(Device* device, CommandList* cmdList, IO::Bitmap8bpp& bmp);
		
		inline Texture* GetTexture(handle_type handle) { return m_Textures[handle].get(); }
		inline DescriptorHandle GetTextureSRV(handle_type handle) { return m_TextureDescriptors[handle]; }

		void Free(handle_type);
		inline bool CheckHandle(handle_type handle) { 
			return handle < m_Textures.size() && 
				m_Textures[handle].get() != nullptr;
		};
		inline handle_type GetMaxHandle() { return m_Textures.size(); };

	private:
		std::vector<std::unique_ptr<Texture>> m_Textures;
		std::vector<DescriptorHandle> m_TextureDescriptors;
		HandleQueue m_HandleQueue;
	};
}