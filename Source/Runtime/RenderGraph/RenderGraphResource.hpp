#pragma once
#include "../RHI/RHI.hpp"
enum class RgResourceType {
	Unknown,
	Dummy,
	Buffer,
	Texture,
	ResourceViewSRV,
	ResourceViewUAV,
	ResourceViewRTV,
	ResourceViewDSV
};
enum class RgResourceFlag {
	Imported,
	Created
};

struct RgHandle {
	uint version;
	RgResourceType type;
	RgResourceFlag flag;
	entt::entity entity; // entity within RenderGraph's registry. indexes `rg_handle`. may index `rg_resource` deriviatives
	inline operator entt::entity() const { return entity; }
	bool is_imported() { return flag == RgResourceFlag::Imported; }

	friend bool operator==(const RgHandle& lhs, const RgHandle& rhs) { return lhs.entity == rhs.entity; }
};
template<> struct std::hash<RgHandle> {
	inline entt::entity operator()(const RgHandle& resource) const { return resource; }
};

struct RgResource {
	entt::entity entity;
};
struct RgBuffer : public RgResource {
	RHI::Resource::ResourceDesc desc;
};
struct RgTexture : public RgResource {
	RHI::Resource::ResourceDesc desc;
};
struct RgSRV : public RgResource {
	struct view_desc {
		RHI::ShaderResourceViewDesc viewDesc;
		entt::entity viewed; // the viewed resource		
	} desc;	
};
struct RgRTV : public RgResource {
	struct view_desc {
		RHI::RenderTargetViewDesc viewDesc;
		entt::entity viewed; // the viewed resource		
	} desc;
};
struct RgDSV : public RgResource {
	struct view_desc {
		RHI::DepthStencilViewDesc viewDesc;
		entt::entity viewed; // the viewed resource		
	} desc;
};
struct RgUAV : public RgResource {
	struct view_desc {
		RHI::UnorderedAccessViewDesc viewDesc;
		entt::entity viewed; // the viewed resource
		entt::entity viewedCounter = entt::tombstone; // UAV specialization : counter resource associated with `viewed`. usually (if any) is the resource itself
	} desc;
};
// Dummy placeholder. Useful for directing intransient resources / graph flow
struct Dummy {};
struct RgDummy : public RgResource {
	struct view_desc{
	} desc;
};

template<typename T> struct RgResourceTraits {
	static constexpr RgResourceType type_enum = RgResourceType::Unknown;
	using type = void;
	using rhi_type = void;
	using desc_type = void;
};

template<> struct RgResourceTraits<RHI::Texture> {
	static constexpr RgResourceType type_enum = RgResourceType::Texture;
	using type = RgTexture;
	using rhi_type = RHI::Texture;
	using desc_type = decltype(RgTexture::desc);
};

template<> struct RgResourceTraits<RHI::Buffer> {
	static constexpr RgResourceType type_enum = RgResourceType::Buffer;
	using type = RgBuffer;
	using rhi_type = RHI::Buffer;
	using desc_type = decltype(RgBuffer::desc);
};

template<> struct RgResourceTraits<RHI::ShaderResourceView> {
	static constexpr RgResourceType type_enum = RgResourceType::ResourceViewSRV;
	using type = RgSRV;
	using rhi_type = RHI::ShaderResourceView;
	using desc_type = decltype(RgSRV::desc);
};

template<> struct RgResourceTraits<RHI::RenderTargetView> {
	static constexpr RgResourceType type_enum = RgResourceType::ResourceViewRTV;
	using type = RgRTV;
	using rhi_type = RHI::RenderTargetView;
	using desc_type = decltype(RgRTV::desc);
};

template<> struct RgResourceTraits<RHI::DepthStencilView> {
	static constexpr RgResourceType type_enum = RgResourceType::ResourceViewDSV;
	using type = RgDSV;
	using rhi_type = RHI::DepthStencilView;
	using desc_type = decltype(RgDSV::desc);
};

template<> struct RgResourceTraits<RHI::UnorderedAccessView> {
	static constexpr RgResourceType type_enum = RgResourceType::ResourceViewUAV;
	using type = RgUAV;
	using rhi_type = RHI::UnorderedAccessView;
	using desc_type = decltype(RgUAV::desc);
};

template<> struct RgResourceTraits<Dummy> {
	static constexpr RgResourceType type_enum = RgResourceType::Dummy;
	using type = RgDummy;
	using rhi_type = Dummy;
	using desc_type = decltype(RgDummy::desc);
};
template<typename T> concept RgDefinedResource = !std::is_void_v<typename RgResourceTraits<T>::type>;