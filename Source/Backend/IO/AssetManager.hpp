#pragma once
#include "IO.hpp"
#include "../RHI/RHI.hpp"
#include "MeshAsset.hpp"
#include "MeshImporter.hpp"

namespace IO {	
	template<typename T> concept Cleanable = requires(T obj) { obj.clean(); };
	class AssetManager {
		entt::registry registry;
	public:
		template<typename T, typename Src> asset_handle load(Src& src) {
			static_assert(false || "Not implemented");			
		};		
		template<> asset_handle load<StaticMeshAsset>(mesh_static& mesh);

		template<typename T> void upload(RHI::Device* device, T* resource) {
			static_assert(false || "Not implemented");
		};
		template<> void upload<StaticMeshAsset>(RHI::Device* device, StaticMeshAsset* resource);

		template<typename T> void upload_all(RHI::Device* device) {
			for (auto& obj : registry.storage<T>())
				upload<T>(device, &obj);
		};		

		template<Cleanable T> void clean() {
			for (auto& obj : registry.storage<T>())
				obj.clean();
		}
		template<typename T> T& get(asset_handle resource) {
			return registry.get<T>(resource.resource);
		}

		template<typename T> T* try_get(asset_handle resource) {
			return registry.try_get<T>(resource.resource);
		}
	};
}