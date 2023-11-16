#pragma once
#include "../RHI/RHI.hpp"
enum class rg_resource_types {
	Unknown,
	Buffer,
	Texture,
	ResourceViewSRV,
	ResourceViewUAV,
	ResourceViewRTV,
	ResourceViewDSV
};
enum class rg_resource_flags {
	Imported,
	Created
};

struct rg_handle {
	uint version;
	rg_resource_types type;
	rg_resource_flags flag;
	entt::entity entity; // entity within RenderGraph's registry. indexes `rg_handle`. may index `rg_resource` deriviatives
	inline operator entt::entity() const { return entity; }
	bool is_imported() { return flag == rg_resource_flags::Imported; }
};
template<> struct std::hash<rg_handle> {
	inline entt::entity operator()(const rg_handle& resource) const { return resource; }
};

struct rg_resource {
	entt::entity entity;
};
struct rg_buffer : public rg_resource {
	RHI::Resource::ResourceDesc desc;
};
struct rg_texture : public rg_resource {
	RHI::Resource::ResourceDesc desc;
};
struct rg_srv : public rg_resource {
	struct view_desc {
		RHI::ShaderResourceViewDesc viewDesc;
		entt::entity viewed; // the viewed resource		
	} desc;	
};
struct rg_rtv : public rg_resource {
	struct view_desc {
		RHI::RenderTargetViewDesc viewDesc;
		entt::entity viewed; // the viewed resource		
	} desc;
};
struct rg_dsv : public rg_resource {
	struct view_desc {
		RHI::DepthStencilViewDesc viewDesc;
		entt::entity viewed; // the viewed resource		
	} desc;
};
struct rg_uav : public rg_resource {
	struct view_desc {
		RHI::UnorderedAccessViewDesc viewDesc;
		entt::entity viewed; // the viewed resource		
	} desc;
};

template<typename T> struct rg_resource_traits {
	static constexpr rg_resource_types type_enum = rg_resource_types::Unknown;
	using type = void;
	using rhi_type = void;
	using desc_type = void;
};

template<> struct rg_resource_traits<RHI::Texture> {
	static constexpr rg_resource_types type_enum = rg_resource_types::Texture;
	using type = rg_texture;
	using rhi_type = RHI::Texture;
	using desc_type = decltype(rg_texture::desc);
};

template<> struct rg_resource_traits<RHI::Buffer> {
	static constexpr rg_resource_types type_enum = rg_resource_types::Buffer;
	using type = rg_buffer;
	using rhi_type = RHI::Buffer;
	using desc_type = decltype(rg_buffer::desc);
};

template<> struct rg_resource_traits<RHI::ShaderResourceView> {
	static constexpr rg_resource_types type_enum = rg_resource_types::ResourceViewSRV;
	using type = rg_srv;
	using rhi_type = RHI::ShaderResourceView;
	using desc_type = decltype(rg_srv::desc);
};

template<> struct rg_resource_traits<RHI::RenderTargetView> {
	static constexpr rg_resource_types type_enum = rg_resource_types::ResourceViewRTV;
	using type = rg_rtv;
	using rhi_type = RHI::RenderTargetView;
	using desc_type = decltype(rg_rtv::desc);
};

template<> struct rg_resource_traits<RHI::DepthStencilView> {
	static constexpr rg_resource_types type_enum = rg_resource_types::ResourceViewDSV;
	using type = rg_dsv;
	using rhi_type = RHI::DepthStencilView;
	using desc_type = decltype(rg_dsv::desc);
};

template<> struct rg_resource_traits<RHI::UnorderedAccessView> {
	static constexpr rg_resource_types type_enum = rg_resource_types::ResourceViewUAV;
	using type = rg_uav;
	using rhi_type = RHI::UnorderedAccessView;
	using desc_type = decltype(rg_uav::desc);
};

template<typename T> concept rg_defined_resource = !std::is_void_v<typename rg_resource_traits<T>::type>;