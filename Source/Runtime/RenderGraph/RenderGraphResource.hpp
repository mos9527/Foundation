#pragma once
#include "../RHI/RHI.hpp"
enum class RgResourceType {
	Unknown = 0,
	Dummy = 1,
	Buffer = 2,
	Texture = 3,
	ResourceView = 0xf0,
	ResourceViewSRV = 0xf1,
	ResourceViewUAV = 0xf2,
	ResourceViewRTV = 0xf3,
	ResourceViewDSV = 0xf4
};
DEFINE_ENUM_FLAG_OPERATORS(RgResourceType);
enum class RgResourceFlag {
	Imported,
	Created
};

struct RgHandle {
	uint version;
	RgResourceType type; 
	entt::entity entity = entt::tombstone; // entity within RenderGraph's registry. indexes `RgHandle`. may index `RgResource`
	bool imported = false;

	inline operator entt::entity() const { return entity; }
	// since each entity has only one assigned type, we don't compare type since entity comparision will suffice
	// this also applies to the hash function
	inline uint64_t hash() const { return entt::to_integral(entity) | ((uint64_t)version << 32); }
	friend bool operator<(const RgHandle& lhs, const RgHandle& rhs) { return lhs.hash() < rhs.hash(); }
	friend bool operator==(const RgHandle& lhs, const RgHandle& rhs) { return lhs.hash() == rhs.hash(); }

	RgHandle(const RgHandle&) = delete;
};
template<> struct std::hash<RgHandle> {
	inline uint64_t operator()(const RgHandle& resource) const { return resource.hash(); }
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
	using resource_type = void;
	using rhi_type = void;
	using desc_type = void;
};

template<> struct RgResourceTraits<RHI::Texture> {
	static constexpr RgResourceType type_enum = RgResourceType::Texture;
	using resource_type = RgTexture;
	using rhi_type = RHI::Texture;
	using desc_type = decltype(RgTexture::desc);
};

template<> struct RgResourceTraits<RHI::Buffer> {
	static constexpr RgResourceType type_enum = RgResourceType::Buffer;
	using resource_type = RgBuffer;
	using rhi_type = RHI::Buffer;
	using desc_type = decltype(RgBuffer::desc);
};

template<> struct RgResourceTraits<RHI::ShaderResourceView> {
	static constexpr RgResourceType type_enum = RgResourceType::ResourceViewSRV;
	using resource_type = RgSRV;
	using rhi_type = RHI::ShaderResourceView;
	using desc_type = decltype(RgSRV::desc);
};

template<> struct RgResourceTraits<RHI::RenderTargetView> {
	static constexpr RgResourceType type_enum = RgResourceType::ResourceViewRTV;
	using resource_type = RgRTV;
	using rhi_type = RHI::RenderTargetView;
	using desc_type = decltype(RgRTV::desc);
};

template<> struct RgResourceTraits<RHI::DepthStencilView> {
	static constexpr RgResourceType type_enum = RgResourceType::ResourceViewDSV;
	using resource_type = RgDSV;
	using rhi_type = RHI::DepthStencilView;
	using desc_type = decltype(RgDSV::desc);
};

template<> struct RgResourceTraits<RHI::UnorderedAccessView> {
	static constexpr RgResourceType type_enum = RgResourceType::ResourceViewUAV;
	using resource_type = RgUAV;
	using rhi_type = RHI::UnorderedAccessView;
	using desc_type = decltype(RgUAV::desc);
};

template<> struct RgResourceTraits<Dummy> {
	static constexpr RgResourceType type_enum = RgResourceType::Dummy;
	using resource_type = RgDummy;
	using rhi_type = Dummy;
	using desc_type = decltype(RgDummy::desc);
};
template<typename T> concept RgDefinedResource = !std::is_void_v<typename RgResourceTraits<T>::resource_type>;